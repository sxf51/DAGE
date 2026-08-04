# Canonical Bundle format and golden vectors

Bundle identity is independent of input JSON formatting, resource enumeration order, ZIP metadata,
timestamps and compression. The canonicalization contract is:

- objects are encoded with keys sorted by UTF-8 byte order;
- arrays retain their declared order;
- null, booleans and integers use lowercase JSON literals and decimal integers;
- finite doubles use lowercase scientific notation with 17 fractional digits;
- positive and negative zero both encode as `0`;
- strings use UTF-8, escape quote/backslash and use the short JSON escapes for control characters;
- remaining bytes below U+0020 use lowercase `\u00xx`;
- no insignificant whitespace or trailing newline is emitted.

The signing payload removes the top-level `signatures` member and is then:

```text
manifest.json\n
sha256:<canonical unsigned manifest hash>\n
<normalized resource path>\n
sha256:<uncompressed resource hash>\n
...
```

Non-manifest resources are sorted by normalized path. The Bundle digest is SHA-256 of the complete
signing payload. Ed25519 signs those exact UTF-8 bytes, including the final newline.

## Golden vector

[`testdata/golden/bundle_v1.vector.json`](../testdata/golden/bundle_v1.vector.json) freezes:

- canonical signed and unsigned manifests, including Unicode, escaping, double and negative zero;
- each resource hash, complete signing payload and Bundle digest;
- a fixed Ed25519 seed/public key/signature used only for conformance testing.

The test loads the directory, compares every frozen byte, verifies the signature in `verified`
mode, proves provider enumeration order is irrelevant, and rejects modified content. CI runs the
same vector on Linux, macOS and Windows UCRT64. `.gitattributes` fixes vector files to LF so checkout
settings cannot alter signed bytes.

Changing this vector is a protocol change. A vector update must document why existing published
Bundle identities are intentionally invalidated; after 1.0, create a new vector/canonical format
version instead of rewriting an existing vector.
