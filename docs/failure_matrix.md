# Failure and race matrix

`dage_failure_matrix` is an isolated CTest executable with the `failure` label and a 90-second
timeout. It covers failures that must produce deterministic errors rather than crashes, hangs,
partial publication, or duplicate callbacks.

## StateStore and durable files

The matrix verifies:

- a host StateStore `DISK_FULL` Result reaches the Run unchanged;
- a throwing StateStore becomes `STATE_PROVIDER_EXCEPTION`;
- a successful CAS returning anything other than `expected + 1` becomes
  `STATE_PROTOCOL_ERROR`;
- an orphan temporary partial record cannot affect the committed record;
- truncated headers and checksum corruption return `STATE_CORRUPT`;
- `restore_run` rejects corrupt or unreadable durable state instead of treating it as absent;
- a failed atomic write preserves the previous version and removes its temporary file.

On POSIX, the last case runs in a child process with `RLIMIT_FSIZE=1`. This exercises a real
short/failed file write in the official FileStateStore. Windows still runs provider-level disk-full
injection, corruption, orphan partial, and atomic-state checks; low-level write failure is covered
by Linux CI.

FileStateStore atomic replacement now owns its temporary path with an RAII cleanup guard. Failures
during create, write, flush, or rename do not leave unbounded `.tmp.*` files.

## Hostile SPI

C++ host SPI methods are not allowed to unwind through Run orchestration. StateStore calls are
wrapped and translated to stable state errors. ResourceLeaseProvider exceptions become
`LEASE_PROVIDER_EXCEPTION`. StateStore load errors other than `STATE_NOT_FOUND` fail closed during
root recovery and parallel-child discovery.

TraceSink `emit` is declared `noexcept`; throwing from it violates the C++ interface contract and
terminates by language design. C ABI callbacks remain separately exception-contained at their
adapter boundary.

## Cancellation and timeout races

The matrix repeatedly races:

- host cancellation against native async completion;
- node timeout against native async completion.

Each race must terminate within five seconds, deliver the Run callback exactly once, and return
only one of the two valid winner outcomes. Late completion remains safe.

Run locally with:

```powershell
ctest --test-dir build -L failure --output-on-failure
```

ASan/UBSan CI runs the matrix, and TSAN explicitly includes the `failure` label.
