// SPDX-License-Identifier: Apache-2.0
//
// core/capi/src/capi_internal.h — private to libuapp_capi.
//
// Nothing in here is installed or visible to a binding. It holds the three
// things every exported function needs: the unwind guard, the handle-validity
// check, and the Status -> uapp_status_t mapping.

#ifndef UAPP_CAPI_SRC_CAPI_INTERNAL_H
#define UAPP_CAPI_SRC_CAPI_INTERNAL_H

#include <cstdint>
#include <cstring>
#include <new>

#include "uapp/capi/uapp_capi.h"
#include "uapp/status.h"

// ---------------------------------------------------------------------------
// The unwind guard.
//
// ADR-0001 disables exceptions core-wide and cmake/UappCompilerOptions.cmake
// enforces it with -fno-exceptions, so under the shipping build these macros
// expand to a bare block and cost exactly nothing.
//
// They are still here, and they are not theatre. The C boundary is the one
// place where "the flag is set" is not a sufficient argument:
//
//   * A future engine (PDFium, an OCR backend, a vendored parser) may be
//     compiled with exceptions on and linked into this library. The flag is
//     per translation unit, not per program.
//   * Unwinding out of a C frame into JNI or Swift is undefined behaviour and
//     manifests as an unreadable native crash on a user's device.
//
// So the discipline lives in the source: if this TU is ever built with
// exceptions enabled, every exported function converts an escaping exception
// into UAPP_STATUS_UNEXPECTED at the boundary instead of unwinding through it.
// ---------------------------------------------------------------------------

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || \
    (defined(_MSC_VER) && defined(_CPPUNWIND))
#  define UAPP_CAPI_EXCEPTIONS_ENABLED 1
#else
#  define UAPP_CAPI_EXCEPTIONS_ENABLED 0
#endif

#if UAPP_CAPI_EXCEPTIONS_ENABLED
#  define UAPP_CAPI_GUARD_BEGIN try {
#  define UAPP_CAPI_GUARD_END \
      }                       \
      catch (...) { return UAPP_STATUS_UNEXPECTED; }
// Destructors return void, so there is nothing to report; swallowing is the
// only option that is not worse. They are written so this cannot trigger.
#  define UAPP_CAPI_GUARD_END_VOID \
      }                            \
      catch (...) { return; }
// For the handful of exported functions that are pure lookups and therefore
// return a value rather than a status — uapp_block_type_name(),
// uapp_block_type_is_implemented(), uapp_source_kind_name(). They cannot fail,
// so there is no status to carry a failure on; the fallback is the same answer
// they already give for an input they do not recognise, which keeps the "never
// NULL" promise in the public headers true even on a path that cannot be taken.
#  define UAPP_CAPI_GUARD_END_FALLBACK(fallback) \
      }                                          \
      catch (...) { return (fallback); }
#else
#  define UAPP_CAPI_GUARD_BEGIN {
#  define UAPP_CAPI_GUARD_END }
#  define UAPP_CAPI_GUARD_END_VOID }
#  define UAPP_CAPI_GUARD_END_FALLBACK(fallback) }
#endif

namespace uapp::capi {

/// Map a core Status onto the ABI enum.
///
/// Written as an exhaustive switch rather than a cast even though the numeric
/// values are identical (and asserted identical in status.cpp): a cast would
/// silently forward a code this layer has never heard of, and a binding would
/// then surface a number with no documented meaning.
[[nodiscard]] uapp_status_t to_c_status(Status s) noexcept;

// --- handle magic cookies --------------------------------------------------
//
// One per handle type, first member of the struct. Checking it catches a NULL
// handle, a zeroed or uninitialised one, and — because the values differ — a
// handle of the wrong type passed to the wrong function, which is the mistake
// a generated binding is most likely to make.
//
// It is defence in depth. It cannot detect use-after-free, and this header
// does not pretend otherwise.

inline constexpr std::uint32_t kServiceMagic = 0x55415053u;  // 'UAPS'
inline constexpr std::uint32_t kNoteMagic    = 0x5541504Eu;  // 'UAPN'
inline constexpr std::uint32_t kResultsMagic = 0x55415052u;  // 'UAPR'

// ABI 0.2 added two surfaces and therefore four more handle types. The values
// stay distinct from the three above for exactly the reason the comment gives:
// the mistake a generated binding makes is passing the wrong opaque pointer,
// and on the Kotlin/Swift side every handle is an indistinguishable integer.
inline constexpr std::uint32_t kDocServiceMagic    = 0x55415044u;  // 'UAPD'
inline constexpr std::uint32_t kPageMagic          = 0x55415047u;  // 'UAPG'
inline constexpr std::uint32_t kSearchIndexMagic   = 0x55415049u;  // 'UAPI'
inline constexpr std::uint32_t kUnifiedResultMagic = 0x55415055u;  // 'UAPU'

/// Duplicate a NUL-terminated string into a buffer owned by the caller and
/// released by uapp_free_string(). Returns nullptr on allocation failure.
///
/// Uses new[] (std::nothrow) so that allocation failure is a return value
/// rather than a terminate() under -fno-exceptions, and so that the allocator
/// that frees it is provably the one that allocated it — the host's malloc and
/// ours may not be the same, which is the whole reason uapp_free_string exists.
[[nodiscard]] char* dup_string(const char* s, std::size_t length) noexcept;

}  // namespace uapp::capi

#endif  // UAPP_CAPI_SRC_CAPI_INTERNAL_H
