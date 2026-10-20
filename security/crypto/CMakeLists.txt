add_library(uapp_security_crypto STATIC
    src/crypto.cpp
    src/secure_bytes.cpp)
add_library(uapp::security_crypto ALIAS uapp_security_crypto)

target_include_directories(uapp_security_crypto PUBLIC include)

# uapp::sodium is PRIVATE, and that is the most load-bearing line in this
# file.
#
# <sodium.h> must not be reachable from anything that links this target.
# crypto.h declares its own kNonceBytes/kTagBytes/... constants and
# static_asserts them against libsodium's macros in crypto.cpp precisely so
# that the public header never needs the libsodium header — which means
# PRIVATE here is enforceable rather than aspirational. The effect: no other
# module in the tree can make a raw libsodium call, so "all cryptography goes
# through security/crypto" is a property of the build graph rather than of a
# review checklist. The same argument core/database/CMakeLists.txt makes for
# keeping sqlite3 off its link line.
#
# Verifiable, not just asserted. Measured on the Debug build of 2026-10-17:
#
#   # no first-party library outside this one imports a libsodium symbol
#   for f in build/core/*/libuapp_core_*.a build/storage/sqlite/*.a; do
#       printf '%s: ' "$f"; nm -u "$f" | grep -cE '_(crypto|sodium|randombytes)'
#   done
#   # -> 0 for all seven
#
#   # only this module's two .cpp files include the libsodium header
#   grep -rln 'include <sodium.h>' core storage file-engine pdf-engine tools tests security
#   # -> security/crypto/src/crypto.cpp, security/crypto/src/secure_bytes.cpp
#
#   # and the symbols this library does import are 15 libsodium calls and no
#   # primitive of our own (Rule 0 in crypto.h), verifiable with:
#   nm -u build/security/crypto/libuapp_security_crypto.a \
#       | grep -oE '_(crypto|sodium|randombytes)[A-Za-z0-9_]*' | sort -u
#
# uapp::core_common is PUBLIC: crypto.h returns uapp::Status by value, so
# every consumer needs uapp/status.h on its include path.
target_link_libraries(uapp_security_crypto
    PUBLIC  uapp::core_common
    PRIVATE uapp::sodium uapp::compiler_options)

# Guard rails, enforced by the link line rather than by review:
#
#  * NO uapp::core_blocks, NO uapp::core_database, NO uapp::storage_sqlite.
#    This module encrypts BYTES. It has no opinion about whether those bytes
#    are a block, a property value or a page of a PDF, and giving it one
#    would invert the layering: the document model should call crypto, never
#    the reverse. A `#include <uapp/blocks/block.h>` here fails to link.
#  * NO uapp::capi. There is no C ABI for this module yet, on purpose (see
#    crypto.h, NOT IMPLEMENTED item 6) — and the dependency would run the
#    wrong way if there were.
#  * The exception/RTTI posture is NOT re-exported PUBLIC-ly the way
#    core/common does it, because core/common already does it and it arrives
#    transitively. Repeating it would duplicate flags on every consumer's
#    command line for no gain.
