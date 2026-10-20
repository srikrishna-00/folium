#include "auth.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace uapp::api {

namespace {

// 32 bytes of randomness from std::random_device, hex-encoded. Not routed
// through security/crypto deliberately: this file must work even in a
// build with UAPP_CRYPTO_ENABLED=OFF (the CI "minimal" configuration has
// no libsodium at all), and a locally-scoped bearer token for a
// self-hosted single-user server does not carry the same stakes as a
// content-encryption key — std::random_device, seeded from the OS's own
// entropy source on every platform this project targets, is a reasonable,
// dependency-free choice for this specific, narrow purpose.
std::string generate_token() {
    std::random_device rd;
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        const unsigned byte = rd() & 0xFFu;
        oss << std::hex << (byte < 16 ? "0" : "") << byte;
    }
    return oss.str();
}

}  // namespace

std::optional<std::string> load_or_create_token(const std::string& token_path) {
    {
        std::ifstream in(token_path);
        if (in.good()) {
            std::string token;
            std::getline(in, token);
            if (!token.empty()) {
                return token;
            }
        }
    }

    const std::string token = generate_token();
    std::ofstream     out(token_path, std::ios::trunc);
    if (!out.good()) {
        return std::nullopt;
    }
    out << token << '\n';
    out.close();

#if defined(__unix__) || defined(__APPLE__)
    // 0600: this file is a credential. Best-effort — a failure here does
    // not invalidate the token itself, so it is not treated as fatal.
    chmod(token_path.c_str(), S_IRUSR | S_IWUSR);
#endif

    return token;
}

bool bearer_token_matches(const std::string& header_value, const std::string& token) noexcept {
    static constexpr const char* kPrefix = "Bearer ";
    const std::size_t            prefix_len = 7;  // strlen("Bearer ")
    if (header_value.size() != prefix_len + token.size()) {
        return false;
    }
    if (header_value.compare(0, prefix_len, kPrefix) != 0) {
        return false;
    }
    // Constant-time compare over the token portion only. `diff` is `unsigned`
    // rather than `unsigned char` so that `|=`'s usual-arithmetic-conversions
    // result (an int) never narrows on assignment — GCC's -Wconversion flags
    // that narrowing under -Werror even though it is provably value-safe
    // here; Clang does not flag it, so this was only caught building inside
    // a real Ubuntu/GCC container.
    unsigned diff = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(header_value[prefix_len + i]) ^
                                      static_cast<unsigned char>(token[i]));
    }
    return diff == 0;
}

}  // namespace uapp::api
