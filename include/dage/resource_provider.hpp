#ifndef DAGE_RESOURCE_PROVIDER_HPP
#define DAGE_RESOURCE_PROVIDER_HPP

#include "dage/result.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dage {

using ResourceBytes = std::vector<std::uint8_t>;

class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;
    virtual Result<std::vector<std::string>> list_resources() const = 0;
    virtual Result<ResourceBytes> read_resource(const std::string& normalized_path) const = 0;
};

class MemoryResourceProvider final : public ResourceProvider {
public:
    MemoryResourceProvider() = default;
    explicit MemoryResourceProvider(std::map<std::string, ResourceBytes> resources);

    void add(std::string normalized_path, ResourceBytes content);
    void add_text(std::string normalized_path, const std::string& content);
    Result<std::vector<std::string>> list_resources() const override;
    Result<ResourceBytes> read_resource(const std::string& normalized_path) const override;

private:
    std::map<std::string, ResourceBytes> resources_;
};

Result<std::string> normalize_resource_path(const std::string& path);

} // namespace dage
#endif
