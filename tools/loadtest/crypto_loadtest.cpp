// SPDX-License-Identifier: Apache-2.0
//
// tools/loadtest/crypto_loadtest.cpp — single-process throughput harness for
// security/crypto and security/keys.
//
// WHAT "LOAD" MEANS FOR A CRYPTO LAYER, AND WHAT IT DOES NOT. There is no
// server here and no multi-tenancy, so simulating "many users" would be
// fiction. The real question a device answers is single-process throughput:
// how many documents per second can one device's encryption layer seal, how
// does that scale with document size, and where does it stop. That is what
// this measures.
//
// IT ALSO MEASURES TWO THINGS A THROUGHPUT NUMBER WOULD HIDE:
//
//   1. The Argon2id MEMORY FLOOR, per KdfProfile. crypto.cpp pins
//      MobileInteractive at 64 MiB, DesktopModerate at libsodium's MODERATE
//      (256 MiB) and ArchivalSensitive at SENSITIVE (1 GiB). Those are
//      reservations, not estimates, so each profile is a separate subcommand
//      and therefore a separate process: peak RSS is a per-process high-water
//      mark, and letting the 1 GiB profile share a process with the others
//      would report one number and attribute it to nothing.
//
//   2. The GUARDED-ALLOCATOR ceiling. Every live key is a SecureBytes, which
//      is sodium_malloc: guard pages plus mlock. mlock is a finite,
//      per-process and per-system resource, so "how many keys can be live at
//      once" is a real capacity limit that no per-operation latency reveals.
//      The `live-keys` subcommand holds keys simultaneously rather than in a
//      loop, which is the only way to find it.
//
// Subcommands (one process each, deliberately):
//   aead        AEAD seal/open across message sizes, plus the 8 MiB cap
//   docs        4 KiB document seal/open at 10 .. 100 000 operations
//   subkey      derive_subkey (HKDF-shaped, not a password KDF) at scale
//   hierarchy   security/keys: wrap/unwrap and per-document key derivation
//   live-keys   how many keys sodium_malloc will keep live at once
//   kdf-mobile | kdf-moderate | kdf-sensitive
//               Argon2id latency AND peak RSS for one profile

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "loadtest_util.h"

#include "uapp/crypto/crypto.h"
#include "uapp/crypto/secure_bytes.h"
#include "uapp/keys/key_hierarchy.h"
#include "uapp/status.h"

namespace {

using uapp::Status;
using uapp::is_ok;
using uapp::loadtest::Timer;
using uapp::loadtest::current_rss_mib;
using uapp::loadtest::die;
using uapp::loadtest::emit_row;
using uapp::loadtest::note;
using uapp::loadtest::peak_rss_mib;
using uapp::loadtest::status_text;

namespace cr = uapp::crypto;

[[nodiscard]] cr::ByteView view(const std::vector<std::uint8_t>& v) {
    return cr::ByteView{v.data(), v.size()};
}

[[nodiscard]] cr::ByteView view(const char* s) {
    return cr::ByteView{reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)};
}

// ---------------------------------------------------------------------------
// AEAD across message sizes
// ---------------------------------------------------------------------------

void run_aead() {
    note("== crypto: aead ==");

    cr::ContentKey key;
    if (const Status s = cr::generate_content_key(key); !is_ok(s)) { die("content key", s); }

    const std::size_t sizes[] = {
        1u * 1024u,
        16u * 1024u,
        256u * 1024u,
        1u * 1024u * 1024u,
        cr::kMaxPlaintextBytes,          // 8 MiB, the documented cap
        cr::kMaxPlaintextBytes + 1u,     // must be a Status, not an overrun
    };

    const std::vector<std::uint8_t> aad(64, 0xA5u);

    for (const std::size_t size : sizes) {
        std::vector<std::uint8_t> plain(size, 0x5Au);

        // Enough iterations to move ~64 MiB at small sizes, but never so many
        // that an 8 MiB message takes minutes.
        long long ops = static_cast<long long>((64u * 1024u * 1024u) / size);
        if (ops < 10) { ops = 10; }
        if (ops > 2000) { ops = 2000; }
        const bool over = size > cr::kMaxPlaintextBytes;
        if (over) { ops = 1; }

        cr::SealedBlob blob;
        Timer t;
        Status seal_status = Status::Ok;
        long long sealed   = 0;
        for (long long i = 0; i < ops; ++i) {
            seal_status = cr::seal(key, view(plain), view(aad), blob);
            if (!is_ok(seal_status)) { break; }
            ++sealed;
        }
        const double seal_ms = t.ms();

        char detail[224];
        if (over) {
            std::snprintf(detail, sizeof detail, "%s bytes=%zu",
                          status_text(seal_status).c_str(), size);
            emit_row("crypto", "limit_max_plaintext_bytes", static_cast<long long>(size), 1,
                     seal_ms,
                     seal_status == Status::CryptoMessageTooLarge ? "limit" : "FAIL", detail);
            continue;
        }
        if (!is_ok(seal_status)) {
            emit_row("crypto", "seal", static_cast<long long>(size), sealed, seal_ms, "FAIL",
                     status_text(seal_status).c_str());
            continue;
        }
        const double mib = (static_cast<double>(size) * static_cast<double>(sealed)) /
                           (1024.0 * 1024.0);
        std::snprintf(detail, sizeof detail, "MiB_per_s=%.1f ct_bytes=%zu",
                      seal_ms > 0.0 ? mib / (seal_ms / 1000.0) : 0.0, blob.ciphertext.size());
        emit_row("crypto", "seal", static_cast<long long>(size), sealed, seal_ms, "ok", detail);

        // --- open, with a verified round trip -----------------------------
        bool verified = false;
        t.reset();
        Status open_status = Status::Ok;
        long long opened   = 0;
        for (long long i = 0; i < ops; ++i) {
            cr::SecureBytes out;
            open_status = cr::open(key, blob, view(aad), out);
            if (!is_ok(open_status)) { break; }
            if (i == 0) {
                verified = out.size() == size &&
                           std::memcmp(out.data(), plain.data(), size) == 0;
            }
            ++opened;
        }
        const double open_ms = t.ms();
        if (!is_ok(open_status)) {
            emit_row("crypto", "open", static_cast<long long>(size), opened, open_ms, "FAIL",
                     status_text(open_status).c_str());
            continue;
        }
        std::snprintf(detail, sizeof detail, "MiB_per_s=%.1f roundtrip_verified=%d",
                      open_ms > 0.0 ? mib / (open_ms / 1000.0) : 0.0, verified ? 1 : 0);
        emit_row("crypto", "open", static_cast<long long>(size), opened, open_ms,
                 verified ? "ok" : "FAIL", detail);
    }

    // --- tamper detection must hold at every size, not just in unit tests -
    {
        std::vector<std::uint8_t> plain(4096, 0x11u);
        cr::SealedBlob blob;
        if (const Status s = cr::seal(key, view(plain), cr::no_aad(), blob); !is_ok(s)) {
            die("seal for tamper", s);
        }
        blob.ciphertext[blob.ciphertext.size() / 2] ^= 0x01u;
        cr::SecureBytes out;
        Timer t;
        const Status s  = cr::open(key, blob, cr::no_aad(), out);
        const double ms = t.ms();
        emit_row("crypto", "tamper_rejected", 4096, 1, ms,
                 s == Status::CryptoAuthFailed ? "ok" : "FAIL", status_text(s).c_str());
    }
}

// ---------------------------------------------------------------------------
// Document-sized throughput ladder
// ---------------------------------------------------------------------------

void run_docs() {
    note("== crypto: docs ==");

    cr::ContentKey key;
    if (const Status s = cr::generate_content_key(key); !is_ok(s)) { die("content key", s); }

    constexpr std::size_t kDocBytes = 4096;
    const std::vector<std::uint8_t> plain(kDocBytes, 0x3Cu);

    for (std::size_t si = 0; si < uapp::loadtest::kScaleCount; ++si) {
        const long long n = uapp::loadtest::kScales[si];
        cr::SealedBlob blob;
        Timer t;
        Status err      = Status::Ok;
        long long done  = 0;
        for (long long i = 0; i < n; ++i) {
            err = cr::seal(key, view(plain), cr::no_aad(), blob);
            if (!is_ok(err)) { break; }
            ++done;
        }
        const double ms = t.ms();
        char detail[160];
        std::snprintf(detail, sizeof detail, "docs_per_s=%.0f",
                      ms > 0.0 ? static_cast<double>(done) / (ms / 1000.0) : 0.0);
        emit_row("crypto", "seal_4KiB_docs", n, done, ms,
                 done == n ? "ok" : "FAIL", done == n ? detail : status_text(err).c_str());

        t.reset();
        done = 0;
        for (long long i = 0; i < n; ++i) {
            cr::SecureBytes out;
            err = cr::open(key, blob, cr::no_aad(), out);
            if (!is_ok(err)) { break; }
            ++done;
        }
        const double open_ms = t.ms();
        std::snprintf(detail, sizeof detail, "docs_per_s=%.0f",
                      open_ms > 0.0 ? static_cast<double>(done) / (open_ms / 1000.0) : 0.0);
        emit_row("crypto", "open_4KiB_docs", n, done, open_ms,
                 done == n ? "ok" : "FAIL", done == n ? detail : status_text(err).c_str());
    }
}

// ---------------------------------------------------------------------------
// derive_subkey — the KDF that is NOT a password KDF
// ---------------------------------------------------------------------------

void run_subkey() {
    note("== crypto: subkey ==");

    cr::ContentKey parent;
    if (const Status s = cr::generate_content_key(parent); !is_ok(s)) { die("parent key", s); }

    for (std::size_t si = 0; si < uapp::loadtest::kScaleCount; ++si) {
        const long long n = uapp::loadtest::kScales[si];
        Timer t;
        Status err     = Status::Ok;
        long long done = 0;
        for (long long i = 0; i < n; ++i) {
            char info[32];
            std::snprintf(info, sizeof info, "doc-%lld", i);
            cr::ContentKey child;
            err = cr::derive_subkey(parent, view("uapp/loadtest/v1/subkey"), view(info), child);
            if (!is_ok(err)) { break; }
            ++done;
        }
        const double ms = t.ms();
        char detail[160];
        std::snprintf(detail, sizeof detail, "derivations_per_s=%.0f",
                      ms > 0.0 ? static_cast<double>(done) / (ms / 1000.0) : 0.0);
        emit_row("crypto", "derive_subkey", n, done, ms, done == n ? "ok" : "FAIL",
                 done == n ? detail : status_text(err).c_str());
    }
}

// ---------------------------------------------------------------------------
// security/keys — the real hierarchy
// ---------------------------------------------------------------------------

void run_hierarchy() {
    note("== keys: hierarchy ==");

    namespace ky = uapp::keys;

    ky::RecoverySecret secret;
    ky::UserRootKey    urk;
    ky::WorkspaceKey   wsk;

    Timer t;
    if (const Status s = ky::generate_recovery_secret(secret); !is_ok(s)) {
        die("generate_recovery_secret", s);
    }
    if (const Status s = ky::derive_user_root_key(secret, urk); !is_ok(s)) {
        die("derive_user_root_key", s);
    }
    if (const Status s = ky::generate_workspace_key(wsk); !is_ok(s)) {
        die("generate_workspace_key", s);
    }
    emit_row("keys", "bootstrap_hierarchy", 1, 3, t.ms(), "ok",
             "recovery_secret+urk+workspace_key");

    // --- device wrap / unwrap (X25519 + AEAD) at scale --------------------
    cr::DeviceKeyPair device_a;
    cr::DeviceKeyPair device_b;
    if (const Status s = cr::generate_device_keypair(device_a); !is_ok(s)) {
        die("device keypair a", s);
    }
    if (const Status s = cr::generate_device_keypair(device_b); !is_ok(s)) {
        die("device keypair b", s);
    }

    const long long wrap_scales[] = {10, 100, 1000, 10000};
    for (const long long n : wrap_scales) {
        ky::DeviceWrappedUrkRecord record;
        t.reset();
        Status err     = Status::Ok;
        long long done = 0;
        for (long long i = 0; i < n; ++i) {
            err = ky::wrap_urk_for_device(device_a, device_b.public_key, urk, record);
            if (!is_ok(err)) { break; }
            ++done;
        }
        const double wrap_ms = t.ms();
        emit_row("keys", "wrap_urk_for_device", n, done, wrap_ms, done == n ? "ok" : "FAIL",
                 done == n ? "" : status_text(err).c_str());

        t.reset();
        done = 0;
        bool verified = false;
        for (long long i = 0; i < n; ++i) {
            ky::UserRootKey recovered;
            err = ky::unwrap_urk_from_device(device_b, device_a.public_key, record, recovered);
            if (!is_ok(err)) { break; }
            if (i == 0) {
                verified = recovered.valid() &&
                           cr::secure_equal(recovered.material.bytes(), urk.material.bytes(),
                                            cr::kContentKeyBytes);
            }
            ++done;
        }
        const double unwrap_ms = t.ms();
        emit_row("keys", "unwrap_urk_from_device", n, done, unwrap_ms,
                 (done == n && verified) ? "ok" : "FAIL",
                 done == n ? "urk_identical=1" : status_text(err).c_str());
    }

    // --- per-document key derivation + content seal, at scale -------------
    ky::UrkWrappedWorkspaceKeyRecord wrapped_wsk;
    if (const Status s = ky::wrap_workspace_key(urk, view("workspace-0001"), wsk, wrapped_wsk);
        !is_ok(s)) {
        die("wrap_workspace_key", s);
    }
    ky::WorkspaceKey reopened;
    if (const Status s =
            ky::unwrap_workspace_key(urk, view("workspace-0001"), wrapped_wsk, reopened);
        !is_ok(s)) {
        die("unwrap_workspace_key", s);
    }

    const std::vector<std::uint8_t> doc(4096, 0x7Eu);
    for (std::size_t si = 0; si < uapp::loadtest::kScaleCount; ++si) {
        const long long n = uapp::loadtest::kScales[si];

        t.reset();
        Status err     = Status::Ok;
        long long done = 0;
        for (long long i = 0; i < n; ++i) {
            char doc_id[48];
            std::snprintf(doc_id, sizeof doc_id, "document-%010lld", i);
            ky::DocumentKey dk;
            err = ky::derive_document_key(reopened, view(doc_id), dk);
            if (!is_ok(err)) { break; }
            ++done;
        }
        const double derive_ms = t.ms();
        char detail[160];
        std::snprintf(detail, sizeof detail, "keys_per_s=%.0f",
                      derive_ms > 0.0 ? static_cast<double>(done) / (derive_ms / 1000.0) : 0.0);
        emit_row("keys", "derive_document_key", n, done, derive_ms, done == n ? "ok" : "FAIL",
                 done == n ? detail : status_text(err).c_str());

        // The full per-document path a sync layer would run: derive the key
        // for this document id, then seal its content under it.
        t.reset();
        done = 0;
        for (long long i = 0; i < n; ++i) {
            char doc_id[48];
            std::snprintf(doc_id, sizeof doc_id, "document-%010lld", i);
            ky::DocumentKey dk;
            err = ky::derive_document_key(reopened, view(doc_id), dk);
            if (!is_ok(err)) { break; }
            cr::SealedBlob blob;
            err = ky::seal_document_content(dk, view(doc_id), view(doc), blob);
            if (!is_ok(err)) { break; }
            ++done;
        }
        const double seal_ms = t.ms();
        std::snprintf(detail, sizeof detail, "documents_per_s=%.0f",
                      seal_ms > 0.0 ? static_cast<double>(done) / (seal_ms / 1000.0) : 0.0);
        emit_row("keys", "derive_and_seal_4KiB_document", n, done, seal_ms,
                 done == n ? "ok" : "FAIL", done == n ? detail : status_text(err).c_str());
    }
}

// ---------------------------------------------------------------------------
// The guarded-allocator ceiling
// ---------------------------------------------------------------------------

/// Every live key sits in sodium_malloc memory: guard pages plus mlock. mlock
/// is finite, so "how many keys can be LIVE simultaneously" is a capacity
/// limit that no per-operation benchmark can show. Allocating in a loop and
/// freeing each one would never find it; this holds them.
void run_live_keys() {
    note("== crypto: live-keys ==");

    const long long scales[] = {10, 100, 1000, 10000, 100000};
    for (const long long n : scales) {
        std::vector<cr::ContentKey> held;
        held.reserve(static_cast<std::size_t>(n));
        const double rss_before = current_rss_mib();
        Timer t;
        Status err     = Status::Ok;
        long long done = 0;
        for (long long i = 0; i < n; ++i) {
            cr::ContentKey k;
            err = cr::generate_content_key(k);
            if (!is_ok(err)) { break; }
            held.push_back(std::move(k));
            ++done;
        }
        const double ms         = t.ms();
        const double rss_after  = current_rss_mib();
        char detail[224];
        std::snprintf(detail, sizeof detail,
                      "live=%lld rss_before=%.2f rss_after=%.2f bytes_per_key=%.0f%s%s",
                      done, rss_before, rss_after,
                      done > 0 ? ((rss_after - rss_before) * 1024.0 * 1024.0) /
                                     static_cast<double>(done)
                               : 0.0,
                      done == n ? "" : " first_failure=",
                      done == n ? "" : status_text(err).c_str());
        emit_row("crypto", "live_keys_held", n, done, ms,
                 done == n ? "ok" : "limit", detail);
        if (done != n) { break; }
    }
}

// ---------------------------------------------------------------------------
// Argon2id, one profile per process
// ---------------------------------------------------------------------------

void run_kdf(cr::KdfProfile profile, const char* phase) {
    note("== crypto: %s ==", phase);

    unsigned long long opslimit = 0;
    std::size_t        memlimit = 0;
    cr::kdf_parameters(profile, opslimit, memlimit);
    note("  opslimit=%llu memlimit=%zu MiB", opslimit, memlimit / (1024u * 1024u));

    cr::Salt salt{};
    if (const Status s = cr::generate_salt(salt); !is_ok(s)) { die("generate_salt", s); }

    const char* passphrase = "correct horse battery staple";

    // Three derivations, because the first one pays page-fault costs the next
    // two do not, and a single sample would hide that.
    constexpr int kReps = 3;
    cr::ContentKey out;
    Timer t;
    Status err     = Status::Ok;
    long long done = 0;
    for (int i = 0; i < kReps; ++i) {
        err = cr::derive_key_from_passphrase(view(passphrase), salt, profile, out);
        if (!is_ok(err)) { break; }
        ++done;
    }
    const double ms = t.ms();

    char detail[256];
    std::snprintf(detail, sizeof detail,
                  "opslimit=%llu memlimit_MiB=%zu peak_rss_MiB=%.1f%s%s", opslimit,
                  memlimit / (1024u * 1024u), peak_rss_mib(), done == kReps ? "" : " ",
                  done == kReps ? "" : status_text(err).c_str());
    emit_row("crypto", phase, static_cast<long long>(memlimit / (1024u * 1024u)), done, ms,
             done == kReps ? "ok" : "FAIL", detail);
}

void print_usage() {
    std::fputs(
        "uapp-loadtest-crypto — throughput harness for security/crypto + security/keys\n"
        "\n"
        "Usage:\n"
        "  uapp-loadtest-crypto <aead|docs|subkey|hierarchy|live-keys|\n"
        "                        kdf-mobile|kdf-moderate|kdf-sensitive>\n"
        "\n"
        "One subcommand per process on purpose: peak RSS is per-process, and the\n"
        "Argon2id profiles reserve 64 MiB / 256 MiB / 1 GiB respectively.\n",
        stderr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        print_usage();
        return 2;
    }

    if (const Status s = cr::init(); !is_ok(s)) { die("crypto::init", s); }

    uapp::loadtest::emit_csv_header();
    note("loadtest-crypto: libsodium %s", cr::library_version());

    const char* what = argv[1];
    if (std::strcmp(what, "aead") == 0) {
        run_aead();
    } else if (std::strcmp(what, "docs") == 0) {
        run_docs();
    } else if (std::strcmp(what, "subkey") == 0) {
        run_subkey();
    } else if (std::strcmp(what, "hierarchy") == 0) {
        run_hierarchy();
    } else if (std::strcmp(what, "live-keys") == 0) {
        run_live_keys();
    } else if (std::strcmp(what, "kdf-mobile") == 0) {
        run_kdf(cr::KdfProfile::MobileInteractive, "kdf_mobile_interactive");
    } else if (std::strcmp(what, "kdf-moderate") == 0) {
        run_kdf(cr::KdfProfile::DesktopModerate, "kdf_desktop_moderate");
    } else if (std::strcmp(what, "kdf-sensitive") == 0) {
        run_kdf(cr::KdfProfile::ArchivalSensitive, "kdf_archival_sensitive");
    } else {
        print_usage();
        return 2;
    }

    note("loadtest-crypto: done. process peak RSS = %.2f MiB", peak_rss_mib());
    return 0;
}
