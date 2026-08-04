#include "dage/dage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size) {
    if(size>1024U*1024U)return 0;
    const std::string input(reinterpret_cast<const char*>(data),size);
    try {
        (void)dage::Value::parse(input);
    } catch(...) {
    }
    static dage::Engine engine;
    const auto diagnostics=engine.validate(input);
    const bool has_error=std::any_of(
        diagnostics.begin(),diagnostics.end(),[](const dage::Diagnostic& item) {
            return item.severity==dage::Severity::Error;
        });
    if(!has_error) {
        std::unique_ptr<dage::Workflow> workflow=engine.load(input);
        (void)workflow->normalized_json();
        (void)workflow->export_dot();
        (void)workflow->export_mermaid();
    }
    return 0;
}
