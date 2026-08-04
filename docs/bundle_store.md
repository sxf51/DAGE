# Atomic lockfile, Bundle cache, and production profile

These APIs live in the optional `dage_bundle_tools` library. Core remains provider-agnostic: it
does not choose a filesystem layout, delete cached data, or define a process-wide production mode.

## Atomic `dage.lock` updates

`write_lockfile_atomic` validates the lock header and every exact `sha256:` digest before touching
the destination. It writes a private same-directory temporary file, flushes its contents, atomically
replaces the target, and durably syncs the containing directory where the platform exposes that
operation. A validation or temporary-write failure leaves the existing lockfile unchanged.

```cpp
#include <dage/tools/bundle_store.hpp>

dage::tools::AtomicLockfileOptions write;
write.create_parent = true;
write.durable = true;

auto saved = dage::tools::write_lockfile_atomic(
    "deployment/dage.lock", resolved_graph->lock_json(), write);
if (!saved) {
    // saved.error().code is stable enough for programmatic handling.
}
```

Only regular single-host filesystems with atomic rename semantics are suitable. Network and
userspace filesystems may weaken those guarantees. A `DIRECTORY_SYNC_FAILED` result means the
replacement became visible but crash durability could not be confirmed; the host must treat that
as an indeterminate publication, reread the file, and reconcile it.

## Immutable content-addressed cache

`BundleCache` stores normalized snapshots under `sha256/<digest-hex>`. Installation loads and
validates the source Bundle, writes a private staging directory, reloads that snapshot, compares
its digest, and only then publishes it. Existing and concurrent winners are revalidated instead of
being overwritten.

```cpp
dage::tools::BundleCache cache("/var/lib/my-host/dage-cache");
auto digest = cache.install(provider);
auto immutable_provider = cache.open(digest.value());
```

Garbage collection is deliberately explicit. The host computes the complete live digest set from
all deployed lockfiles, rollbacks, and active runs, previews the candidates, then opts into removal:

```cpp
std::set<std::string> live;
for (const std::string& lock : deployment_lockfiles) {
    auto digests = dage::tools::lockfile_digests(lock);
    live.insert(digests.value().begin(), digests.value().end());
}

auto preview = cache.garbage_collect(live); // dry-run by default
dage::tools::CacheGcOptions apply;
apply.dry_run = false;
apply.max_removals = 100;
auto collected = cache.garbage_collect(live, apply);
```

GC ignores unknown filenames and link/reparse entries, never follows them, and stops if the
candidate safety limit is exceeded. It does not infer liveness: omitting an active digest is a host
error, so production hosts should retain a safety window or serialize deployment publication and
GC with a host-owned lock.

## Verified production resolver

`verified_production_profile` is a fail-closed constructor for resolver options. It requires a
valid lockfile, a nonzero signature threshold, a repository, `KeyProvider`, and `TrustPolicy`, and
selects `Verified` (or explicitly requested `Offline`) mode.

```cpp
auto profile = dage::tools::verified_production_profile(
    lock_json, repository, key_provider, trust_policy, signature_policy, true);
if (!profile) return fail(profile.error());

dage::BundleResolver resolver(profile.value());
auto graph = resolver.resolve(root_provider);
```

The returned options contain non-owning pointers. The repository, key provider, and trust policy
must outlive the resolver and resolution call. “Offline” is a policy signal enforced through the
repository SPI; a host repository must reject network access when that flag is set.
