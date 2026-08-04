# Workflow parse and compile resource limits

Every `Engine` has nonzero Workflow ingestion limits. Defaults are 16 MiB source bytes, JSON depth
64, 10,000 nodes, 50,000 edges, 256 KiB of expression-bearing strings, 8 MiB of UTF-8 literals,
and 32 MiB of compiled IR. Direct load, registered Workflows, Bundle-loaded Workflows and
Patch-produced candidates share the same `Engine::load` path.

```cpp
dage::WorkflowResourceLimits limits;
limits.max_workflow_bytes = 2 * 1024 * 1024;
limits.max_json_depth = 48;
limits.max_nodes = 2000;
limits.max_edges = 10000;
limits.max_expression_bytes = 128 * 1024;
limits.max_literal_bytes = 4 * 1024 * 1024;
limits.max_compiled_ir_bytes = 16 * 1024 * 1024;
engine.set_workflow_resource_limits(limits);
```

The C ABI exposes the same fields in `dage_engine_options_t`; zero selects the runtime default.
Resource rejection returns `DAGE_STATUS_RESOURCE_EXHAUSTED`. Diagnostics contain stable codes plus
`observed` and `limit` values.

Byte and nesting checks run before JsonCpp constructs the document. The depth scanner ignores
brackets inside JSON strings and the same limit is passed to JsonCpp's `stackLimit`. Node limits run
immediately after root/schema checks.

Expression accounting includes strings containing `${`; literal accounting includes UTF-8 object
keys and string values. Defaults are 256 KiB of expression-bearing strings and 8 MiB of total
literal bytes.

Compiled-IR accounting is deterministic and platform-independent: it charges fixed costs for IR
records and exact encoded sizes for retained strings and `Value` payloads. It is deliberately a
logical admission budget rather than an allocator- or standard-library-specific heap measurement.
Rejection uses `WORKFLOW_IR_BYTES_LIMIT` with the attempted logical `observed` size and configured
`limit`.

Replay accepts an already validated immutable `Workflow` handle; it has no alternate Workflow
parser or compiler. Consequently a replay Run cannot bypass admission limits. Patch dry-run and
analysis compile their candidate through the same bounded path and report IR exhaustion at
`/candidate`.
