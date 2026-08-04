#include "dage/dage.hpp"
#include <iostream>
int main() {
    const std::string json="{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"hello\",\"nodes\":{\"hello\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"input\":{\"message\":\"${workflow.input.message}\"},\"next\":\"done\"},\"done\":{\"type\":\"end\",\"input\":{\"result\":\"${nodes.hello.output.message}\"}}}}";
    dage::Engine engine;
    engine.register_executor("echo",[](const dage::ExecutionContext&,const dage::Value& input){return dage::ExecutionResult::ok(input);});
    std::unique_ptr<dage::Workflow> workflow=engine.load(json);
    std::unique_ptr<dage::Run> run=engine.create_run(*workflow);
    dage::ExecutionResult result=run->execute(dage::Value::parse("{\"message\":\"hello DAGE\"}"));
    std::cout<<result.output.to_json(true);
    return result.success?0:1;
}
