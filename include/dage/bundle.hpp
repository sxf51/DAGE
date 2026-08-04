#ifndef DAGE_BUNDLE_HPP
#define DAGE_BUNDLE_HPP

#include "dage/dage.hpp"
#include "dage/resource_provider.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dage {

struct BundleDependency {
    std::string id;
    std::string constraint;
};
struct BundleSignature {
    std::string algorithm;
    std::string key_id;
    std::string signature_base64;
    std::string signed_at;
};

enum class ConfigArrayMerge { Replace, Append };
enum class ConfigNullMerge { Delete, Preserve };
struct ConfigMergeOptions {
    ConfigArrayMerge arrays = ConfigArrayMerge::Replace;
    ConfigNullMerge nulls = ConfigNullMerge::Delete;
    std::size_t max_depth = 64;
    std::size_t max_output_bytes = 16 * 1024 * 1024;
};

DAGE_CPP_API Result<Value> deep_merge_config(const Value& base,const Value& overlay,
                                              const ConfigMergeOptions& options = {});

class DAGE_CPP_API Bundle {
public:
    ~Bundle();
    Bundle(Bundle&&) noexcept;
    Bundle& operator=(Bundle&&) noexcept;
    Bundle(const Bundle&) = delete;
    Bundle& operator=(const Bundle&) = delete;

    const std::string& id() const noexcept;
    const std::string& version() const noexcept;
    const std::string& digest() const noexcept;
    const std::string& canonical_manifest() const noexcept;
    const std::string& canonical_unsigned_manifest() const noexcept;
    const std::string& signing_payload() const noexcept;
    const std::vector<BundleDependency>& dependencies() const noexcept;
    const std::vector<BundleSignature>& signatures() const noexcept;
    const Value& config() const noexcept;
    std::vector<std::string> workflow_ids() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    explicit Bundle(std::unique_ptr<Impl> impl);
    friend class Engine;
    friend std::unique_ptr<Bundle> load_bundle_from_provider(const ResourceProvider& provider);
    friend Result<std::string> bundle_workflow_json(const Bundle& bundle,
                                                    const std::string& workflow_id);
};

} // namespace dage
#endif
