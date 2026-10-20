# security/ — the cross-cutting policy, audit and cryptography layer named in
# architecture.md section 4's layer diagram.
#
# Two modules exist so far, in a deliberate layering:
#
#   crypto/  the PRIMITIVES (libsodium PRIVATE) — key generation, AEAD, key
#            wrapping, HKDF, the passphrase KDF. Has no opinion about what
#            any given key MEANS.
#   keys/    the key HIERARCHY (security-model.md §5A.1) — Recovery Secret ->
#            URK -> Workspace Key -> derived document key, plus the
#            wrapped-key record format. Links crypto/ and NOT libsodium, so
#            "all cryptography goes through security/crypto" is a property of
#            the build graph one layer up as well as at the bottom.
#
# The dependency runs strictly one way and must keep doing so: crypto/ must
# never link keys/. A primitive that knew which key in the hierarchy it was
# operating on would be a primitive with a policy.
#
# The rest of what that diagram puts here — authorization, AI egress consent,
# agent permissions, the audit log, diagnostic redaction, and the
# `security/key-management` adapter that is supposed to own the
# Keychain/Keystore boundary — is NOT IMPLEMENTED, and is deliberately not
# stubbed: an empty permissions module would make `security-model.md` §8's
# authorisation model look started when it is not.
add_subdirectory(crypto)
add_subdirectory(keys)
