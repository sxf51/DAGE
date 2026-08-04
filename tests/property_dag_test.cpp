#include "dage/dage.hpp"
#include "dage/ir.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string workflow_json(const std::vector<std::size_t>& next,bool reverse_order) {
    std::ostringstream json;
    json << R"({"format":"dage-workflow","format_version":"0.2.0","id":"property",)"
         << R"("entry":"n0","limits":{"max_steps":1024},"nodes":{)";
    for(std::size_t position=0;position<next.size();++position) {
        const std::size_t index=reverse_order?next.size()-1-position:position;
        if(position)json << ',';
        json << "\"n" << index << "\":{\"type\":\"tool\",\"executor\":\"model\","
             << "\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},"
             << "\"input\":\"${workflow.input}\",\"next\":\"n" << next[index] << "\"}";
    }
    if(!next.empty())json << ',';
    json << "\"n" << next.size()
         << "\":{\"type\":\"end\",\"input\":\"${workflow.input}\"}}}";
    return json.str();
}

std::vector<bool> reference_path(const std::vector<std::size_t>& next) {
    std::vector<bool> visited(next.size(),false);
    std::size_t current=0;
    while(current<next.size()) {
        if(visited[current])throw std::runtime_error("generator produced a cycle");
        visited[current]=true;
        current=next[current];
    }
    if(current!=next.size())throw std::runtime_error("generator produced an invalid target");
    return visited;
}

void require(bool condition,const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        for(std::uint32_t seed=0;seed<250;++seed) {
            std::mt19937 random(seed);
            const std::size_t count=1U+(random()%64U);
            std::vector<std::size_t> next(count);
            for(std::size_t node=0;node<count;++node) {
                std::uniform_int_distribution<std::size_t> target(node+1,count);
                next[node]=target(random);
            }
            const std::vector<bool> expected=reference_path(next);
            std::vector<std::uint32_t> calls(count,0);
            dage::Engine engine;
            engine.register_executor("model",[&](const dage::ExecutionContext& context,
                                                  const dage::Value& input) {
                const std::size_t index=static_cast<std::size_t>(
                    std::stoul(context.node_id.substr(1)));
                if(index>=calls.size())throw std::runtime_error("unexpected model node");
                ++calls[index];
                return dage::ExecutionResult::ok(input);
            });
            const std::string forward=workflow_json(next,false);
            const std::string reversed=workflow_json(next,true);
            const auto generated_diagnostics=engine.validate(forward);
            require(std::none_of(
                        generated_diagnostics.begin(),generated_diagnostics.end(),
                        [](const dage::Diagnostic& item) {
                            return item.severity==dage::Severity::Error;
                        }),
                    "generated DAG did not validate");
            std::unique_ptr<dage::Workflow> workflow=engine.load(forward);
            std::unique_ptr<dage::Workflow> reordered=engine.load(reversed);
            require(workflow->ir().digest()==reordered->ir().digest(),
                    "semantic digest depends on JSON object order");
            dage::Value input=dage::Value::object();
            input.set("seed",dage::Value(static_cast<std::int64_t>(seed)));
            const dage::ExecutionResult result=engine.create_run(*workflow)->execute(input);
            require(result.success,"generated DAG execution failed");
            require(result.output.get("seed").as_integer()==seed,"model output differs");
            for(std::size_t node=0;node<count;++node)
                require(calls[node]==(expected[node]?1U:0U),"runtime path differs from reference model");
        }

        dage::Engine engine;
        const std::string cycle=
            R"({"format":"dage-workflow","format_version":"0.2.0","entry":"a","nodes":{)"
            R"("a":{"type":"noop","next":"b"},"b":{"type":"noop","next":"a"}}})";
        const auto diagnostics=engine.validate(cycle);
        require(std::any_of(diagnostics.begin(),diagnostics.end(),[](const dage::Diagnostic& item) {
            return item.code=="UNDECLARED_CYCLE";
        }),"undeclared cycle was not rejected");
        std::cout << "250 generated DAGs matched the reference model\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "property DAG test failed: " << error.what() << '\n';
        return 1;
    }
}
