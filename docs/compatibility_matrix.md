# Compatibility matrix

This page separates configurations exercised by current CI from the support contract that will
start with DAGE 1.0. A configured job is not evidence until it completes; a passing development job
is not a promise to support every vendor version indefinitely.

## Native runtime candidate matrix

| Operating system | Architecture | Compiler path | Package/ABI evidence |
| --- | --- | --- | --- |
| Linux | x86_64 | GCC on Ubuntu 22.04 | shared install, static install, C ABI |
| Linux | arm64 | GCC on Ubuntu 24.04 arm64 | shared install, C ABI |
| macOS | x86_64 | AppleClang on macOS 15 Intel | shared install, C ABI |
| macOS | arm64 | AppleClang on macOS 14 | shared install, C ABI |
| Windows | x86_64 | MSVC 2019-compatible toolset on Windows 2022 | shared install, C ABI |
| Windows | x86_64 | MinGW UCRT64 GCC | shared install, C ABI |

The C header is C99-compatible. Core is implemented in C++17. All candidate Tier-1 ABIs are
64-bit little-endian with eight-byte pointers/`size_t`, four-byte public enums, native platform C
calling convention, and the checked v1 struct layout. 32-bit, big-endian, Windows arm64, mobile and
no-thread environments have no 1.0 support commitment.

## Language binding validation

| Binding | Current CI toolchain | Boundary |
| --- | --- | --- |
| Python | CPython 3.11+ | stable C ABI through `ctypes`; enforced by package metadata |
| Java | Java 21+ | JNI adapter compiled with `javac --release 21` |
| C# | .NET 10+ | `net10.0` P/Invoke assembly over stable C ABI |
| Rust | Rust 1.75+ | Cargo `rust-version` and edition 2021 FFI crate |
| Node.js | Node.js 24+ | package `engines` plus Node-API adapter |

These are the candidate 1.0 minimum versions and CI compile/run baselines. DAGE does not promise
support after an upstream runtime reaches end of life; a binding may raise its minimum in a minor
release without changing the native C ABI, with advance release-note notice. Bindings query named
runtime capabilities rather than infer features from `DAGE_RUNTIME_VERSION_STRING`.

## Compatibility evidence

Every Tier-1 shared build runs:

- all-name dynamic export verification;
- exact 64-bit public struct layout and enum-width verification;
- old v1 header compiled against the new library;
- a previously linked v1 consumer executed against the new library;
- installed-package consumption through a clean external CMake project.

At the first 1.0 release candidate, the candidate fixtures become immutable. Later CI must also run
against retained, signed Tier-1 release binaries. Until that event, this matrix describes the
candidate contract and test coverage, not a released 1.0 compatibility guarantee.
