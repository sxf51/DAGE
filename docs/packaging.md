# Installing and packaging DAGE

DAGE installs as a relocatable CMake package and supports both shared and static Core builds. The
official archive is a development/runtime package: it contains DAGE headers, libraries, optional
extension libraries, the CLI when enabled, CMake package metadata, and pkg-config metadata. It does
not copy or hide third-party runtime dependencies.

## Build and install

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DDAGE_BUILD_SHARED=ON \
  -DDAGE_BUILD_TESTS=OFF \
  -DDAGE_BUILD_EXAMPLES=OFF
cmake --build build-release
cmake --install build-release --prefix /opt/dage
```

Important build switches:

| Option | Default | Purpose |
| --- | --- | --- |
| `DAGE_BUILD_SHARED` | `ON` | Build the Core as a shared library; `OFF` selects static. |
| `DAGE_BUILD_TOOLS` | `ON` | Build and install the `dage` CLI. |
| `DAGE_BUILD_BUNDLE_TOOLS` | `ON` | Build official directory/ZIP and Bundle-store helpers. |
| `DAGE_BUILD_OPENTELEMETRY_BRIDGE` | `ON` | Build the SDK-neutral trace bridge. |
| `DAGE_WARNINGS_AS_ERRORS` | `OFF` | Make warnings fatal for DAGE-owned targets only. |

Dependency discovery prefers standard CMake packages for jsoncpp, OpenSSL, and zlib, with
pkg-config as the fallback. The checked-in `vcpkg.json` is the supported MSVC manifest path.

## Consume from CMake

```cmake
find_package(DAGE CONFIG REQUIRED)
target_link_libraries(my_runtime PRIVATE DAGE::dage)
```

Optional installed targets are `DAGE::dage_bundle_tools` and `DAGE::dage_opentelemetry` when their
build options were enabled. Static consumers receive `DAGE_STATIC` and the required transitive link
dependencies through the exported target; applications must not define ABI macros manually.

An executable package check lives in `tests/package/consumer`. CI installs DAGE into a clean prefix,
configures that project out of tree, links it, and executes a real Workflow. Shared packages are
checked on Linux x64/arm64, macOS x64/arm64, MinGW x64, and MSVC x64. A separate clean static
consumer gate catches missing transitive dependencies and compile definitions.

## pkg-config

The installed `lib/pkgconfig/dage.pc` derives its prefix from its own location, so moving the
installation tree does not preserve the original staging path:

```bash
PKG_CONFIG_PATH=/opt/dage/lib/pkgconfig pkg-config --cflags --libs dage
```

## Create an archive

```bash
cpack --config build-release/CPackConfig.cmake
```

CPack writes a `.zip` on Windows and `.tar.gz` elsewhere under `build-release/packages`. The file
name includes DAGE version, operating system, and processor architecture. CPack also writes a
`.sha256` sidecar, and the archive includes the MIT license and README under
`share/doc/DAGE`. It also includes `dage-dependencies.json`, recording the exact jsoncpp, OpenSSL
and optional zlib versions plus compiler/target identity used for that package.

CI archives are deliberately named `unsigned`. They validate layout and consumption but are not
production releases. A production release must additionally have provenance, checksums, an
Ed25519-signed release manifest covering the canonical file list and digests, and verification
instructions. Bundle signature enforcement does not by itself authenticate the DAGE runtime
archive.

The complete production procedure and support policy are defined in
[Support, LTS, and release process](support_and_release.md).

## Support evidence

The configured Tier-1 source/package matrix is:

| Platform | Architecture | CI compiler path |
| --- | --- | --- |
| Linux | x86_64, arm64 | GCC |
| macOS | x86_64, arm64 | AppleClang |
| Windows | x86_64 | MSVC and MinGW |

Configuration is not release evidence by itself. The TODO remains in progress until the matrix has
completed cleanly on hosted runners and the signed release procedure has been exercised. Windows
arm64 is not a Tier-1 binary target for 1.0.
