# Support, LTS, and release process

## Version support

DAGE is pre-1.0 and currently supports only the latest development line; breaking changes remain
possible and are documented in the same change.

The proposed 1.x policy, to be frozen at the first release candidate, is:

- C ABI 1.x is append-only: existing status values, fields and meanings are not renumbered or
  changed; new structs/fields use size-versioned prefixes.
- A 1.x reader migrates checkpoints from earlier 1.x versions. Later 1.x checkpoints may be
  inspected, but resume is rejected unless all required capabilities are understood.
- The latest minor release receives normal fixes. The previous minor receives critical/high
  security fixes for at least six months after its successor. A designated LTS minor, if announced,
  receives critical/high fixes for 24 months.
- Support dates are promises only when published in release metadata. No release is silently
  designated LTS, and unsupported versions may still receive a best-effort patch without reopening
  their support window.

This window is intentionally narrow for a small library. Longer parallel support would dilute
security and ABI testing capacity; hosts requiring longer support should pin a signed release and
fund/own a maintained branch.

## Release checklist

1. Freeze scope and version; review protocol, ABI registry, checkpoint capabilities and TODO.
2. Require clean Tier-1 shared/package consumers, static consumer, C/C++ tests, language
   conformance, ABI old-header/old-binary fixtures, fuzz history, concurrency stress and sanitizers.
3. Review dependency advisories, threat-model changes and open security findings.
4. Build Release archives from a clean, immutable source revision on controlled runners.
5. Generate CPack SHA-256 sidecars and a canonical release manifest containing version, source
   revision, builder identity/provenance, target triple, filename, size and SHA-256 for every file.
6. Sign the canonical manifest with an offline/HSM-backed Ed25519 release key. Never place a private
   key in CI variables, logs, repository files, Bundle manifests, or test fixtures.
7. Verify signature and every digest in a separate clean environment; install the archive and run
   the external consumer against the installed files.
8. Have a second maintainer approve security-sensitive supply-chain changes and verification
   evidence. Publish archive, checksum, signed manifest, public-key ID, provenance, SBOM/relevant
   dependency inventory, changelog and support dates atomically.
9. Test download and verification from the public location, then retain immutable artifacts and
   evidence. Announce revocation/replacement rather than overwriting an existing version.

The deterministic create/sign/verify commands are documented in
[Signed runtime release manifest](release_manifest.md). The tool atomically writes canonical JSON
and a raw 64-byte Ed25519 signature, verifies every artifact digest, and deliberately delegates
private-key custody and public-key trust to the release environment.

Checksums detect accidental corruption but do not authenticate origin. Git tags, GitHub release
signatures, Bundle signatures and runtime archive signatures are separate trust statements and must
not be conflated.

## Emergency security release

Private reports follow `SECURITY.md`. Maintainers reproduce privately, assess affected versions,
prepare a minimal regression test and patch, coordinate CVE/advisory when appropriate, and run all
feasible release gates. Any skipped gate and compensating control must be recorded. Revoke a
compromised key through authenticated policy and publish a newly signed artifact; never replace
bytes under an existing version.
