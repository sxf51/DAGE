# Key scope, rotation, revocation, and signature policy

DAGE verifies Bundle bytes locally with Ed25519. It does not store private keys or become a PKI:
the host controls key distribution and higher-level trust through `KeyProvider` and `TrustPolicy`.

## Scoped persistent Keyring

`KeyRecord::namespace_scopes` restricts a key to exact reverse-DNS namespaces and their dot-delimited
children. For example, `com.example` permits `com.example` and `com.example.search`, but not
`com.examples` or `com.example-malicious`. An empty scope list is intentionally unrestricted and
should only be used for controlled development or a separately scoped `TrustPolicy`.

```cpp
dage::KeyRecord release;
release.key_id = "org-example-release-2026-a";
release.key = public_key; // exactly 32 raw Ed25519 bytes
release.namespace_scopes = {"org.example"};

dage::Keyring keyring;
keyring.add(std::move(release));
keyring.revoke("retired-or-compromised-key");
```

`snapshot_json()` emits a deterministic `dage-keyring` version 1 document. The official Bundle tools
write it with the same durable atomic replacement used for `dage.lock`:

```cpp
auto saved = dage::tools::write_keyring_atomic("trust/keyring.json", keyring);
auto restored = dage::Keyring::from_snapshot_json(read_file("trust/keyring.json"));
```

Rotation is additive: publish the new public key under a new ID, overlap both keys while artifacts
move to the new signer, then revoke or remove the old ID. Revocation is absolute and persists in
the snapshot. DAGE deliberately does not implement “accept signatures made before revocation”
from the manifest's `signed_at`: that value is signer-supplied metadata, not a trusted timestamp.
Hosts needing historical validity must validate a signed transparency-log entry, trusted timestamp,
or release attestation in `TrustPolicy`.

The snapshot contains trust decisions and is security-sensitive. Production hosts should themselves
authenticate its origin (for example, OS-protected configuration, a signed policy document, or a
configuration control plane). Atomic replacement provides crash consistency, not provenance.

## Signature policies

`BundleResolverOptions::signature_policy` supports:

- `Disabled`: explicit development/test escape hatch; rejected by
  `verified_production_profile`.
- `AtLeastOne`: the production default.
- `Threshold`: requires `threshold` different valid public keys.
- `All`: every declared signature must be trusted and cryptographically valid.

Thresholds count distinct public-key bytes, not signature entries or key IDs. Repeating a signature
or registering aliases for the same public key therefore cannot satisfy M-of-N.

```cpp
dage::SignaturePolicy policy;
policy.kind = dage::SignaturePolicyKind::Threshold;
policy.threshold = 2;

auto production = dage::tools::verified_production_profile(
    lock_json, repository, keys, trust, policy, true /* offline */);
```

`TrustPolicy` remains the final authorization layer. It should enforce environment, Bundle class,
organization, release channel, attestation, and any trusted-time requirements not represented by
the local Keyring. Enabled verification fails closed when either `KeyProvider` or `TrustPolicy` is
missing. `AllowKnownKeysTrustPolicy` is the explicit choice when successful scoped key lookup is
the host's complete authorization rule.

## HSM/KMS integration

[The compilable adapter example](../examples/cpp/hsm_kms_key_provider.cpp) delegates public-key
lookup to a host callback suitable for a PKCS#11 or cloud KMS client. DAGE never asks an HSM/KMS to
sign and never receives private material. The host must authenticate the service, pin the intended
tenant/key namespace, honor offline mode, and choose a cache TTL consistent with its revocation SLA.
