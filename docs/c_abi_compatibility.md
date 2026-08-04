# C ABI compatibility gate

DAGE treats the C ABI as the long-lived boundary for every language binding. The compatibility
gate is enabled for shared-library builds and runs on Linux, macOS, and Windows CI.

## What the gate proves

The frozen `tests/abi/v1` fixture is an ABI consumer, not another copy of the implementation:

1. `dage_abi_v1_old_header_new_library` compiles the frozen v1 header and consumer directly
   against the current shared library. This detects source compatibility and missing declarations.
2. `dage_abi_v1_old_binary_new_library` first links that consumer to a minimal same-name baseline
   library. The test copies the already-linked executable into an isolated staging directory and
   injects only the current DAGE library through the platform loader search path. The baseline
   artifact is never modified. This detects removed exports, calling-convention changes,
   incompatible struct prefixes, and changed status values.
3. `dage_abi_v1_export_manifest` loads the produced shared library without linking to it and checks
   every name in the source-controlled `tests/abi/v1/exports.txt`. This prevents the broader C ABI
   surface from disappearing merely because the behavioral fixture does not call every function.
4. `dage_abi_v1_layout_64` checks pointer, `size_t`, enum, public struct size and every
   compatibility-relevant member offset against the candidate 64-bit little-endian v1 layout. It
   runs with the platform's C compiler, so GCC, AppleClang, MinGW and MSVC disagreements fail in
   their own Tier-1 jobs.

The fixture exercises capability discovery, engine creation, Workflow loading, Run creation, the
two-call output contract, execution, and paired handle destruction.

Run only the compatibility gate with:

```sh
cmake -S . -B build-abi -DDAGE_BUILD_SHARED=ON -DDAGE_BUILD_TESTS=ON
cmake --build build-abi
ctest --test-dir build-abi -L abi --output-on-failure
```

## Maintenance rule

Once a baseline is published, its directory is immutable. Do not update an existing frozen header,
consumer, stub, or export manifest to make a breaking change pass. Add a new versioned fixture when
the ABI grows. Before the first 1.0 release candidate, the v1 fixture is a candidate baseline and
must track intentional breaking changes rather than preserving accidental prototype compatibility.

For a stable major version:

- exported functions and status values are never removed, renumbered, or repurposed;
- an existing field keeps its type, offset, ownership, and semantics;
- size-versioned structs grow only by appending fields;
- callers may pass any documented older struct prefix;
- reserved fields remain reserved until an explicitly documented additive revision;
- ownership pairs and callback threading rules remain stable.

The 1.0 Tier-1 ABI baseline is 64-bit little-endian and uses each platform's native C calling
convention. DAGE does not support 32-bit or big-endian targets in 1.0. On all Tier-1 x64/arm64
targets, pointers and `size_t` are eight bytes and public enums are four bytes. This layout test is
a deliberate freeze candidate, not an assertion that arbitrary compiler flags such as structure
packing are supported; consumers must not alter ABI layout with `#pragma pack` around `dage.h`.

This source-controlled baseline is reproducible before DAGE has historical release artifacts.
Starting with the first 1.0 release candidate, CI must additionally retain and execute the signed
Tier-1 release binaries. The frozen fixture and complete export-presence test do not replace symbol
metadata/calling-convention comparison or package validation.
