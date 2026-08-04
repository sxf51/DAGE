#ifndef DAGE_BUNDLE_INTERNAL_HPP
#define DAGE_BUNDLE_INTERNAL_HPP

#include "dage/bundle.hpp"

#include <memory>
#include <string>

namespace dage {
std::unique_ptr<Bundle> load_bundle_from_provider(const ResourceProvider& provider);
Result<std::string> bundle_workflow_json(const Bundle& bundle, const std::string& workflow_id);
}
#endif
