# C analysis, replay, and evaluation API

Complex analysis results are returned as UTF-8 JSON with `schema_version: 1`. This keeps the C ABI
small while allowing additive report fields. Existing fields and meanings are frozen at 1.0; hosts
must ignore unknown object fields.

## Patch and Workflow diff

`dage_workflow_dry_run_patch` validates each change and the fully compiled private candidate without
publishing it. `dage_workflow_analyze_patch` adds direct/transitive impact, effect-policy changes,
checkpoint compatibility, and an inverse Patch. `dage_workflow_diff` compares two already compiled
Workflows.

All three use the standard two-call output protocol and are side-effect free:

```c
size_t required = 0;
dage_workflow_analyze_patch(engine, workflow, patch, NULL, 0, &required);
char* json = allocate(required);
dage_workflow_analyze_patch(engine, workflow, patch, json, required, &required);
```

The result `kind` values are `patch_dry_run`, `patch_analysis`, and `workflow_diff`. Diagnostics retain
stable code, JSON Pointer path, severity, message, and suggestion fields.

## Deterministic Trace replay

Runs intended for replay must set:

```c
dage_run_options_t options = {0};
options.struct_size = sizeof(options);
options.trace_capture = DAGE_TRACE_CAPTURE_FULL;
```

Full traces contain resolved inputs and may contain sensitive application data. Hosts must redact
according to policy while preserving replay-required fields, use a lossless sink, and protect traces
at rest.

`dage_trace_replay_prepare` accepts a JSON array of structured event envelopes, validates schema,
Workflow/Bundle identity, per-Run sequence continuity, invocation identity, recorded Executor
outcomes, root input, and final result, then returns an immutable plan handle. Trace input is capped
at 64 MiB at this ABI boundary.

The plan exposes:

- `dage_trace_replay_plan_input`: exact root input used to execute the replay Run;
- `dage_trace_replay_plan_describe`: source run/trace identity, pinned digests, expected result, and
  outcome count;
- `dage_run_create_replay`: a Run whose Executor outcomes come only from the validated trace.

Replay does not invoke host Executors, acquire ResourceLeases, or repeat external effects. Destroy
the plan after creating the Run; the Run copies the required replay state.

## Selective rerun

`dage_run_selective_rerun` resets the requested completed node and its reachable downstream subgraph.
It follows the normal two-call output protocol and caches the first result between calls. Nodes with
`at_most_once`, `manual`, or `forbidden` replay policy are rejected. This is an execution operation,
not an analysis operation: safe/idempotent Executors may run again.

## Shadow comparison

`dage_shadow_compare` consumes baseline and candidate samples:

```json
{
  "label": "candidate",
  "workflow_digest": "sha256:...",
  "result": {"success": true, "output": {}},
  "trace": [{"event": "run_started", "payload": {"run_mode": "shadow"}}]
}
```

The candidate must prove Shadow mode in its trace. Each `dage_metric_definition_t` contains a
borrowed name, non-negative weight/tolerance, callback, and userdata. The callback receives the
complete normalized sample including every trace-envelope field. Metric direction is “higher is
better”; cost metrics should return negative cost.

Comparison invokes each metric exactly once for baseline and once for candidate. Therefore the
report uses a transferred `dage_owned_buffer_t` instead of the two-call protocol. The host must call
its release callback. At most 1024 metrics and 64 MiB per sample are accepted.

The report is advisory: DAGE does not persist experiments or deploy the recommended candidate.

