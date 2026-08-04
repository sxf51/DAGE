#ifndef DAGE_TOOLS_RESOURCE_PROVIDERS_HPP
#define DAGE_TOOLS_RESOURCE_PROVIDERS_HPP

#include "dage/resource_provider.hpp"
#include "dage/bundle_resolver.hpp"
#include <filesystem>
#include <memory>

namespace dage { namespace tools {

struct ResourceLimits {
    std::size_t max_resources = 10000;
    std::uint64_t max_resource_bytes = 64 * 1024 * 1024;
    std::uint64_t max_total_bytes = 512 * 1024 * 1024;
    std::size_t max_path_bytes = 1024;
    std::size_t max_depth = 64;
};

class DirectoryResourceProvider final : public ResourceProvider {
public:
    explicit DirectoryResourceProvider(std::filesystem::path root,
                                       ResourceLimits limits = {});
    Result<std::vector<std::string>> list_resources() const override;
    Result<ResourceBytes> read_resource(const std::string& normalized_path) const override;
private:
    std::filesystem::path root_;
    ResourceLimits limits_;
};

struct ZipLimits : ResourceLimits {
    std::uint64_t max_archive_bytes = 512 * 1024 * 1024;
    std::uint32_t max_compression_ratio = 200;
};

class ZipResourceProvider final : public ResourceProvider {
public:
    class Impl;
    explicit ZipResourceProvider(const std::filesystem::path& archive,
                                 ZipLimits limits = {});
    explicit ZipResourceProvider(ResourceBytes archive,
                                 ZipLimits limits = {});
    ~ZipResourceProvider();
    Result<std::vector<std::string>> list_resources() const override;
    Result<ResourceBytes> read_resource(const std::string& normalized_path) const override;
private:
    std::shared_ptr<const Impl> impl_;
};

class DirectoryBundleRepository final : public BundleRepository {
public:
    explicit DirectoryBundleRepository(std::filesystem::path root,
                                       ResourceLimits limits = {});
    Result<std::vector<std::string>> available_versions(
        const std::string& bundle_id,bool offline)const override;
    Result<std::shared_ptr<const ResourceProvider>> get(
        const std::string& bundle_id,const std::string& version,bool offline)const override;
    std::string source()const override;
private:
    std::filesystem::path root_;ResourceLimits limits_;
};

class ZipBundleRepository final : public BundleRepository {
public:
    explicit ZipBundleRepository(std::filesystem::path root,
                                 ZipLimits limits = {});
    Result<std::vector<std::string>> available_versions(
        const std::string& bundle_id,bool offline)const override;
    Result<std::shared_ptr<const ResourceProvider>> get(
        const std::string& bundle_id,const std::string& version,bool offline)const override;
    std::string source()const override;
private:
    std::filesystem::path root_;ZipLimits limits_;
};

}} // namespace dage::tools
#endif
