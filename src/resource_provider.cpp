#include "dage/resource_provider.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dage {
namespace {
Error resource_error(const std::string& code, const std::string& message) {
    Error error;
    error.category = "resource";
    error.code = code;
    error.message = message;
    return error;
}
}

Result<std::string> normalize_resource_path(const std::string& path) {
    if (path.empty()) return Result<std::string>::failure(resource_error("EMPTY_PATH", "resource path is empty"));
    if (path[0] == '/' || path[0] == '\\')
        return Result<std::string>::failure(resource_error("ABSOLUTE_PATH", "resource path must be relative"));
    if (path.find('\\') != std::string::npos)
        return Result<std::string>::failure(resource_error("BACKSLASH_PATH", "resource path must use '/' separators"));
    if (path.find('\0') != std::string::npos)
        return Result<std::string>::failure(resource_error("NUL_PATH", "resource path contains NUL"));

    std::string normalized;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string segment = path.substr(begin, end == std::string::npos ? path.size() - begin : end - begin);
        if (segment.empty() || segment == "." || segment == "..")
            return Result<std::string>::failure(resource_error("INVALID_PATH_SEGMENT", "resource path contains an empty, '.' or '..' segment"));
        if (!normalized.empty()) normalized.push_back('/');
        normalized += segment;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return Result<std::string>::success(std::move(normalized));
}

MemoryResourceProvider::MemoryResourceProvider(std::map<std::string, ResourceBytes> resources) {
    for (auto& item : resources) add(std::move(item.first), std::move(item.second));
}

void MemoryResourceProvider::add(std::string path, ResourceBytes content) {
    Result<std::string> normalized = normalize_resource_path(path);
    if (!normalized) throw std::invalid_argument(normalized.error().message);
    resources_[normalized.value()] = std::move(content);
}

void MemoryResourceProvider::add_text(std::string path, const std::string& content) {
    add(std::move(path), ResourceBytes(content.begin(), content.end()));
}

Result<std::vector<std::string>> MemoryResourceProvider::list_resources() const {
    std::vector<std::string> paths;
    paths.reserve(resources_.size());
    for (const auto& item : resources_) paths.push_back(item.first);
    return Result<std::vector<std::string>>::success(std::move(paths));
}

Result<ResourceBytes> MemoryResourceProvider::read_resource(const std::string& path) const {
    Result<std::string> normalized = normalize_resource_path(path);
    if (!normalized) return Result<ResourceBytes>::failure(normalized.error());
    const auto found = resources_.find(normalized.value());
    if (found == resources_.end())
        return Result<ResourceBytes>::failure(resource_error("RESOURCE_NOT_FOUND", "resource not found: " + normalized.value()));
    return Result<ResourceBytes>::success(found->second);
}

} // namespace dage
