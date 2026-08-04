# Signed runtime release manifest

`tools/release_manifest.py` creates a deterministic manifest for DAGE runtime archives and uses the
OpenSSL CLI for Ed25519 signing and verification. This signs runtime distribution artifacts; it is
separate from Bundle signing.

## Create

```bash
python tools/release_manifest.py create \
  --version 1.0.0 \
  --source-revision 0123456789abcdef0123456789abcdef01234567 \
  --builder controlled-linux-release-01 \
  --target linux-x86_64 \
  --key-id dage-release-2027-a \
  --artifact build-release/packages/DAGE-1.0.0-Linux-x86_64.tar.gz \
  --dependency-inventory build-release/dage-dependencies.json \
  --output build-release/packages/release-manifest.json
```

Artifacts are sorted by UTF-8 filename and record exact byte size and lowercase SHA-256. Duplicate
filenames, non-files and path-bearing manifest names are rejected. JSON uses sorted keys, UTF-8,
compact separators and one trailing newline. Output replacement is same-directory atomic and
file/directory metadata is flushed before publication on POSIX. Version must be SemVer; source
revision must be a full lowercase SHA-1 or SHA-256 object ID; identity fields are bounded.
Exactly one dependency inventory is required and signed with the runtime artifacts. Configure
production builds with `-DDAGE_REQUIRE_DEPENDENCY_VERSIONS=ON`; configuration then fails instead of
publishing an inventory with an unknown enabled dependency version. Creation and post-download
verification also require the inventory's `dage_version` to exactly match the signed release
version, preventing dependency evidence from another build being relabelled into a release.

## Sign

```bash
python tools/release_manifest.py sign \
  --manifest build-release/packages/release-manifest.json \
  --private-key /secure/offline/dage-release.pem \
  --signature build-release/packages/release-manifest.ed25519
```

The private key path is passed to OpenSSL and key bytes are never loaded, copied, logged or stored
by the Python tool. Production keys must live offline or behind an HSM-controlled signing adapter;
the repository and general CI secrets are not acceptable key stores. Signature output must be
exactly 64 bytes and is atomically published.

## Verify

```bash
python tools/release_manifest.py verify \
  --manifest release-manifest.json \
  --signature release-manifest.ed25519 \
  --public-key dage-release-public.pem \
  --expected-key-id dage-release-2027-a \
  --artifact-dir downloaded-release
```

Verification fails if the JSON is not in canonical form, its schema is unsupported, the signature
is invalid, the manifest `key_id` differs from the caller-pinned expected ID, an artifact is absent,
or size/digest differs. Consumers must authenticate the public key and expected key ID through
their own release trust policy; a manifest naming a key does not make that key trusted.

The `release` CTest creates an ephemeral test-only Ed25519 key, proves deterministic creation,
signs and verifies an artifact, then confirms tampering fails. It is functional evidence only, not
a substitute for the controlled two-maintainer production release exercise.
