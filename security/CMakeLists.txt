# security/ — the cross-cutting policy, audit and cryptography layer named in
# architecture.md section 4's layer diagram.
#
# Only the crypto adapter exists so far. The rest of what that diagram puts
# here — authorization, AI egress consent, agent permissions, the audit log,
# diagnostic redaction, and the `security/key-management` adapter that is
# supposed to own the Keychain/Keystore boundary — is NOT IMPLEMENTED, and is
# deliberately not stubbed: an empty permissions module would make
# `security-model.md` §8's authorisation model look started when it is not.
add_subdirectory(crypto)
