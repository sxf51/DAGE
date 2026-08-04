#ifndef DAGE_TOOLS_BUNDLE_STORE_HPP
#define DAGE_TOOLS_BUNDLE_STORE_HPP

#include "dage/bundle_resolver.hpp"
#include "dage/tools/resource_providers.hpp"
#include <filesystem>
#include <set>

namespace dage { namespace tools {

struct AtomicLockfileOptions {
    bool durable = true;
    bool create_parent = false;
    std::uint64_t max_bytes = 16 * 1024 * 1024;
};

Result<bool> write_lockfile_atomic(const std::filesystem::path& path,
                                   const std::string& lock_json,
                                   const AtomicLockfileOptions& options = {});
Result<bool> write_keyring_atomic(const std::filesystem::path& path,
                                  const Keyring& keyring,
                                  const AtomicLockfileOptions& options = {});

Result<std::set<std::string>> lockfile_digests(const std::string& lock_json);

struct CacheGcOptions {
    bool dry_run = true;
    std::size_t max_removals = 1000;
};
struct CacheGcResult {
    std::vector<std::string> candidates;
    std::vector<std::string> removed;
    std::uint64_t reclaimed_bytes = 0;
};

class BundleCache {
public:
    explicit BundleCache(std::filesystem::path root,ResourceLimits limits = {});
    Result<std::string> install(const ResourceProvider& provider);
    Result<std::shared_ptr<const ResourceProvider>> open(const std::string& digest)const;
    Result<CacheGcResult> garbage_collect(const std::set<std::string>& retained_digests,
                                          const CacheGcOptions& options = {});
    const std::filesystem::path& root()const noexcept;
private:
    std::filesystem::path root_;ResourceLimits limits_;
};

Result<BundleResolverOptions> verified_production_profile(
    std::string lock_json,
    const BundleRepository& repository,
    const KeyProvider& keys,
    const TrustPolicy& trust,
    SignaturePolicy signature_policy = {},
    bool offline = false);

}} // namespace dage::tools
#endif
