# Official Directory and ZIP bundle tools

Filesystem and archive support lives in the optional `dage_bundle_tools` target. DAGE Core remains
provider-agnostic and only sees `ResourceProvider`.

```cmake
set(DAGE_BUILD_BUNDLE_TOOLS ON)
target_link_libraries(host PRIVATE dage_bundle_tools)
```

## Providers

`DirectoryResourceProvider` canonicalizes its root, rejects symlinks and Windows reparse points,
checks every resolved path remains under the root, accepts regular files only, sorts paths
deterministically, and detects size changes during reads.

`ZipResourceProvider` supports stored and raw-deflate entries through zlib. It validates:

- EOCD and central-directory bounds, single-disk format and exact directory size;
- normalized UTF-8 relative paths, path length/depth and duplicate entries;
- local/central header agreement;
- encryption, unsupported flags/methods, ZIP64 and archive symlinks;
- archive, resource, total expanded size and compression-ratio limits;
- exact inflate input/output sizes and CRC-32.

Construction fails on an invalid archive; individual read failures return `Result` errors. ZIP
metadata, timestamps, physical entry order and compression bytes do not affect Bundle identity:
Core still hashes normalized paths and uncompressed content.

## Repositories

Two local, inherently offline `BundleRepository` implementations use fixed layouts:

```text
DirectoryBundleRepository:
  ROOT/<bundle-id>/<version>/manifest.json

ZipBundleRepository:
  ROOT/<bundle-id>/<version>.zip
```

Bundle IDs and versions are restricted to safe path components. Repository roots and coordinates
reject symlink/reparse traversal. The Resolver independently verifies that returned manifest
identity exactly matches the requested Bundle ID and version, preventing a repository from
substituting another coordinate.

## Limits

`ResourceLimits` and `ZipLimits` are immutable construction options. Defaults allow 10,000
resources, 64 MiB per resource, 512 MiB total expanded content, paths up to 1,024 bytes/64
segments, archives up to 512 MiB and a 200:1 expansion ratio. Hosts should lower these limits for
their deployment.

Portable C++ cannot make a mutable directory snapshot atomic. Signed/frozen production loading
should publish immutable version directories or ZIP files and verify the resulting lock/digest.
