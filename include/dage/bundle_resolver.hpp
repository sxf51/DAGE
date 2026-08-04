#ifndef DAGE_BUNDLE_RESOLVER_HPP
#define DAGE_BUNDLE_RESOLVER_HPP

#include "dage/bundle.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dage {

enum class BundleLoadMode { Development, Frozen, Verified, Offline };

struct PublicKey {
    std::vector<std::uint8_t> bytes;
};

struct KeyRecord {
    std::string key_id;
    PublicKey key;
    std::vector<std::string> namespace_scopes;
    bool revoked = false;
};

struct SignatureContext {
    std::string bundle_id;
    std::string bundle_version;
    std::string bundle_digest;
    std::string key_id;
    std::string algorithm;
    std::string signed_at;
};

class KeyProvider {
public:
    virtual ~KeyProvider() = default;
    virtual Result<PublicKey> find_key(const std::string& key_id,
                                       const SignatureContext& context) const = 0;
};

class Keyring final : public KeyProvider {
public:
    void add(std::string key_id, PublicKey key);
    void add(KeyRecord record);
    void revoke(const std::string& key_id);
    Result<std::string> snapshot_json() const;
    static Result<Keyring> from_snapshot_json(const std::string& json);
    Result<PublicKey> find_key(const std::string& key_id,
                               const SignatureContext& context) const override;
private:
    std::map<std::string, KeyRecord> records_;
};

class TrustPolicy {
public:
    virtual ~TrustPolicy() = default;
    virtual Result<bool> trust(const SignatureContext& context) const = 0;
};

class AllowKnownKeysTrustPolicy final : public TrustPolicy {
public:
    Result<bool> trust(const SignatureContext& context) const override;
};

enum class SignaturePolicyKind { Disabled, AtLeastOne, Threshold, All };

struct SignaturePolicy {
    SignaturePolicyKind kind = SignaturePolicyKind::AtLeastOne;
    std::size_t threshold = 1;
};

class BundleRepository {
public:
    virtual ~BundleRepository() = default;
    virtual Result<std::vector<std::string>> available_versions(const std::string& bundle_id,
                                                                 bool offline) const = 0;
    virtual Result<std::shared_ptr<const ResourceProvider>> get(const std::string& bundle_id,
                                                                const std::string& version,
                                                                bool offline) const = 0;
    virtual std::string source() const = 0;
};

class MemoryBundleRepository final : public BundleRepository {
public:
    explicit MemoryBundleRepository(std::string source = "memory");
    void add(std::string bundle_id, std::string version,
             std::shared_ptr<const ResourceProvider> provider);
    Result<std::vector<std::string>> available_versions(const std::string& bundle_id,
                                                         bool offline) const override;
    Result<std::shared_ptr<const ResourceProvider>> get(const std::string& bundle_id,
                                                        const std::string& version,
                                                        bool offline) const override;
    std::string source() const override;
private:
    std::string source_;
    std::map<std::string, std::map<std::string, std::shared_ptr<const ResourceProvider>>> bundles_;
};

struct BundleResolverOptions {
    BundleLoadMode mode = BundleLoadMode::Development;
    const BundleRepository* repository = nullptr;
    const KeyProvider* keys = nullptr;
    const TrustPolicy* trust = nullptr;
    std::string lock_json;
    SignaturePolicy signature_policy;
};

enum class BundleVersionChangeKind { Added, Removed, Changed };

struct BundleVersionChange {
    std::string bundle_id;
    BundleVersionChangeKind kind;
    std::string from_version;
    std::string to_version;
    std::string from_digest;
    std::string to_digest;
};

struct BundleRollbackPlan {
    std::string root_bundle_id;
    std::string from_version;
    std::string to_version;
    std::string from_digest;
    std::string to_digest;
    std::string target_lock_json;
    BundleLoadMode target_load_mode;
    bool target_verified;
    std::vector<BundleVersionChange> changes;
};

class DAGE_CPP_API ResolvedBundleGraph {
public:
    ~ResolvedBundleGraph();
    ResolvedBundleGraph(ResolvedBundleGraph&&) noexcept;
    ResolvedBundleGraph& operator=(ResolvedBundleGraph&&) noexcept;
    ResolvedBundleGraph(const ResolvedBundleGraph&) = delete;
    ResolvedBundleGraph& operator=(const ResolvedBundleGraph&) = delete;

    const Bundle& root() const noexcept;
    const Bundle* find(const std::string& bundle_id) const noexcept;
    std::vector<std::string> topological_order() const;
    const std::string& lock_json() const noexcept;
    Result<Value> merged_config(const ConfigMergeOptions& options = {}) const;
    BundleLoadMode load_mode() const noexcept;
    Result<BundleRollbackPlan> plan_rollback_to(const ResolvedBundleGraph& target) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    explicit ResolvedBundleGraph(std::unique_ptr<Impl> impl);
    friend class BundleResolver;
};

class DAGE_CPP_API BundleResolver {
public:
    explicit BundleResolver(BundleResolverOptions options);
    Result<std::unique_ptr<ResolvedBundleGraph>> resolve(const ResourceProvider& root_provider) const;
private:
    std::unique_ptr<ResolvedBundleGraph> resolve_unchecked(const ResourceProvider& root_provider) const;
    BundleResolverOptions options_;
};

} // namespace dage
#endif
