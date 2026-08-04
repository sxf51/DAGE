# C Bundle Resolver and trust SPI

The C ABI exposes the same provider-agnostic resolution model as C++: a normalized root
`ResourceProvider`, an optional dependency repository, exact lockfile policy, and host-owned
KeyProvider/TrustPolicy callbacks.

## Resource providers and repositories

`dage_resource_provider_t` lists normalized resource paths as a JSON string array and reads arbitrary
bytes through `dage_owned_buffer_t`. The root descriptor passed to `dage_bundle_resolve` is borrowed
for that synchronous call; DAGE does not invoke its `release` field. A provider returned by
`repository.get`, however, is transferred and must provide `release`. DAGE releases it on success,
validation failure, and partial graph construction.

Repository callbacks receive the `offline` policy bit. In Offline mode they must not use network
fallback. `available_versions` returns a JSON string array, `get` returns an exact provider, and
`source` returns the stable provenance string written into `dage.lock`.

```c
dage_bundle_repository_vtable_t repository = {0};
repository.struct_size = sizeof(repository);
repository.available_versions = list_versions;
repository.get = get_exact_bundle;
repository.source = repository_source;
```

Callbacks and userdata passed directly to `dage_bundle_resolve` are borrowed and only need to remain
valid until it returns. Owned buffers and transferred dependency providers are released on every
exit path.

## Verified and offline resolution

```c
dage_bundle_resolver_options_t options = {0};
options.struct_size = sizeof(options);
options.mode = DAGE_BUNDLE_LOAD_VERIFIED;
options.signature_policy = DAGE_SIGNATURE_THRESHOLD;
options.signature_threshold = 2;
options.lock_json = lock_view;

dage_resolved_bundle_graph_handle graph = NULL;
dage_status_t status = dage_bundle_resolve(
    engine, &root, &options,
    &repository, repository_userdata,
    &keys, keys_userdata,
    &trust, trust_userdata,
    &graph);
```

KeyProvider receives the key ID and a JSON `SignatureContext`, then returns exactly 32 raw Ed25519
public-key bytes. TrustPolicy receives the same context and writes a strict 0/1 decision. Enabled
verification fails closed if either callback is absent. Cryptographic verification, distinct-public-
key threshold counting, lock digest enforcement, and dependency identity checks remain inside DAGE.

`Disabled` is an explicit development/testing policy. Hosts building a production profile must
reject it before invoking the raw Resolver API, just as the C++ `verified_production_profile` helper
does.

## Resolved graph

The transferred graph handle owns all resolved Bundles. It exposes the generated/exact lockfile,
root Bundle ID, and Workflow loading from either the root (empty Bundle ID) or a named dependency.

```c
size_t required = 0;
dage_resolved_bundle_graph_lock(graph, NULL, 0, &required);

dage_workflow_handle workflow = NULL;
dage_resolved_bundle_graph_load_workflow(
    engine, graph,
    (dage_string_view_t){NULL, 0},
    (dage_string_view_t){"main", 4},
    &workflow);
```

Resolution is synchronous. The returned graph no longer references repository, key, trust, or root
callbacks. Destroy it with `dage_resolved_bundle_graph_destroy`.

