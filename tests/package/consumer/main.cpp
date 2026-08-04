#include "dage/dage.hpp"

#include <memory>

int main() {
    dage::Engine engine;
    std::unique_ptr<dage::Workflow> workflow=engine.load(R"({
      "format":"dage-workflow","format_version":"0.2.0","entry":"done","nodes":{
        "done":{"type":"end","input":{"installed":true}}
      }
    })");
    const dage::ExecutionResult result=
        engine.create_run(*workflow)->execute(dage::Value::object());
    return result.success&&result.output.get("installed").as_bool()?0:1;
}
