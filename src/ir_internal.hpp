#ifndef DAGE_IR_INTERNAL_HPP
#define DAGE_IR_INTERNAL_HPP

#include "dage/ir.hpp"

#include <memory>
#include <string>

namespace dage {
std::shared_ptr<const WorkflowIR> compile_workflow_ir(
    const std::string& normalized_json, std::size_t max_accounted_bytes);
}
#endif
