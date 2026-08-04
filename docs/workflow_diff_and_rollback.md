# Workflow Diff and Bundle Rollback

DAGE exposes comparison and rollback planning without owning deployment state. The host remains
responsible for choosing the active immutable Workflow and `ResolvedBundleGraph`.

## Workflow diff

`Engine::diff_workflows(before, after)` compares two validated, compiled Workflows and returns:

- semantic IR digests before and after;
- changed top-level Workflow fields (excluding `nodes` and the non-semantic `x_revision`);
- added, removed, and modified nodes, including the exact changed node fields;
- whether effect/replay policy changed;
- nodes transitively affected through control edges, parallel branches/joins, or data references;
- checkpoint resume compatibility.

Checkpoint compatibility is intentionally strict: it is true only when the semantic IR digests are
equal. A revision-only change therefore remains compatible, while any execution-semantic change
requires a new Run or an explicit migration facility.

Adding or removing a node counts as an effect-policy change when the normalized node has an effect
policy, including defaults. This conservative rule ensures a release review cannot overlook a new
executable node merely because its policy was implicit in the source JSON.

```cpp
dage::WorkflowDiff diff = engine.diff_workflows(*deployed, *candidate);
if (diff.effect_policy_changed) {
    // Require a safety review before publishing the candidate.
}
```

## Bundle rollback plan

`ResolvedBundleGraph::plan_rollback_to(target)` creates an immutable plan from the current graph to
an already resolved older graph. It requires the same root Bundle id, a strictly older root SemVer,
and an exact target lockfile. The plan contains:

- current and target root version/digest;
- every added, removed, or changed Bundle with both version/digest identities;
- the complete target `dage.lock`;
- the target load mode and whether it was resolved in `verified` or `offline` mode.

```cpp
auto plan = current_graph->plan_rollback_to(*known_good_graph);
if (!plan) {
    // Report plan.error(); no state has changed.
}
if (production && !plan.value().target_verified) {
    // Resolve the target again in verified/offline mode before deployment.
}
// Atomically replace the host-owned graph using plan.value().target_lock_json.
```

Planning never downloads, mutates a graph, switches traffic, resumes checkpoints, or maintains a
global “current version.” In production, the host must resolve the target with signature
verification, persist/audit the plan, atomically swap its graph reference, drain or cancel old
Runs, and use Workflow diff results to decide whether old checkpoints may resume.

Rollback is deliberately directional. Upgrades, same-version content replacement, and rollback to
a different root Bundle id are rejected rather than being disguised as rollback.
