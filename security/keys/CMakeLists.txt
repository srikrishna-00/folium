add_library(uapp_security_keys STATIC
    src/key_hierarchy.cpp)
add_library(uapp::security_keys ALIAS uapp_security_keys)

target_include_directories(uapp_security_keys PUBLIC include)

# uapp::security_crypto is PUBLIC, and uapp::sodium appears NOWHERE on this
# line. Both halves of that sentence are the point of this file.
#
# PUBLIC because key_hierarchy.h's API is written in the primitives layer's
# vocabulary — crypto::ContentKey, crypto::DeviceKeyPair, crypto::SealedBlob,
# crypto::SecureBytes — so every consumer needs uapp/crypto/*.h on its
# include path. Hiding that behind PRIVATE and re-declaring the types here
# would mean two definitions of a key, which is how a 32-byte buffer ends up
# being copied between them.
#
# NO uapp::sodium, not even PRIVATE. security/crypto links libsodium PRIVATE
# precisely so that the module above it cannot reach a raw libsodium call,
# and this module is the first test of whether that boundary holds for real
# code rather than for a test binary. It does: there is no <sodium.h> under
# security/keys/, no primitive implemented here, and the only cryptography
# this library performs is a call into uapp::security_crypto.
#
# Verifiable, not asserted. Measured on the Debug build of 2026-10-20:
#
#   # this library imports ZERO libsodium symbols
#   nm -u build/security/keys/libuapp_security_keys.a \
#       | grep -cE '_(crypto|sodium|randombytes)[A-Za-z0-9_]*'
#   # -> 0
#
#   # what it DOES import from below is the primitives API and nothing else
#   nm -u build/security/keys/libuapp_security_keys.a | grep uapp
#   # -> uapp::crypto::{seal,open,derive_subkey,generate_content_key,
#   #                   generate_salt,wrap_content_key,unwrap_content_key,
#   #                   derive_key_from_passphrase,ContentKey::import,...}
#
#   # and the libsodium header is still included by exactly two files in the
#   # whole tree, both in security/crypto
#   grep -rln 'include <sodium.h>' core storage file-engine pdf-engine tools tests security
#
# Guard rails, enforced by the link line rather than by review:
#
#  * NO uapp::core_blocks, NO uapp::core_database, NO uapp::storage_sqlite.
#    This module knows that a document HAS an id; it does not know what a
#    document IS, and it must not learn. The hierarchy is a function over
#    keys and ids — persistence of its records is deliberately NOT
#    IMPLEMENTED (key_hierarchy.h, item 5), and a SQLite dependency here
#    would be the first step of implementing it by accident.
#  * NO uapp::capi. Unchanged from security/crypto: there is no C ABI for
#    the key hierarchy yet, and this module is the reason the crypto ABI was
#    deferred in the first place (core/capi/src/capi_common.cpp).
#  * The exception/RTTI posture arrives transitively through
#    uapp::core_common, exactly as it does for security/crypto.
target_link_libraries(uapp_security_keys
    PUBLIC  uapp::security_crypto
    PRIVATE uapp::compiler_options)
