#include "ir_internal.hpp"
#include "sha256.hpp"

#include <json/json.h>
#include <array>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace dage {
namespace {
Json::Value parse(const std::string& text) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors)) throw std::runtime_error(errors);
    return root;
}
Value value_of(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Value::parse(Json::writeString(builder, value));
}
NodeType node_type(const std::string& type) {
    static const std::map<std::string, NodeType> values = {
        {"llm",NodeType::Llm},{"tool",NodeType::Tool},{"transform",NodeType::Transform},
        {"condition",NodeType::Condition},{"parallel",NodeType::Parallel},{"join",NodeType::Join},
        {"subflow",NodeType::Subflow},{"human",NodeType::Human},{"start",NodeType::Start},
        {"end",NodeType::End},{"noop",NodeType::Noop},{"custom",NodeType::Custom}};
    const auto found=values.find(type);if(found==values.end())throw std::runtime_error("unknown node type");
    return found->second;
}
EffectKind effect_kind(const std::string& value) {
    if(value=="pure")return EffectKind::Pure;
    if(value=="local_state")return EffectKind::LocalState;
    if(value=="external_read")return EffectKind::ExternalRead;
    if(value=="external_write")return EffectKind::ExternalWrite;
    if(value=="irreversible")return EffectKind::Irreversible;
    return EffectKind::Pure;
}
ReplayPolicy replay_policy(const std::string& value) {
    if(value=="safe")return ReplayPolicy::Safe;
    if(value=="idempotent")return ReplayPolicy::Idempotent;
    if(value=="at_most_once")return ReplayPolicy::AtMostOnce;
    if(value=="manual")return ReplayPolicy::Manual;
    if(value=="forbidden")return ReplayPolicy::Forbidden;
    return ReplayPolicy::Safe;
}
std::vector<Json::Value> edge_values(const Json::Value& node,const char* field) {
    std::vector<Json::Value> out;if(!node.isMember(field))return out;const Json::Value& value=node[field];
    if(value.isString()){Json::Value edge(Json::objectValue);edge["to"]=value;out.push_back(edge);}
    else if(value.isObject())out.push_back(value);
    else if(value.isArray()){
        for(Json::ArrayIndex i=0;i<value.size();++i){
            if(value[i].isString()){
                Json::Value edge(Json::objectValue);edge["to"]=value[i];out.push_back(edge);
            }else out.push_back(value[i]);
        }
    }
    return out;
}
}

class EdgeIR::Impl {
public:
    std::string target,condition;bool otherwise=false;Value set_values=Value::object(),loop=Value::object();std::uint32_t max_hits=0;
};
EdgeIR::EdgeIR(std::shared_ptr<const Impl> impl):impl_(std::move(impl)){}
const std::string& EdgeIR::target()const noexcept{return impl_->target;}
const std::string& EdgeIR::condition()const noexcept{return impl_->condition;}
bool EdgeIR::otherwise()const noexcept{return impl_->otherwise;}
const Value& EdgeIR::set_values()const noexcept{return impl_->set_values;}
const Value& EdgeIR::loop()const noexcept{return impl_->loop;}
std::uint32_t EdgeIR::max_hits()const noexcept{return impl_->max_hits;}

class NodeIR::Impl {
public:
    std::string id,name,executor,idempotency_key,parallel_join;
    NodeType type=NodeType::Noop;bool enabled=true,executor_guarantees=false;
    Value input=Value::object(),config=Value::object(),parallel_policy=Value::object();
    EffectKind effect=EffectKind::Pure;ReplayPolicy replay=ReplayPolicy::Safe;
    std::uint64_t timeout_ms=0,retry_delay_ms=0,retry_jitter_ms=0;std::uint32_t max_attempts=1;std::string retry_backoff;
    std::vector<EdgeIR> next,error;std::vector<std::string> branches;
};
NodeIR::NodeIR(std::shared_ptr<const Impl> impl):impl_(std::move(impl)){}
const std::string& NodeIR::id()const noexcept{return impl_->id;} const std::string& NodeIR::name()const noexcept{return impl_->name;}
NodeType NodeIR::type()const noexcept{return impl_->type;} bool NodeIR::enabled()const noexcept{return impl_->enabled;}
const std::string& NodeIR::executor()const noexcept{return impl_->executor;} const Value& NodeIR::input()const noexcept{return impl_->input;}
const Value& NodeIR::config()const noexcept{return impl_->config;} EffectKind NodeIR::effect_kind()const noexcept{return impl_->effect;}
ReplayPolicy NodeIR::replay_policy()const noexcept{return impl_->replay;} const std::string& NodeIR::idempotency_key()const noexcept{return impl_->idempotency_key;}
bool NodeIR::executor_guarantees_idempotency()const noexcept{return impl_->executor_guarantees;}
std::uint64_t NodeIR::timeout_ms()const noexcept{return impl_->timeout_ms;} std::uint32_t NodeIR::max_attempts()const noexcept{return impl_->max_attempts;}
std::uint64_t NodeIR::retry_delay_ms()const noexcept{return impl_->retry_delay_ms;} const std::vector<EdgeIR>& NodeIR::next_edges()const noexcept{return impl_->next;}
std::uint64_t NodeIR::retry_jitter_ms()const noexcept{return impl_->retry_jitter_ms;}const std::string& NodeIR::retry_backoff()const noexcept{return impl_->retry_backoff;}
const std::vector<EdgeIR>& NodeIR::error_edges()const noexcept{return impl_->error;} const std::vector<std::string>& NodeIR::parallel_branches()const noexcept{return impl_->branches;}
const std::string& NodeIR::parallel_join()const noexcept{return impl_->parallel_join;} const Value& NodeIR::parallel_policy()const noexcept{return impl_->parallel_policy;}

class WorkflowIR::Impl {
public:
    std::string id,entry,digest;std::uint64_t max_steps=1000,timeout_ms=0;std::uint32_t max_parallel=1;
    std::vector<NodeIR> nodes;std::map<std::string,std::size_t> index;
};
WorkflowIR::WorkflowIR(std::unique_ptr<Impl> impl):impl_(std::move(impl)){} WorkflowIR::~WorkflowIR()=default;
WorkflowIR::WorkflowIR(WorkflowIR&&) noexcept=default;WorkflowIR& WorkflowIR::operator=(WorkflowIR&&) noexcept=default;
const std::string& WorkflowIR::id()const noexcept{return impl_->id;}const std::string& WorkflowIR::entry()const noexcept{return impl_->entry;}
const std::string& WorkflowIR::digest()const noexcept{return impl_->digest;}std::uint64_t WorkflowIR::max_steps()const noexcept{return impl_->max_steps;}
std::uint64_t WorkflowIR::timeout_ms()const noexcept{return impl_->timeout_ms;}std::uint32_t WorkflowIR::max_parallel()const noexcept{return impl_->max_parallel;}
const std::vector<NodeIR>& WorkflowIR::nodes()const noexcept{return impl_->nodes;}
const NodeIR* WorkflowIR::find_node(const std::string& id)const noexcept{auto i=impl_->index.find(id);return i==impl_->index.end()?nullptr:&impl_->nodes[i->second];}

class WorkflowIRCompiler {
public:
    static std::shared_ptr<const WorkflowIR> compile(
        const std::string& normalized,std::size_t max_accounted_bytes) {
        std::size_t accounted=128;
        auto account=[&](std::size_t amount){
            if(accounted>max_accounted_bytes||amount>max_accounted_bytes-accounted){
                const std::string observed=amount>std::numeric_limits<std::size_t>::max()-accounted
                    ?"overflow":std::to_string(accounted+amount);
                throw std::runtime_error("WORKFLOW_IR_BYTES_LIMIT: observed="+observed+
                                         " limit="+std::to_string(max_accounted_bytes));
            }
            accounted+=amount;
        };
        Json::Value root=parse(normalized);auto impl=std::make_unique<WorkflowIR::Impl>();
        Json::Value semantic=root;semantic.removeMember("x_revision");
        Json::StreamWriterBuilder digest_writer;digest_writer["indentation"]="";
        impl->id=root.get("id","").asString();impl->entry=root["entry"].asString();
        account(impl->id.size()+impl->entry.size()+64);
        impl->digest=sha256_digest(Json::writeString(digest_writer,semantic));
        impl->max_steps=root["limits"].get("max_steps",1000).asUInt64();impl->timeout_ms=root["limits"].get("timeout_ms",0).asUInt64();
        impl->max_parallel=root["limits"].get("max_parallel",1).asUInt();
        const Json::Value& nodes=root["nodes"];const std::vector<std::string> names=nodes.getMemberNames();
        for(const std::string& id:names){const Json::Value& source=nodes[id];auto node=std::make_shared<NodeIR::Impl>();
            node->id=id;node->name=source.get("name",id).asString();node->type=node_type(source["type"].asString());node->enabled=source.get("enabled",true).asBool();
            node->executor=source.get("executor","").asString();node->input=value_of(source.get("input",Json::Value(Json::objectValue)));
            node->config=value_of(source.get("config",Json::Value(Json::objectValue)));node->timeout_ms=source.get("timeout_ms",0).asUInt64();
            node->max_attempts=source["retry"].get("max_attempts",1).asUInt();node->retry_delay_ms=source["retry"].get("delay_ms",0).asUInt64();
            node->retry_jitter_ms=source["retry"].get("jitter_ms",0).asUInt64();node->retry_backoff=source["retry"].get("backoff","fixed").asString();
            const Json::Value effects=source.get("effects",Json::Value(Json::objectValue));node->effect=effect_kind(effects.get("kind","pure").asString());
            node->replay=replay_policy(effects.get("replay","safe").asString());node->idempotency_key=effects.get("idempotency_key","").asString();
            node->executor_guarantees=effects.get("executor_guarantees_idempotency",false).asBool();
            account(256+node->id.size()+node->name.size()+node->executor.size()+
                    node->idempotency_key.size()+node->retry_backoff.size()+
                    node->input.to_json(false).size()+node->config.to_json(false).size());
            auto add_edges=[&](const char* field,std::vector<EdgeIR>& target){for(const Json::Value& e:edge_values(source,field)){auto edge=std::make_shared<EdgeIR::Impl>();
                edge->target=e.get("to","").asString();edge->condition=e.get("when","").asString();edge->otherwise=e.get("otherwise",false).asBool();
                edge->set_values=value_of(e.get("set",Json::Value(Json::objectValue)));edge->loop=value_of(e.get("loop",Json::Value(Json::objectValue)));
                edge->max_hits=e.get("max_hits",0).asUInt();
                account(128+edge->target.size()+edge->condition.size()+edge->set_values.to_json(false).size()+
                        edge->loop.to_json(false).size());
                target.emplace_back(EdgeIR(edge));}};
            add_edges("next",node->next);add_edges("on_error",node->error);
            if(source["branches"].isArray())for(const Json::Value& branch:source["branches"])node->branches.push_back(branch.asString());
            node->parallel_join=source.get("join","").asString();node->parallel_policy=value_of(source.get("policy",Json::Value(Json::objectValue)));
            account(node->parallel_join.size()+node->parallel_policy.to_json(false).size());
            for(const std::string& branch:node->branches)account(branch.size()+16);
            impl->index[id]=impl->nodes.size();impl->nodes.emplace_back(NodeIR(node));
        }
        return std::shared_ptr<const WorkflowIR>(new WorkflowIR(std::move(impl)));
    }
};
std::shared_ptr<const WorkflowIR> compile_workflow_ir(
    const std::string& normalized,std::size_t max_accounted_bytes){
    if(!max_accounted_bytes)throw std::invalid_argument("IR byte budget must be positive");
    return WorkflowIRCompiler::compile(normalized,max_accounted_bytes);
}

} // namespace dage
