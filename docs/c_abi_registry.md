# C ABI capabilities, allocator, and callback threading

## Runtime discovery

Bindings must compile against named header constants and may query the loaded library before using
optional surfaces:

```c
if (!dage_runtime_has_capability(
        (dage_string_view_t){"c.trace_replay.v1", 17})) {
    return unsupported();
}
```

`dage_runtime_registry` returns a versioned JSON document using the standard two-call protocol. It
is the authoritative machine-readable registry for:

- runtime and C ABI versions;
- named capabilities;
- numeric status and enum values;
- minimum accepted `struct_size` prefixes;
- stable JSON/report and callback-context fields;
- supported node types and analysis result kinds;
- callback thread, concurrency, blocking, and ownership contracts.

Unknown capability names return false. Bindings should query capabilities, not compare runtime
version strings to guess features. Enum values and existing struct fields are never renumbered after
1.0; new values and tail fields are additive.

## Output allocator

`dage_engine_options_t::output_allocator` installs an optional allocator for buffers allocated by
DAGE and returned as `dage_owned_buffer_t`.

```c
dage_allocator_t allocator = {0};
allocator.struct_size = sizeof(allocator);
allocator.allocate = host_allocate;
allocator.deallocate = host_deallocate;
allocator.userdata = arena;

dage_engine_options_t options = {0};
options.struct_size = sizeof(options);
options.api_version = DAGE_C_ABI_VERSION;
options.output_allocator = &allocator;
```

DAGE copies the function pointers and userdata during Engine creation. The `dage_allocator_t`
object itself may then go out of scope, but its userdata and callback code must remain valid until
every outstanding owned output has invoked its release callback—even if the Engine was already
destroyed. Allocation receives total size and `alignof(max_align_t)` alignment; deallocation receives
the exact same pointer, size, alignment, and userdata.

Opaque handles are intentionally not allocated through this hook. They retain matching
`dage_*_destroy` functions, preventing allocator confusion across languages and shared-library
boundaries. Buffers returned by host callbacks continue to use the release callback supplied by the
host.

The discarded prototype `max_json_bytes` and `max_expression_depth` fields never affected Core and
were deleted before 1.0. `dage_engine_options_t` now exposes enforced source-byte, JSON-depth,
node, edge, expression, literal and deterministic compiled-IR budgets; zero selects the documented
runtime default. Effective per-Run execution limits remain in `dage_run_options_t`.

## Callback threading matrix

| Callback | Calling thread | Concurrent | May block | Ownership |
| --- | --- | ---: | ---: | --- |
| sync/async Executor | Scheduler worker | yes | sync only | inputs borrowed; async completion transferred |
| Scheduler submit | submitting thread | yes | no | task transferred only when returning OK |
| StateStore | Scheduler worker | yes | yes | inputs borrowed; outputs transferred |
| TraceSink emit | emitting thread | yes | no | event borrowed |
| ResourceLease acquire/renew | Scheduler worker | yes | yes | lease transferred on successful acquire |
| Repository | Resolver caller | no per resolve | yes | dependency Provider transferred |
| KeyProvider/TrustPolicy | Resolver caller | no per resolve | yes | context borrowed; key transferred |
| Shadow metric | compare caller | no | yes | sample borrowed |
| userdata destroy | last releasing thread | possible | no | userdata consumed |
| owned-buffer release | consuming thread | possible | no | buffer consumed |

“No per resolve” does not serialize separate concurrent calls to `dage_bundle_resolve`; shared host
userdata must still be thread-safe when the host resolves graphs concurrently. Destroy and release
callbacks must tolerate whichever thread drops the last reference and must never call back into a
partially destroyed host runtime.
