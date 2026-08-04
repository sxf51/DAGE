#include "dage/dage.hpp"
#include "dage/bundle.hpp"
#include "dage/ir.hpp"
#include "bundle_internal.hpp"
#include "ir_internal.hpp"
#include "sha256.hpp"

#include <json/json.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <future>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace dage {

namespace {

std::atomic<std::uint64_t> next_run_id(1);
std::atomic<std::uint64_t> next_recovery_owner(1);

Error state_provider_error(const std::string& code,const std::string& message) {
    Error error;error.category="state_store";error.code=code;error.message=message;return error;
}

template<class T,class Function>
Result<T> call_state_provider(Function function) {
    try {
        return function();
    } catch(const std::exception& error) {
        return Result<T>::failure(state_provider_error("STATE_PROVIDER_EXCEPTION",error.what()));
    } catch(...) {
        return Result<T>::failure(
            state_provider_error("STATE_PROVIDER_EXCEPTION","StateStore threw a non-standard exception."));
    }
}

Result<std::uint64_t> store_compare_exchange(
    const std::shared_ptr<StateStore>& store,const std::string& run_id,
    std::uint64_t expected,const std::string& checkpoint) {
    Result<std::uint64_t> result=call_state_provider<std::uint64_t>(
        [&](){return store->compare_exchange(run_id,expected,checkpoint);});
    if(result&&result.value()!=expected+1)
        return Result<std::uint64_t>::failure(state_provider_error(
            "STATE_PROTOCOL_ERROR","StateStore CAS returned a non-monotonic version."));
    return result;
}

Result<StateRecord> store_load(const std::shared_ptr<StateStore>& store,const std::string& run_id) {
    return call_state_provider<StateRecord>([&](){return store->load(run_id);});
}

Result<StateRecord> store_claim(
    const std::shared_ptr<StateStore>& store,const std::string& run_id,
    std::uint64_t expected,const std::string& owner) {
    Result<StateRecord> result=call_state_provider<StateRecord>(
        [&](){return store->claim(run_id,expected,owner);});
    if(result&&(result.value().version!=expected+1||result.value().owner!=owner))
        return Result<StateRecord>::failure(state_provider_error(
            "STATE_PROTOCOL_ERROR","StateStore claim returned invalid ownership metadata."));
    return result;
}

Result<bool> store_erase(const std::shared_ptr<StateStore>& store,const std::string& run_id) {
    return call_state_provider<bool>([&](){return store->erase(run_id);});
}

Result<std::unique_ptr<ResourceLease>> acquire_resource_lease(
    const std::shared_ptr<ResourceLeaseProvider>& provider,const ResourceRequest& request,
    const CancellationToken* cancellation) {
    try {
        return provider->acquire(request,cancellation);
    } catch(const std::exception& error) {
        Error translated;translated.category="resource_limit";
        translated.code="LEASE_PROVIDER_EXCEPTION";translated.message=error.what();
        return Result<std::unique_ptr<ResourceLease>>::failure(std::move(translated));
    } catch(...) {
        Error translated;translated.category="resource_limit";
        translated.code="LEASE_PROVIDER_EXCEPTION";
        translated.message="ResourceLeaseProvider threw a non-standard exception.";
        return Result<std::unique_ptr<ResourceLease>>::failure(std::move(translated));
    }
}

class TimerCoordinator {
public:
    class Handle {
    public:
        void cancel() noexcept {
            if(cancelled_.exchange(true))return;
            try{if(on_cancel_)on_cancel_();}catch(...){}
        }
        bool cancelled()const noexcept{return cancelled_.load();}
    private:
        std::atomic<bool> cancelled_{false};
        std::function<void()> on_cancel_;
        friend class TimerCoordinator;
    };
    TimerCoordinator():stopping_(false),worker_([this](){run();}){}
    ~TimerCoordinator(){
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
        changed_.notify_all();if(worker_.joinable())worker_.join();
    }
    std::shared_ptr<Handle> schedule_after(std::uint64_t delay_ms,std::function<void()> callback){
        if(!delay_ms||!callback)return std::shared_ptr<Handle>();
        std::shared_ptr<Handle> handle(new Handle());
        handle->on_cancel_=[this](){
            {std::lock_guard<std::mutex> lock(mutex_);++cancelled_count_;++revision_;compact_locked();}
            changed_.notify_one();
        };
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(Task{std::chrono::steady_clock::now()+std::chrono::milliseconds(delay_ms),
                         sequence_++,std::move(callback),handle});
        ++revision_;compact_locked();
        changed_.notify_one();
        return handle;
    }
    std::size_t pending_count()const{std::lock_guard<std::mutex> lock(mutex_);return tasks_.size();}
private:
    struct Task {
        std::chrono::steady_clock::time_point due;std::uint64_t sequence;
        std::function<void()> callback;std::shared_ptr<Handle> handle;
    };
    struct Later {bool operator()(const Task&a,const Task&b)const{
        return a.due==b.due?a.sequence>b.sequence:a.due>b.due;}};
    void run(){
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stopping_){
            while(!tasks_.empty()&&tasks_.top().handle->cancelled()){
                tasks_.pop();if(cancelled_count_)--cancelled_count_;
            }
            if(tasks_.empty()){changed_.wait(lock,[this](){return stopping_||!tasks_.empty();});continue;}
            const auto due=tasks_.top().due;
            const std::uint64_t revision=revision_;
            if(changed_.wait_until(lock,due,[this,revision](){return stopping_||revision_!=revision;}))continue;
            if(stopping_||tasks_.empty())continue;
            Task task=tasks_.top();tasks_.pop();lock.unlock();
            if(!task.handle->cancelled())try{task.callback();}catch(...){}
            lock.lock();
        }
    }
    void compact_locked(){
        if(cancelled_count_<64||cancelled_count_*2<tasks_.size())return;
        std::priority_queue<Task,std::vector<Task>,Later> retained;
        while(!tasks_.empty()){
            Task task=tasks_.top();tasks_.pop();
            if(!task.handle->cancelled())retained.push(std::move(task));
        }
        tasks_.swap(retained);cancelled_count_=0;
    }
    mutable std::mutex mutex_;std::condition_variable changed_;bool stopping_;
    std::uint64_t sequence_=0;std::uint64_t revision_=0;std::size_t cancelled_count_=0;
    std::priority_queue<Task,std::vector<Task>,Later> tasks_;std::thread worker_;
};

Json::Value parse_json(const std::string& text) {
    if (text.size() > 16U * 1024U * 1024U) throw std::runtime_error("JSON exceeds 16 MiB limit");
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors))
        throw std::runtime_error(errors);
    return root;
}

std::size_t json_nesting_depth(const std::string& text) {
    std::size_t depth=0,maximum=0;bool quoted=false,escaped=false;
    for(char value:text){if(quoted){if(escaped)escaped=false;else if(value=='\\')escaped=true;
        else if(value=='"')quoted=false;}else if(value=='"')quoted=true;
        else if(value=='{'||value=='['){++depth;maximum=std::max(maximum,depth);}
        else if((value=='}'||value==']')&&depth)--depth;}return maximum;
}
Json::Value parse_workflow_json(const std::string& text,const WorkflowResourceLimits& limits) {
    if(text.size()>limits.max_workflow_bytes)throw std::runtime_error(
        "WORKFLOW_BYTES_LIMIT: observed="+std::to_string(text.size())+" limit="+std::to_string(limits.max_workflow_bytes));
    const std::size_t depth=json_nesting_depth(text);
    if(depth>limits.max_json_depth)throw std::runtime_error(
        "WORKFLOW_DEPTH_LIMIT: observed="+std::to_string(depth)+" limit="+std::to_string(limits.max_json_depth));
    Json::CharReaderBuilder builder;builder["collectComments"]=false;
    builder["stackLimit"]=static_cast<Json::UInt64>(limits.max_json_depth+1);
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());Json::Value root;std::string errors;
    if(!reader->parse(text.data(),text.data()+text.size(),&root,&errors))throw std::runtime_error(errors);
    return root;
}
void measure_workflow_strings(const Json::Value& value,std::size_t& literals,std::size_t& expressions){
    if(value.isString()){
        const std::string text=value.asString();literals+=text.size();
        if(text.find("${")!=std::string::npos)expressions+=text.size();
        return;
    }
    if(value.isArray()){for(const Json::Value& item:value)measure_workflow_strings(item,literals,expressions);return;}
    if(value.isObject())for(const std::string& name:value.getMemberNames()){
        literals+=name.size();measure_workflow_strings(value[name],literals,expressions);}
}

class PatchValidationError : public std::runtime_error {
public:
    PatchValidationError(std::string code,std::string path,std::string message)
        :std::runtime_error(code+": "+message),code_(std::move(code)),path_(std::move(path)),
         message_(std::move(message)){}
    const std::string& code()const noexcept{return code_;}
    const std::string& path()const noexcept{return path_;}
    const std::string& message()const noexcept{return message_;}
private:
    std::string code_,path_,message_;
};

std::string write_json(const Json::Value& value, bool styled) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = styled ? "  " : "";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, value);
}

bool valid_id(const std::string& id) {
    if (id.empty() || id[0] < 'a' || id[0] > 'z') return false;
    for (std::size_t i = 1; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

const char* const node_types[] = {
    "llm", "tool", "transform", "condition", "parallel", "join",
    "subflow", "human", "start", "end", "noop", "custom"
};

bool known_type(const std::string& type) {
    for (std::size_t i = 0; i < sizeof(node_types) / sizeof(node_types[0]); ++i)
        if (type == node_types[i]) return true;
    return false;
}

bool needs_executor(const std::string& type) {
    return type == "llm" || type == "tool" || type == "transform" ||
           type == "join" || type == "human" || type == "custom";
}
bool needs_executor(NodeType type) {
    return type==NodeType::Llm||type==NodeType::Tool||type==NodeType::Transform||
           type==NodeType::Join||type==NodeType::Human||type==NodeType::Custom;
}
const char* node_type_name(NodeType type){
    switch(type){
    case NodeType::Llm:return "llm";case NodeType::Tool:return "tool";case NodeType::Transform:return "transform";
    case NodeType::Condition:return "condition";case NodeType::Parallel:return "parallel";case NodeType::Join:return "join";
    case NodeType::Subflow:return "subflow";case NodeType::Human:return "human";case NodeType::Start:return "start";
    case NodeType::End:return "end";case NodeType::Noop:return "noop";case NodeType::Custom:return "custom";
    }return "unknown";
}
const char* run_mode_name(RunMode mode){
    switch(mode){case RunMode::Normal:return "normal";case RunMode::Replay:return "replay";case RunMode::Shadow:return "shadow";}
    return "normal";
}
const char* node_status_name(NodeStatus status){
    switch(status){case NodeStatus::Pending:return "pending";case NodeStatus::Running:return "running";
    case NodeStatus::Succeeded:return "succeeded";case NodeStatus::Failed:return "failed";
    case NodeStatus::Suspended:return "suspended";case NodeStatus::Skipped:return "skipped";case NodeStatus::Blocked:return "blocked";}
    return "pending";
}

void add_diag(std::vector<Diagnostic>& out, Severity severity, const std::string& code,
              const std::string& path, const std::string& message, const std::string& suggestion = "") {
    Diagnostic d;
    d.severity = severity; d.code = code; d.path = path; d.message = message; d.suggestion = suggestion;
    out.push_back(d);
}

std::vector<Json::Value> edges_of(const Json::Value& node, const char* field) {
    std::vector<Json::Value> result;
    if (!node.isMember(field)) return result;
    const Json::Value& edge = node[field];
    if (edge.isString()) {
        Json::Value e(Json::objectValue); e["to"] = edge; result.push_back(e);
    } else if (edge.isObject()) result.push_back(edge);
    else if (edge.isArray()) for (Json::ArrayIndex i = 0; i < edge.size(); ++i) {
        if (edge[i].isString()) { Json::Value e(Json::objectValue); e["to"] = edge[i]; result.push_back(e); }
        else result.push_back(edge[i]);
    }
    return result;
}

std::string escape_mermaid(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') out += "&quot;";
        else if (text[i] == '\n' || text[i] == '\r') out += " ";
        else out += text[i];
    }
    return out;
}

std::string escape_dot(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"' || text[i] == '\\') out += '\\';
        if (text[i] == '\n' || text[i] == '\r') out += "\\n";
        else out += text[i];
    }
    return out;
}

bool truthy(const Json::Value& value) {
    if (value.isBool()) return value.asBool();
    if (value.isNull()) return false;
    if (value.isNumeric()) return value.asDouble() != 0.0;
    if (value.isString()) return !value.asString().empty();
    return !value.empty();
}

Json::Value lookup_path(const std::string& path, const Json::Value& workflow_input,
                        const Json::Value& input, const Json::Value& output,
                        const Json::Value& error, const Json::Value& nodes,
                        const Json::Value& state, const Json::Value& run) {
    std::size_t dot = path.find('.');
    std::string root = dot == std::string::npos ? path : path.substr(0, dot);
    Json::Value cur;
    if (root == "workflow") { cur = Json::Value(Json::objectValue); cur["input"] = workflow_input; }
    else if (root == "input") cur = input;
    else if (root == "output") cur = output;
    else if (root == "error") cur = error;
    else if (root == "nodes") cur = nodes;
    else if (root == "state") cur = state;
    else if (root == "run") cur = run;
    else return Json::Value();
    std::size_t pos = dot == std::string::npos ? path.size() : dot + 1;
    while (pos < path.size()) {
        dot = path.find('.', pos);
        std::string key = path.substr(pos, dot == std::string::npos ? path.size() - pos : dot - pos);
        if (!cur.isObject() || !cur.isMember(key)) return Json::Value();
        cur = cur[key];
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return cur;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string replace_token(std::string value,const std::string& token,const std::string& replacement){
    std::size_t position=0;while((position=value.find(token,position))!=std::string::npos){
        value.replace(position,token.size(),replacement);position+=replacement.size();
    }return value;
}
class RuntimeEffectCommitter final:public EffectCommitter{
public:void commit()noexcept override{value_.store(true);}bool committed()const noexcept override{return value_.load();}
private:std::atomic<bool> value_{false};
};

Json::Value literal(const std::string& text) {
    const std::string t = trim(text);
    if (t == "true") return Json::Value(true);
    if (t == "false") return Json::Value(false);
    if (t == "null") return Json::Value();
    if (t.size() >= 2 && ((t[0] == '\'' && t[t.size()-1] == '\'') ||
                          (t[0] == '"' && t[t.size()-1] == '"')))
        return Json::Value(t.substr(1, t.size()-2));
    Json::Value parsed;
    try { parsed = parse_json(t); return parsed; } catch (...) {}
    return Json::Value(t);
}

bool compare(const Json::Value& a, const Json::Value& b, const std::string& op) {
    if (op == "==" || op == "!=") {
        bool eq = (a.isNumeric() && b.isNumeric()) ? a.asDouble() == b.asDouble() : a == b;
        return op == "==" ? eq : !eq;
    }
    if (a.isNumeric() && b.isNumeric()) {
        double x = a.asDouble(), y = b.asDouble();
        if (op == ">") return x > y;
        if (op == ">=") return x >= y;
        if (op == "<") return x < y;
        if (op == "<=") return x <= y;
    }
    const std::string x = a.asString(), y = b.asString();
    if (op == ">") return x > y;
    if (op == ">=") return x >= y;
    if (op == "<") return x < y;
    if (op == "<=") return x <= y;
    return false;
}

Json::Value eval_operand(const std::string& raw, const Json::Value& workflow_input,
                         const Json::Value& input, const Json::Value& output,
                         const Json::Value& error, const Json::Value& nodes,
                         const Json::Value& state, const Json::Value& run) {
    const std::string text=trim(raw);
    if(text.substr(0,7)=="length("&&text.size()>8&&text[text.size()-1]==')') {
        Json::Value v=lookup_path(trim(text.substr(7,text.size()-8)),workflow_input,input,output,error,nodes,state,run);
        return Json::Value(static_cast<Json::UInt64>(v.size()));
    }
    if(text.find('.')!=std::string::npos) {
        Json::Value v=lookup_path(text,workflow_input,input,output,error,nodes,state,run);
        if(!v.isNull())return v;
    }
    return literal(text);
}

bool eval_expr(const std::string& wrapped, const Json::Value& workflow_input,
               const Json::Value& input, const Json::Value& output,
               const Json::Value& error, const Json::Value& nodes,
               const Json::Value& state, const Json::Value& run) {
    std::string expr = trim(wrapped);
    if (expr.size() >= 3 && expr.substr(0, 2) == "${" && expr[expr.size()-1] == '}')
        expr = trim(expr.substr(2, expr.size()-3));
    std::size_t p = expr.find(" or ");
    if (p != std::string::npos)
        return eval_expr(expr.substr(0,p), workflow_input,input,output,error,nodes,state,run) ||
               eval_expr(expr.substr(p+4), workflow_input,input,output,error,nodes,state,run);
    p = expr.find(" and ");
    if (p != std::string::npos)
        return eval_expr(expr.substr(0,p), workflow_input,input,output,error,nodes,state,run) &&
               eval_expr(expr.substr(p+5), workflow_input,input,output,error,nodes,state,run);
    if (expr.substr(0, 4) == "not ")
        return !eval_expr(expr.substr(4), workflow_input,input,output,error,nodes,state,run);
    const char* ops[] = {"==", "!=", ">=", "<=", ">", "<"};
    for (std::size_t i = 0; i < 6; ++i) {
        p = expr.find(ops[i]);
        if (p != std::string::npos) {
            Json::Value lhs = eval_operand(expr.substr(0,p),workflow_input,input,output,error,nodes,state,run);
            Json::Value rhs = eval_operand(expr.substr(p + std::string(ops[i]).size()),workflow_input,input,output,error,nodes,state,run);
            return compare(lhs, rhs, ops[i]);
        }
    }
    const char* binary_funcs[] = {"contains", "starts_with", "ends_with"};
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string prefix = std::string(binary_funcs[i]) + "(";
        if (expr.substr(0,prefix.size()) == prefix && expr[expr.size()-1] == ')') {
            const std::string args=expr.substr(prefix.size(),expr.size()-prefix.size()-1);
            const std::size_t comma=args.find(',');
            if(comma==std::string::npos)return false;
            Json::Value a=lookup_path(trim(args.substr(0,comma)),workflow_input,input,output,error,nodes,state,run);
            Json::Value b=literal(args.substr(comma+1));
            if(i==0){
                if(a.isArray()){for(Json::ArrayIndex j=0;j<a.size();++j)if(a[j]==b)return true;return false;}
                if(a.isObject())return a.isMember(b.asString());
                return a.asString().find(b.asString())!=std::string::npos;
            }
            const std::string x=a.asString(),y=b.asString();
            if(i==1)return x.size()>=y.size()&&x.substr(0,y.size())==y;
            return x.size()>=y.size()&&x.substr(x.size()-y.size())==y;
        }
    }
    const char* funcs[] = {"exists", "empty", "length"};
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string prefix = std::string(funcs[i]) + "(";
        if (expr.substr(0,prefix.size()) == prefix && expr[expr.size()-1] == ')') {
            Json::Value v = lookup_path(trim(expr.substr(prefix.size(), expr.size()-prefix.size()-1)),
                                        workflow_input,input,output,error,nodes,state,run);
            if (i == 0) return !v.isNull();
            if (i == 1) return v.isNull() || v.empty();
            return v.size() != 0;
        }
    }
    return truthy(lookup_path(expr,workflow_input,input,output,error,nodes,state,run));
}

Json::Value resolve_template(const Json::Value& value, const Json::Value& workflow_input,
                             const Json::Value& input, const Json::Value& output,
                             const Json::Value& error, const Json::Value& nodes,
                             const Json::Value& state, const Json::Value& run) {
    if (value.isString()) {
        const std::string s = value.asString();
        if (s.size() >= 3 && s.substr(0,2) == "${" && s[s.size()-1] == '}')
            return lookup_path(trim(s.substr(2,s.size()-3)),workflow_input,input,output,error,nodes,state,run);
        std::string result = s;
        std::size_t start = 0;
        while ((start = result.find("${", start)) != std::string::npos) {
            std::size_t end = result.find('}', start + 2);
            if (end == std::string::npos) break;
            Json::Value v = lookup_path(trim(result.substr(start+2,end-start-2)),workflow_input,input,output,error,nodes,state,run);
            std::string replacement = v.isString() ? v.asString() : write_json(v,false);
            result.replace(start,end-start+1,replacement); start += replacement.size();
        }
        return Json::Value(result);
    }
    if (value.isArray()) {
        Json::Value result(Json::arrayValue);
        for (Json::ArrayIndex i=0;i<value.size();++i)
            result.append(resolve_template(value[i],workflow_input,input,output,error,nodes,state,run));
        return result;
    }
    if (value.isObject()) {
        Json::Value result(Json::objectValue);
        std::vector<std::string> names = value.getMemberNames();
        for (std::size_t i=0;i<names.size();++i)
            result[names[i]]=resolve_template(value[names[i]],workflow_input,input,output,error,nodes,state,run);
        return result;
    }
    return value;
}

void rename_edge_targets(Json::Value& node,const char* field,const std::string& from,const std::string& to){
    if(!node.isMember(field))return;
    Json::Value& value=node[field];
    if(value.isString()){if(value.asString()==from)value=to;return;}
    if(value.isObject()){if(value.get("to","").asString()==from)value["to"]=to;return;}
    if(value.isArray())for(Json::ArrayIndex i=0;i<value.size();++i){
        if(value[i].isString()&&value[i].asString()==from)value[i]=to;
        else if(value[i].isObject()&&value[i].get("to","").asString()==from)value[i]["to"]=to;
    }
}

} // namespace

class Value::Impl {
public:
    Json::Value value;
    Impl() : value() {}
    explicit Impl(const Json::Value& v) : value(v) {}
};

Value::Value() : impl_(new Impl()) {}
Value::Value(bool v) : impl_(new Impl(Json::Value(v))) {}
Value::Value(std::int64_t v) : impl_(new Impl(Json::Value(static_cast<Json::Int64>(v)))) {}
Value::Value(double v) : impl_(new Impl(Json::Value(v))) {}
Value::Value(const std::string& v) : impl_(new Impl(Json::Value(v))) {}
Value::Value(const Impl& v) : impl_(new Impl(v.value)) {}
Value::~Value() {}
Value::Value(const Value& v) : impl_(new Impl(v.impl_->value)) {}
Value& Value::operator=(const Value& v) { if (this != &v) impl_.reset(new Impl(v.impl_->value)); return *this; }
Value::Value(Value&& v) noexcept : impl_(std::move(v.impl_)) { if (!impl_) impl_.reset(new Impl()); }
Value& Value::operator=(Value&& v) noexcept { if(this!=&v){impl_=std::move(v.impl_);if(!impl_)impl_.reset(new Impl());}return *this; }
Value Value::parse(const std::string& json) { Impl i(parse_json(json)); return Value(i); }
Value Value::object() { Impl i(Json::Value(Json::objectValue)); return Value(i); }
Value Value::array() { Impl i(Json::Value(Json::arrayValue)); return Value(i); }
Value::Type Value::type() const {
    if(impl_->value.isNull())return Type::Null;
    if(impl_->value.isBool())return Type::Boolean;
    if(impl_->value.isInt64()||impl_->value.isUInt64())return Type::Integer;
    if(impl_->value.isDouble())return Type::Double;
    if(impl_->value.isString())return Type::String;
    if(impl_->value.isArray())return Type::Array;
    return Type::Object;
}
bool Value::is_null() const{return impl_->value.isNull();}
bool Value::as_bool() const{return impl_->value.asBool();}
std::int64_t Value::as_integer() const{return impl_->value.asInt64();}
double Value::as_double() const{return impl_->value.asDouble();}
std::string Value::as_string() const{return impl_->value.asString();}
std::string Value::to_json(bool styled) const{return write_json(impl_->value,styled);}
bool Value::has(const std::string& k) const{return impl_->value.isMember(k);}
Value Value::get(const std::string& k) const{Impl i(impl_->value[k]);return Value(i);}
void Value::set(const std::string& k,const Value& v){impl_->value[k]=v.impl_->value;}
void Value::append(const Value& v){impl_->value.append(v.impl_->value);}
std::size_t Value::size()const{return impl_->value.size();}

ExecutionResult ExecutionResult::ok(const Value& output) {
    ExecutionResult r; r.success=true; r.output=output; r.error.attempt=0;r.error.retryable=false;return r;
}
ExecutionResult ExecutionResult::fail(const std::string& cat,const std::string& code,
                                      const std::string& msg,bool retryable) {
    ExecutionResult r;r.success=false;r.error.category=cat;r.error.code=code;r.error.message=msg;
    r.error.attempt=0;r.error.retryable=retryable;return r;
}

Result<TraceReplayPlan> prepare_trace_replay(const std::vector<EventEnvelope>& events,
                                             const std::string& expected_workflow_digest,
                                             const std::string& expected_bundle_digest){
    auto failure=[](const std::string& code,const std::string& message){
        Error error;error.category="trace_replay";error.code=code;error.message=message;
        return Result<TraceReplayPlan>::failure(error);};
    TraceReplayPlan plan;bool started=false,finished=false;std::map<std::string,std::uint64_t> previous;
    for(const EventEnvelope& event:events){
        if(!started){
            if(event.event!="run_started")continue;
            if(event.schema_version!=1)return failure("REPLAY_SCHEMA_UNSUPPORTED","unsupported trace schema");
            if(event.workflow_digest!=expected_workflow_digest)return failure("REPLAY_WORKFLOW_MISMATCH","trace Workflow digest does not match");
            if(!expected_bundle_digest.empty()&&event.bundle_digest!=expected_bundle_digest)
                return failure("REPLAY_BUNDLE_MISMATCH","trace Bundle digest does not match");
            Json::Value payload;try{payload=parse_json(event.payload_json);}catch(...){
                return failure("REPLAY_FULL_TRACE_REQUIRED","run_started payload is missing or invalid");}
            if(!payload.isMember("workflow_input"))
                return failure("REPLAY_FULL_TRACE_REQUIRED","Full trace with workflow input is required");
            plan.source_run_id=event.run_id;plan.trace_id=event.trace_id;plan.workflow_digest=event.workflow_digest;
            plan.bundle_digest=event.bundle_digest;plan.workflow_input=Value::parse(write_json(payload["workflow_input"],false));
            started=true;previous[event.run_id]=event.sequence;continue;
        }
        if(event.trace_id!=plan.trace_id)continue;
        auto prior=previous.find(event.run_id);
        if(prior==previous.end()){if(event.event!="run_started")
                return failure("REPLAY_TRACE_INCOMPLETE","child run is missing run_started");
            previous[event.run_id]=event.sequence;continue;}
        if(event.sequence<=prior->second)return failure("REPLAY_SEQUENCE_INVALID","trace sequence is not strictly increasing");
        if(event.sequence!=prior->second+1)return failure("REPLAY_TRACE_INCOMPLETE","trace contains a sequence gap");
        prior->second=event.sequence;
        if(event.event=="node_succeeded"&&!event.executor.empty()){
            if(event.invocation_id.empty())return failure("REPLAY_INVOCATION_ID_REQUIRED","executor event has no invocation identity");
            if(event.payload_json.empty())return failure("REPLAY_FULL_TRACE_REQUIRED","executor output is missing");
            TraceReplayOutcome outcome;outcome.invocation_id=event.invocation_id;
            outcome.node_id=event.node_id;outcome.attempt=event.attempt;
            try{outcome.result=ExecutionResult::ok(Value::parse(event.payload_json));}
            catch(...){return failure("REPLAY_PAYLOAD_INVALID","executor output is invalid JSON");}
            plan.executor_outcomes.push_back(std::move(outcome));
        }else if(event.event=="node_failed"&&!event.executor.empty()){
            if(event.invocation_id.empty())return failure("REPLAY_INVOCATION_ID_REQUIRED","executor event has no invocation identity");
            Json::Value payload;try{payload=parse_json(event.payload_json);}catch(...){
                return failure("REPLAY_FULL_TRACE_REQUIRED","executor failure details are missing");}
            TraceReplayOutcome outcome;outcome.invocation_id=event.invocation_id;
            outcome.node_id=event.node_id;outcome.attempt=event.attempt;
            outcome.result=ExecutionResult::fail(payload.get("category","execution_error").asString(),
                payload.get("code","REPLAY_FAILURE").asString(),payload.get("message","Recorded failure.").asString(),
                payload.get("retryable",false).asBool());
            plan.executor_outcomes.push_back(std::move(outcome));
        }else if(event.event=="run_completed"&&event.run_id==plan.source_run_id){
            Json::Value payload;try{payload=parse_json(event.payload_json);}catch(...){
                return failure("REPLAY_FULL_TRACE_REQUIRED","final result is missing");}
            if(payload.get("success",false).asBool())
                plan.expected_result=ExecutionResult::ok(Value::parse(write_json(payload["output"],false)));
            else plan.expected_result=ExecutionResult::fail(payload["error"].get("category","execution_error").asString(),
                payload["error"].get("code","RUN_FAILED").asString(),payload["error"].get("message","Recorded failure.").asString());
            finished=true;
        }
    }
    if(!started)return failure("REPLAY_RUN_NOT_FOUND","trace has no run_started event");
    if(!finished)return failure("REPLAY_TRACE_INCOMPLETE","trace has no run_completed event");
    return Result<TraceReplayPlan>::success(std::move(plan));
}

Result<ShadowEvaluationReport> compare_shadow_evaluation(
    const EvaluationSample& baseline,const EvaluationSample& candidate,
    const std::vector<MetricDefinition>& metrics){
    auto failure=[](const std::string& code,const std::string& message){
        Error error;error.category="evaluation";error.code=code;error.message=message;
        return Result<ShadowEvaluationReport>::failure(error);};
    if(metrics.empty())return failure("METRICS_REQUIRED","at least one metric is required");
    if(baseline.workflow_digest.empty()||candidate.workflow_digest.empty())
        return failure("WORKFLOW_IDENTITY_REQUIRED","both samples require Workflow digests");
    ShadowEvaluationReport report;report.baseline_label=baseline.label;report.candidate_label=candidate.label;
    report.outputs_equal=baseline.result.success==candidate.result.success&&
        (baseline.result.success?baseline.result.output.to_json(false)==candidate.result.output.to_json(false):
         (baseline.result.error.category==candidate.result.error.category&&baseline.result.error.code==candidate.result.error.code));
    for(const EventEnvelope& event:candidate.trace)if(event.event=="run_started"){
        try{report.candidate_is_shadow=parse_json(event.payload_json).get("run_mode","").asString()=="shadow";}catch(...){}
        break;
    }
    if(!report.candidate_is_shadow)return failure("CANDIDATE_NOT_SHADOW","candidate trace must identify a Shadow run");
    bool regressed=false;
    for(const MetricDefinition& definition:metrics){
        if(definition.name.empty()||!definition.evaluate||definition.weight<0.0||definition.regression_tolerance<0.0)
            return failure("METRIC_INVALID","metric name, evaluator, weight, or tolerance is invalid");
        Result<double> base=definition.evaluate(baseline);if(!base)
            return failure("METRIC_BASELINE_FAILED",definition.name+": "+base.error().message);
        Result<double> next=definition.evaluate(candidate);if(!next)
            return failure("METRIC_CANDIDATE_FAILED",definition.name+": "+next.error().message);
        if(!std::isfinite(base.value())||!std::isfinite(next.value()))
            return failure("METRIC_NON_FINITE",definition.name+" returned a non-finite value");
        MetricComparison comparison;comparison.name=definition.name;comparison.baseline=base.value();
        comparison.candidate=next.value();comparison.delta=next.value()-base.value();
        comparison.weighted_delta=comparison.delta*definition.weight;
        comparison.regressed=comparison.delta < -definition.regression_tolerance;
        regressed=regressed||comparison.regressed;report.weighted_score_delta+=comparison.weighted_delta;
        report.metrics.push_back(std::move(comparison));
    }
    report.recommended=candidate.result.success&&!regressed&&report.weighted_score_delta>=0.0;
    return Result<ShadowEvaluationReport>::success(std::move(report));
}

class Workflow::Impl {
public:
    Json::Value root;
    std::shared_ptr<const WorkflowIR> compiled;
    std::string bundle_digest;
    Impl(const Json::Value& v,std::size_t max_ir_bytes)
        :root(v),compiled(compile_workflow_ir(write_json(v,false),max_ir_bytes)){}
};
Workflow::Workflow(std::unique_ptr<Impl> p):impl_(std::move(p)){}
Workflow::~Workflow(){}
Workflow::Workflow(Workflow&& v)noexcept:impl_(std::move(v.impl_)){}
Workflow& Workflow::operator=(Workflow&& v)noexcept{impl_=std::move(v.impl_);return *this;}
std::string Workflow::normalized_json()const{return write_json(impl_->root,true);}
const WorkflowIR& Workflow::ir()const noexcept{return *impl_->compiled;}
const std::string& Workflow::bundle_digest()const noexcept{return impl_->bundle_digest;}

std::string Workflow::export_mermaid()const {
    std::ostringstream s;s<<"flowchart TD\n";
    const Json::Value& nodes=impl_->root["nodes"];std::vector<std::string> names=nodes.getMemberNames();
    for(std::size_t i=0;i<names.size();++i){const Json::Value& n=nodes[names[i]];
        s<<"  "<<names[i]<<"[\""<<escape_mermaid(n.get("name",names[i]).asString())<<"\"]\n";
        const char* fields[]={"next","on_error"};
        for(int f=0;f<2;++f){std::vector<Json::Value> es=edges_of(n,fields[f]);
            for(std::size_t j=0;j<es.size();++j)s<<"  "<<names[i]<<(f?" -.-> ":" --> ")<<es[j]["to"].asString()<<"\n";}
        if(n["type"].asString()=="parallel"){for(Json::ArrayIndex j=0;j<n["branches"].size();++j)s<<"  "<<names[i]<<" --> "<<n["branches"][j].asString()<<"\n";}
    }return s.str();
}
std::string Workflow::export_dot()const {
    std::ostringstream s;s<<"digraph DAGE {\n";const Json::Value& nodes=impl_->root["nodes"];
    std::vector<std::string> names=nodes.getMemberNames();
    for(std::size_t i=0;i<names.size();++i){const Json::Value& n=nodes[names[i]];
        s<<"  \""<<escape_dot(names[i])<<"\" [label=\""<<escape_dot(n.get("name",names[i]).asString())<<"\"];\n";
        const char* fields[]={"next","on_error"};for(int f=0;f<2;++f){std::vector<Json::Value> es=edges_of(n,fields[f]);
        for(std::size_t j=0;j<es.size();++j)s<<"  \""<<escape_dot(names[i])<<"\" -> \""<<escape_dot(es[j]["to"].asString())<<"\""<<(f?" [style=dashed]":"")<<";\n";}
    }s<<"}\n";return s.str();
}

class Engine::Impl {
public:
    explicit Impl(std::shared_ptr<Scheduler> value)
        : scheduler(std::move(value)),store(std::make_shared<MemoryStateStore>()),
          timers(std::make_shared<TimerCoordinator>()) {}
    std::map<std::string,ExecutorFunction> executors;
    std::map<std::string,AsyncExecutorFunction> async_executors;
    std::map<std::string,std::shared_ptr<const WorkflowIR>> workflows;
    ExecutorPolicy policy;
    EventCallback events;
    std::shared_ptr<TraceSink> trace_sink;
    std::shared_ptr<ResourceLeaseProvider> lease_provider;
    std::shared_ptr<Scheduler> scheduler;
    std::shared_ptr<StateStore> store;
    std::shared_ptr<TimerCoordinator> timers;
    std::function<bool(const std::string&,const std::string&)> failure_injector;
    WorkflowResourceLimits workflow_limits;
};
Engine::Engine():Engine(std::make_shared<ThreadPoolScheduler>()){}
Engine::Engine(std::shared_ptr<Scheduler> scheduler):impl_(new Impl(std::move(scheduler))){
    if(!impl_->scheduler)throw std::invalid_argument("scheduler must not be null");
}
Engine::~Engine(){}
Engine::Engine(Engine&& v)noexcept:impl_(std::move(v.impl_)){}
Engine& Engine::operator=(Engine&& v)noexcept{impl_=std::move(v.impl_);return *this;}
void Engine::register_executor(const std::string& n,const ExecutorFunction& e){
    if(!valid_id(n)||!e)throw std::invalid_argument("invalid executor");
    impl_->executors[n]=e;
}
void Engine::register_async_executor(const std::string& n,const AsyncExecutorFunction& e){
    if(!valid_id(n)||!e)throw std::invalid_argument("invalid async executor");
    impl_->async_executors[n]=e;impl_->executors.erase(n);
}
void Engine::register_workflow(const std::string& name,const std::string& json){
    if(!valid_id(name))throw std::invalid_argument("invalid workflow name");
    std::unique_ptr<Workflow> workflow=load(json);impl_->workflows[name]=workflow->impl_->compiled;
}
void Engine::set_executor_policy(const ExecutorPolicy& p){impl_->policy=p;}
void Engine::set_event_callback(const EventCallback& c){impl_->events=c;}
void Engine::set_trace_sink(std::shared_ptr<TraceSink> sink){impl_->trace_sink=std::move(sink);}
void Engine::set_resource_lease_provider(std::shared_ptr<ResourceLeaseProvider> provider){impl_->lease_provider=std::move(provider);}
void Engine::set_scheduler(std::shared_ptr<Scheduler> scheduler){
    if(!scheduler)throw std::invalid_argument("scheduler must not be null");
    impl_->scheduler=std::move(scheduler);
}
void Engine::set_state_store(std::shared_ptr<StateStore> store){
    if(!store)throw std::invalid_argument("state store must not be null");
    impl_->store=std::move(store);
}
void Engine::set_workflow_resource_limits(const WorkflowResourceLimits& limits){
    if(!limits.max_workflow_bytes||!limits.max_json_depth||!limits.max_nodes||!limits.max_edges||
       !limits.max_expression_bytes||!limits.max_literal_bytes||!limits.max_compiled_ir_bytes)
        throw std::invalid_argument("workflow resource limits must be positive");
    impl_->workflow_limits=limits;
}
void Engine::set_failure_injector(const std::function<bool(const std::string&,const std::string&)>& injector){
    impl_->failure_injector=injector;
}

std::vector<Diagnostic> Engine::validate(const std::string& text)const {
    std::vector<Diagnostic> d;Json::Value root;
    try{root=parse_workflow_json(text,impl_->workflow_limits);}catch(const std::exception& e){
        const std::string message=e.what();const bool limited=message.find("WORKFLOW_")==0;
        add_diag(d,Severity::Error,limited?message.substr(0,message.find(':')):"INVALID_JSON","/",message);return d;}
    if(!root.isObject()){add_diag(d,Severity::Error,"INVALID_ROOT","/","Root must be an object.");return d;}
    const std::set<std::string> top={"format","format_version","id","name","description","entry","input","output","limits","defaults","nodes"};
    std::vector<std::string> topnames=root.getMemberNames();for(std::size_t i=0;i<topnames.size();++i)
        if(top.count(topnames[i])==0&&topnames[i].substr(0,2)!="x_")add_diag(d,Severity::Error,"UNKNOWN_FIELD","/"+topnames[i],"Unknown field.");
    if(root.get("format","").asString()!="dage-workflow")add_diag(d,Severity::Error,"INVALID_WORKFLOW_FORMAT","/format","format must be dage-workflow.");
    if(root.get("format_version","").asString()!="0.2.0")add_diag(d,Severity::Error,"UNSUPPORTED_VERSION","/format_version","Only 0.2.0 is supported.");
    if(!root["nodes"].isObject()||root["nodes"].empty()){add_diag(d,Severity::Error,"MISSING_NODES","/nodes","nodes must be a non-empty object.");return d;}
    const Json::Value& nodes=root["nodes"];std::vector<std::string> names=nodes.getMemberNames();
    if(names.size()>impl_->workflow_limits.max_nodes)add_diag(d,Severity::Error,"WORKFLOW_NODES_LIMIT","/nodes",
        "observed="+std::to_string(names.size())+" limit="+std::to_string(impl_->workflow_limits.max_nodes));
    std::size_t literal_bytes=0,expression_bytes=0;measure_workflow_strings(root,literal_bytes,expression_bytes);
    if(literal_bytes>impl_->workflow_limits.max_literal_bytes)add_diag(d,Severity::Error,"WORKFLOW_LITERAL_BYTES_LIMIT","/",
        "observed="+std::to_string(literal_bytes)+" limit="+std::to_string(impl_->workflow_limits.max_literal_bytes));
    if(expression_bytes>impl_->workflow_limits.max_expression_bytes)add_diag(d,Severity::Error,"WORKFLOW_EXPRESSION_BYTES_LIMIT","/",
        "observed="+std::to_string(expression_bytes)+" limit="+std::to_string(impl_->workflow_limits.max_expression_bytes));
    const std::string entry=root.get("entry","").asString();
    if(entry.empty()||!nodes.isMember(entry))add_diag(d,Severity::Error,"MISSING_ENTRY","/entry","entry must reference an existing node.");
    else if(!nodes[entry].get("enabled",true).asBool())add_diag(d,Severity::Error,"DISABLED_ENTRY","/entry","entry cannot be disabled.");
    std::map<std::string,std::vector<std::string> > graph;std::size_t edge_count=0;
    for(std::size_t i=0;i<names.size();++i){const std::string id=names[i];const Json::Value& n=nodes[id];const std::string path="/nodes/"+id;
        if(!valid_id(id))add_diag(d,Severity::Error,"INVALID_NODE_ID",path,"Node ID must match [a-z][a-z0-9_]*.");
        if(!n.isObject()){add_diag(d,Severity::Error,"INVALID_NODE",path,"Node must be an object.");continue;}
        const std::string type=n.get("type","").asString();if(!known_type(type))add_diag(d,Severity::Error,"UNKNOWN_NODE_TYPE",path+"/type","Unknown node type.");
        if(needs_executor(type)&&n.get("executor","").asString().empty())add_diag(d,Severity::Error,"MISSING_EXECUTOR",path+"/executor","This node type requires executor.");
        if((type=="tool"||type=="custom")&&!n.isMember("effects"))
            add_diag(d,Severity::Error,"MISSING_EFFECTS",path+"/effects","tool and custom nodes must declare effects.");
        if(n.isMember("effects")){
            const Json::Value& effects=n["effects"];
            const std::string kind=effects.get("kind","").asString();
            const std::string replay=effects.get("replay","").asString();
            const std::set<std::string> effect_kinds={"pure","local_state","external_read","external_write","irreversible"};
            const std::set<std::string> replay_policies={"safe","idempotent","at_most_once","manual","forbidden"};
            if(!effects.isObject())add_diag(d,Severity::Error,"INVALID_EFFECTS",path+"/effects","effects must be an object.");
            else{
                if(effect_kinds.count(kind)==0)add_diag(d,Severity::Error,"INVALID_EFFECT_KIND",path+"/effects/kind","Unknown effect kind.");
                if(replay_policies.count(replay)==0)add_diag(d,Severity::Error,"INVALID_REPLAY_POLICY",path+"/effects/replay","Unknown replay policy.");
                if(kind=="pure"&&replay!="safe")add_diag(d,Severity::Error,"INVALID_EFFECT_REPLAY",path+"/effects","pure nodes must use safe replay.");
                if(kind=="external_write"&&replay=="safe")add_diag(d,Severity::Error,"INVALID_EFFECT_REPLAY",path+"/effects","external_write cannot use safe replay.");
                if(kind=="irreversible"&&replay!="manual"&&replay!="forbidden")
                    add_diag(d,Severity::Error,"INVALID_EFFECT_REPLAY",path+"/effects","irreversible nodes require manual or forbidden replay.");
                if(replay=="idempotent"&&effects.get("idempotency_key","").asString().empty()&&
                   !effects.get("executor_guarantees_idempotency",false).asBool())
                    add_diag(d,Severity::Error,"MISSING_IDEMPOTENCY_GUARANTEE",path+"/effects","idempotent replay requires idempotency_key or executor guarantee.");
                if(n["retry"].get("max_attempts",1).asUInt()>1&&
                   (replay=="at_most_once"||replay=="manual"||replay=="forbidden"))
                    add_diag(d,Severity::Error,"UNSAFE_RETRY_POLICY",path+"/retry","retry is not allowed by the node replay policy.");
            }
        }
        if(type=="subflow"&&n["config"].get("workflow","").asString().empty())add_diag(d,Severity::Error,"MISSING_SUBFLOW",path+"/config/workflow","subflow requires config.workflow.");
        if(n.isMember("timeout_ms")&&n["timeout_ms"].asInt64()<=0)add_diag(d,Severity::Error,"INVALID_TIMEOUT",path+"/timeout_ms","timeout_ms must be positive.");
        if(n.isMember("retry")){const Json::Value&r=n["retry"];if(!r.isObject()||r.get("max_attempts",1).asInt()<1)add_diag(d,Severity::Error,"INVALID_RETRY",path+"/retry","max_attempts must be >= 1.");
            const std::string backoff=r.get("backoff","fixed").asString();if(backoff!="fixed"&&backoff!="exponential")
                add_diag(d,Severity::Error,"INVALID_BACKOFF",path+"/retry/backoff","backoff must be fixed or exponential.");
            if(r.get("jitter_ms",0).asInt64()<0)add_diag(d,Severity::Error,"INVALID_JITTER",path+"/retry/jitter_ms","jitter_ms must be non-negative.");}
        if(type=="end"&&(n.isMember("next")||n.isMember("on_error")))add_diag(d,Severity::Error,"END_HAS_SUCCESSOR",path,"end cannot have next or on_error.");
        const char* fs[]={"next","on_error"};for(int f=0;f<2;++f){std::vector<Json::Value> es=edges_of(n,fs[f]);edge_count+=es.size();int otherwise_count=0;
            for(std::size_t j=0;j<es.size();++j){const Json::Value&e=es[j];const std::string ep=path+"/"+fs[f]+"/"+std::to_string(j);
                if(!e.isObject()||!e["to"].isString()){add_diag(d,Severity::Error,"INVALID_EDGE",ep,"Edge must contain string 'to'.");continue;}
                const std::string to=e["to"].asString();if(!nodes.isMember(to))add_diag(d,Severity::Error,"UNKNOWN_NODE_REFERENCE",ep+"/to","Referenced node does not exist.");
                else graph[id].push_back(to);
                if(e.get("otherwise",false).asBool()){++otherwise_count;if(j+1!=es.size())add_diag(d,Severity::Error,"OTHERWISE_NOT_LAST",ep,"otherwise must be last.");if(e.isMember("when"))add_diag(d,Severity::Error,"OTHERWISE_WITH_WHEN",ep,"otherwise cannot have when.");}
                if(!e.isMember("when")&&!e.get("otherwise",false).asBool()&&j+1<es.size())add_diag(d,Severity::Error,"SHADOWED_BRANCH",ep,"Unconditional edge shadows following candidates.");
                if(e.get("mode","normal").asString()=="jump_back"&&e.get("max_hits",0).asInt()<1)add_diag(d,Severity::Error,"UNBOUNDED_JUMP_BACK",ep,"jump_back requires max_hits.");
                if(e.isMember("loop")&&e["loop"].get("max_iterations",0).asInt()<1)add_diag(d,Severity::Error,"INVALID_LOOP",ep+"/loop","loop requires positive max_iterations.");
            }if(otherwise_count>1)add_diag(d,Severity::Error,"MULTIPLE_OTHERWISE",path+"/"+fs[f],"At most one otherwise is allowed.");
        }
        if(type=="parallel"){if(!n["branches"].isArray()||n["branches"].empty()||!n["join"].isString())add_diag(d,Severity::Error,"INVALID_PARALLEL",path,"parallel requires branches and join.");
            edge_count+=n["branches"].size()+(n["join"].isString()?1:0);
            for(Json::ArrayIndex j=0;j<n["branches"].size();++j){std::string to=n["branches"][j].asString();if(!nodes.isMember(to))add_diag(d,Severity::Error,"UNKNOWN_NODE_REFERENCE",path+"/branches","Parallel branch does not exist.");else graph[id].push_back(to);}
            if(n["join"].isString()&&!nodes.isMember(n["join"].asString()))add_diag(d,Severity::Error,"UNKNOWN_NODE_REFERENCE",path+"/join","Join does not exist.");
            else if(n["join"].isString())graph[id].push_back(n["join"].asString());
        }
    }
    if(edge_count>impl_->workflow_limits.max_edges)add_diag(d,Severity::Error,"WORKFLOW_EDGES_LIMIT","/nodes",
        "observed="+std::to_string(edge_count)+" limit="+std::to_string(impl_->workflow_limits.max_edges));
    std::set<std::string> reached;std::vector<std::string> stack;if(nodes.isMember(entry))stack.push_back(entry);
    while(!stack.empty()){std::string x=stack.back();stack.pop_back();if(!reached.insert(x).second)continue;
        for(std::size_t j=0;j<graph[x].size();++j)stack.push_back(graph[x][j]);}
    for(std::size_t i=0;i<names.size();++i)if(reached.count(names[i])==0)add_diag(d,Severity::Warning,"UNREACHABLE_NODE","/nodes/"+names[i],"Node is unreachable.");
    // Every cyclic strongly reachable path must contain an explicitly bounded edge.
    std::function<void(const std::string&,std::set<std::string>&)> dfs;
    dfs=[&](const std::string& id,std::set<std::string>& active){active.insert(id);const Json::Value& n=nodes[id];
        const char* fs[]={"next","on_error"};for(int f=0;f<2;++f){std::vector<Json::Value> es=edges_of(n,fs[f]);for(std::size_t j=0;j<es.size();++j){
            std::string to=es[j].get("to","").asString();if(active.count(to)&&!es[j].isMember("loop")&&es[j].get("max_hits",0).asInt()<1)
                add_diag(d,Severity::Error,"UNDECLARED_CYCLE","/nodes/"+id+"/"+fs[f],"Cycle edge requires loop or max_hits.");
            else if(nodes.isMember(to)&&active.count(to)==0)dfs(to,active);
        }}active.erase(id);
    };if(nodes.isMember(entry)){std::set<std::string>a;dfs(entry,a);}
    return d;
}

std::unique_ptr<Workflow> Engine::load(const std::string& text)const {
    std::vector<Diagnostic>d=validate(text);for(std::size_t i=0;i<d.size();++i)if(d[i].severity==Severity::Error)
        throw std::runtime_error(d[i].code+": "+d[i].message);
    Json::Value root=parse_workflow_json(text,impl_->workflow_limits);Json::Value& nodes=root["nodes"];std::vector<std::string> names=nodes.getMemberNames();
    for(std::size_t i=0;i<names.size();++i){Json::Value& n=nodes[names[i]];if(!n.isMember("name"))n["name"]=names[i];if(!n.isMember("enabled"))n["enabled"]=true;
        const std::string type=n.get("type","").asString();
        if(!n.isMember("effects")&&type=="llm"){n["effects"]["kind"]="external_read";n["effects"]["replay"]="safe";}
        if(!n.isMember("effects")&&(type=="transform"||type=="condition"||type=="noop")){
            n["effects"]["kind"]="pure";n["effects"]["replay"]="safe";
        }
        if(!n.isMember("timeout_ms")&&root["defaults"].isMember("timeout_ms"))n["timeout_ms"]=root["defaults"]["timeout_ms"];
        if(!n.isMember("retry")&&root["defaults"].isMember("retry"))n["retry"]=root["defaults"]["retry"];
    }
    return std::unique_ptr<Workflow>(new Workflow(std::unique_ptr<Workflow::Impl>(
        new Workflow::Impl(root,impl_->workflow_limits.max_compiled_ir_bytes))));
}

std::unique_ptr<Bundle> Engine::load_bundle(const ResourceProvider& provider)const{
    return load_bundle_from_provider(provider);
}

std::unique_ptr<Workflow> Engine::load_workflow(const Bundle& bundle,const std::string& workflow_id)const{
    Result<std::string> text=bundle_workflow_json(bundle,workflow_id);
    if(!text)throw std::runtime_error(text.error().code+": "+text.error().message);
    std::unique_ptr<Workflow> workflow=load(text.value());workflow->impl_->bundle_digest=bundle.digest();return workflow;
}

std::unique_ptr<Workflow> Engine::apply_patch(const Workflow& workflow,const std::string& patch_json)const{
    Json::Value root=workflow.impl_->root;Json::Value patch=parse_json(patch_json);
    if(!patch.isObject())throw PatchValidationError("PATCH_INVALID_DOCUMENT","/","Patch must be an object.");
    const std::string expected=workflow.impl_->compiled->digest();
    if(!patch["base_digest"].isString()||patch["base_digest"].asString().empty())
        throw PatchValidationError("PATCH_BASE_DIGEST_REQUIRED","/base_digest","base_digest is required.");
    if(patch["base_digest"].asString()!=expected)
        throw PatchValidationError("PATCH_BASE_DIGEST_MISMATCH","/base_digest","Patch targets a different Workflow.");
    const Json::UInt64 revision=root.get("x_revision",0).asUInt64();
    if(patch.isMember("base_revision")){
        if(!patch["base_revision"].isUInt64()&&!patch["base_revision"].isUInt())
            throw PatchValidationError("PATCH_INVALID_BASE_REVISION","/base_revision","base_revision must be an unsigned integer.");
        if(patch["base_revision"].asUInt64()!=revision)
            throw PatchValidationError("PATCH_BASE_REVISION_MISMATCH","/base_revision","Patch targets a different revision.");
    }
    if(!patch["changes"].isArray()||patch["changes"].empty())
        throw PatchValidationError("PATCH_INVALID_CHANGES","/changes","changes must be a non-empty array.");
    for(Json::ArrayIndex i=0;i<patch["changes"].size();++i){
        const Json::Value& change=patch["changes"][i];const std::string action=change["action"].asString();
        const std::string id=change["node_id"].asString();
        const std::string path="/changes/"+std::to_string(i);
        auto fail=[&](const std::string& code,const std::string& suffix,const std::string& message){
            throw PatchValidationError(code,path+suffix,message);};
        if(!change.isObject())fail("PATCH_CHANGE_NOT_OBJECT","","Change must be an object.");
        if(action.empty())fail("PATCH_ACTION_REQUIRED","/action","action is required.");
        if(id.empty()&&action!="set_entry")fail("PATCH_NODE_ID_REQUIRED","/node_id","node_id is required.");
        if(action=="set_entry"){if(!change["value"].isString()||!root["nodes"].isMember(change["value"].asString()))fail("PATCH_ENTRY_NOT_FOUND","/value","set_entry requires an existing node.");root["entry"]=change["value"];}
        else if(action=="add_node"){if(!change["node"].isObject())fail("PATCH_NODE_NOT_OBJECT","/node","node must be an object.");if(root["nodes"].isMember(id))fail("PATCH_NODE_ALREADY_EXISTS","/node_id","Node already exists.");root["nodes"][id]=change["node"];}
        else if(action=="remove_node"){if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"].removeMember(id);}
        else if(action=="replace_node"){if(!change["node"].isObject())fail("PATCH_NODE_NOT_OBJECT","/node","node must be an object.");if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"][id]=change["node"];}
        else if(action=="rename_node"){
            const std::string to=change["new_node_id"].asString();
            if(to.empty())fail("PATCH_NEW_NODE_ID_REQUIRED","/new_node_id","new_node_id is required.");
            if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");
            if(root["nodes"].isMember(to))fail("PATCH_NODE_ALREADY_EXISTS","/new_node_id","Target node already exists.");
            root["nodes"][to]=root["nodes"][id];root["nodes"].removeMember(id);
            if(root["entry"].asString()==id)root["entry"]=to;
            std::vector<std::string> names=root["nodes"].getMemberNames();
            for(std::size_t j=0;j<names.size();++j){Json::Value& n=root["nodes"][names[j]];
                rename_edge_targets(n,"next",id,to);rename_edge_targets(n,"on_error",id,to);
                if(n.get("join","").asString()==id)n["join"]=to;
                if(n["branches"].isArray())for(Json::ArrayIndex k=0;k<n["branches"].size();++k)if(n["branches"][k].asString()==id)n["branches"][k]=to;
            }
        }else if(action=="set_field"){if(!change["field"].isString()||change["field"].asString().empty())fail("PATCH_FIELD_REQUIRED","/field","field must be a non-empty string.");if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"][id][change["field"].asString()]=change["value"];}
        else if(action=="remove_field"){if(!change["field"].isString()||change["field"].asString().empty())fail("PATCH_FIELD_REQUIRED","/field","field must be a non-empty string.");if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");if(!root["nodes"][id].isMember(change["field"].asString()))fail("PATCH_FIELD_NOT_FOUND","/field","Field does not exist.");root["nodes"][id].removeMember(change["field"].asString());}
        else if(action=="set_next"){if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"][id]["next"]=change["value"];}
        else if(action=="set_on_error"){if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"][id]["on_error"]=change["value"];}
        else if(action=="enable_node"){if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"][id]["enabled"]=true;}
        else if(action=="disable_node"){if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");root["nodes"][id]["enabled"]=false;}
        else if(action=="insert_next"){
            if(!root["nodes"].isMember(id))fail("PATCH_NODE_NOT_FOUND","/node_id","Node does not exist.");
            if(change.isMember("index")&&!change["index"].isUInt()&&!change["index"].isUInt64())
                fail("PATCH_INDEX_INVALID","/index","index must be an unsigned integer.");
            Json::Value& next=root["nodes"][id]["next"];if(!next.isArray()){Json::Value a(Json::arrayValue);if(!next.isNull())a.append(next);next=a;}
            const Json::ArrayIndex position=std::min<Json::ArrayIndex>(change.get("index",next.size()).asUInt(),next.size());
            Json::Value rebuilt(Json::arrayValue);for(Json::ArrayIndex j=0;j<position;++j)rebuilt.append(next[j]);
            rebuilt.append(change["value"]);for(Json::ArrayIndex j=position;j<next.size();++j)rebuilt.append(next[j]);next=rebuilt;
        }else if(action=="remove_next"){
            if(!root["nodes"].isMember(id)||(!change["index"].isUInt()&&!change["index"].isUInt64()))
                fail(!root["nodes"].isMember(id)?"PATCH_NODE_NOT_FOUND":"PATCH_INDEX_INVALID",
                     !root["nodes"].isMember(id)?"/node_id":"/index",
                     !root["nodes"].isMember(id)?"Node does not exist.":"index must be an unsigned integer.");
            Json::Value& next=root["nodes"][id]["next"];const Json::ArrayIndex index=change["index"].asUInt();
            if(!next.isArray()||index>=next.size())fail("PATCH_INDEX_OUT_OF_RANGE","/index","index is outside the next array.");
            Json::Value rebuilt(Json::arrayValue);for(Json::ArrayIndex j=0;j<next.size();++j)if(j!=index)rebuilt.append(next[j]);next=rebuilt;
        }else fail("PATCH_UNKNOWN_ACTION","/action","Unknown Patch action: "+action);
    }
    root["x_revision"]=root.get("x_revision",0).asUInt64()+1;
    return load(write_json(root,false));
}

PatchDryRunResult Engine::dry_run_patch(const Workflow& workflow,const std::string& patch_json)const{
    PatchDryRunResult result;result.valid=false;result.base_digest=workflow.impl_->compiled->digest();
    result.candidate_revision=workflow.impl_->root.get("x_revision",0).asUInt64()+1;
    try{
        const Json::Value document=parse_json(patch_json);
        if(document["changes"].isArray()){
            const std::set<std::string> actions={"set_entry","add_node","remove_node","replace_node",
                "rename_node","set_field","remove_field","set_next","set_on_error","enable_node",
                "disable_node","insert_next","remove_next"};
            for(Json::ArrayIndex i=0;i<document["changes"].size();++i){
                const Json::Value& change=document["changes"][i];const std::string base="/changes/"+std::to_string(i);
                auto add=[&](const std::string& code,const std::string& suffix,const std::string& message){
                    Diagnostic d;d.severity=Severity::Error;d.code=code;d.path=base+suffix;d.message=message;
                    d.suggestion="Fix this Patch change.";result.diagnostics.push_back(d);};
                if(!change.isObject()){add("PATCH_CHANGE_NOT_OBJECT","","Change must be an object.");continue;}
                if(!change["action"].isString()||change["action"].asString().empty()){
                    add("PATCH_ACTION_REQUIRED","/action","action is required.");continue;
                }
                const std::string action=change["action"].asString();
                if(!actions.count(action)){add("PATCH_UNKNOWN_ACTION","/action","Unknown Patch action: "+action);continue;}
                if(action!="set_entry"&&(!change["node_id"].isString()||change["node_id"].asString().empty()))
                    add("PATCH_NODE_ID_REQUIRED","/node_id","node_id is required.");
                if((action=="add_node"||action=="replace_node")&&!change["node"].isObject())
                    add("PATCH_NODE_NOT_OBJECT","/node","node must be an object.");
                if((action=="set_field"||action=="remove_field")&&
                   (!change["field"].isString()||change["field"].asString().empty()))
                    add("PATCH_FIELD_REQUIRED","/field","field must be a non-empty string.");
                if((action=="remove_next"&&!change["index"].isUInt()&&!change["index"].isUInt64())||
                   (action=="insert_next"&&change.isMember("index")&&!change["index"].isUInt()&&!change["index"].isUInt64()))
                    add("PATCH_INDEX_INVALID","/index","index must be an unsigned integer.");
                if(action=="set_entry"&&!change["value"].isString())
                    add("PATCH_ENTRY_NOT_FOUND","/value","set_entry value must be a node id.");
            }
            if(!result.diagnostics.empty())return result;
        }
    }catch(const std::exception&){}
    try{
        std::unique_ptr<Workflow> candidate=apply_patch(workflow,patch_json);
        result.valid=true;result.candidate_digest=candidate->impl_->compiled->digest();
    }catch(const PatchValidationError& ex){
        Diagnostic diagnostic;diagnostic.severity=Severity::Error;diagnostic.code=ex.code();
        diagnostic.path=ex.path();diagnostic.message=ex.message();
        diagnostic.suggestion="Fix this Patch field and retry against the current base digest.";
        result.diagnostics.push_back(diagnostic);
    }catch(const std::exception& ex){
        const std::string message=ex.what();const std::size_t colon=message.find(':');
        Diagnostic diagnostic;diagnostic.severity=Severity::Error;
        diagnostic.code=colon==std::string::npos?"PATCH_CANDIDATE_INVALID":message.substr(0,colon);
        diagnostic.path="/candidate";diagnostic.message=message;
        diagnostic.suggestion="Fix the candidate Workflow produced by this Patch.";
        result.diagnostics.push_back(diagnostic);
    }
    return result;
}

PatchAnalysis Engine::analyze_patch(const Workflow& workflow,const std::string& patch_json)const{
    PatchAnalysis analysis;analysis.valid=false;analysis.base_digest=workflow.impl_->compiled->digest();
    analysis.effect_policy_changed=false;analysis.checkpoint_resume_compatible=false;
    PatchDryRunResult dry=dry_run_patch(workflow,patch_json);
    if(!dry.valid){analysis.diagnostics=dry.diagnostics;return analysis;}
    std::unique_ptr<Workflow> candidate=apply_patch(workflow,patch_json);
    analysis.valid=true;analysis.candidate_digest=candidate->impl_->compiled->digest();
    const Json::Value& before=workflow.impl_->root;const Json::Value& after=candidate->impl_->root;
    std::set<std::string> direct;
    Json::Value inverse(Json::objectValue),changes(Json::arrayValue);
    inverse["base_digest"]=analysis.candidate_digest;
    inverse["base_revision"]=after.get("x_revision",0).asUInt64();
    if(before["entry"]!=after["entry"]){Json::Value c(Json::objectValue);c["action"]="set_entry";c["value"]=before["entry"];changes.append(c);}
    std::set<std::string> names;
    for(const std::string& id:before["nodes"].getMemberNames())names.insert(id);
    for(const std::string& id:after["nodes"].getMemberNames())names.insert(id);
    for(const std::string& id:names){
        const bool old_has=before["nodes"].isMember(id),new_has=after["nodes"].isMember(id);
        if(old_has&&new_has&&before["nodes"][id]==after["nodes"][id])continue;
        direct.insert(id);Json::Value c(Json::objectValue);c["node_id"]=id;
        if(!old_has){c["action"]="remove_node";}
        else if(!new_has){c["action"]="add_node";c["node"]=before["nodes"][id];}
        else{c["action"]="replace_node";c["node"]=before["nodes"][id];}
        changes.append(c);
        if(old_has&&new_has&&before["nodes"][id].get("effects",Json::Value())!=after["nodes"][id].get("effects",Json::Value()))
            analysis.effect_policy_changed=true;
        if(old_has!=new_has&&(old_has?before["nodes"][id]:after["nodes"][id]).isMember("effects"))
            analysis.effect_policy_changed=true;
    }
    inverse["changes"]=changes;analysis.inverse_patch_json=write_json(inverse,true);
    analysis.directly_changed_nodes.assign(direct.begin(),direct.end());
    std::set<std::string> affected=direct;bool expanded=true;
    while(expanded){expanded=false;
        for(const NodeIR& node:candidate->impl_->compiled->nodes()){
            bool depends=affected.count(node.id())>0;
            for(const EdgeIR& edge:node.next_edges())if(affected.count(node.id())&&affected.insert(edge.target()).second)expanded=true;
            for(const EdgeIR& edge:node.error_edges())if(affected.count(node.id())&&affected.insert(edge.target()).second)expanded=true;
            if(affected.count(node.id()))for(const std::string& branch:node.parallel_branches())
                if(affected.insert(branch).second)expanded=true;
            if(affected.count(node.id())&&!node.parallel_join().empty()&&affected.insert(node.parallel_join()).second)expanded=true;
            const std::string node_json=write_json(after["nodes"][node.id()],false);
            for(const std::string& changed:affected)
                if(node_json.find("${nodes."+changed+".")!=std::string::npos)depends=true;
            if(depends&&affected.insert(node.id()).second)expanded=true;
        }
    }
    analysis.affected_nodes.assign(affected.begin(),affected.end());
    analysis.checkpoint_resume_compatible=analysis.base_digest==analysis.candidate_digest;
    return analysis;
}

WorkflowDiff Engine::diff_workflows(const Workflow& before,const Workflow& after)const{
    WorkflowDiff diff;diff.before_digest=before.impl_->compiled->digest();
    diff.after_digest=after.impl_->compiled->digest();diff.effect_policy_changed=false;
    diff.checkpoint_resume_compatible=diff.before_digest==diff.after_digest;
    const Json::Value& old_root=before.impl_->root;const Json::Value& new_root=after.impl_->root;
    std::set<std::string> root_fields;
    for(const std::string& field:old_root.getMemberNames())if(field!="nodes"&&field!="x_revision")root_fields.insert(field);
    for(const std::string& field:new_root.getMemberNames())if(field!="nodes"&&field!="x_revision")root_fields.insert(field);
    for(const std::string& field:root_fields)if(old_root[field]!=new_root[field])diff.changed_workflow_fields.push_back(field);
    std::set<std::string> ids;
    for(const std::string& id:old_root["nodes"].getMemberNames())ids.insert(id);
    for(const std::string& id:new_root["nodes"].getMemberNames())ids.insert(id);
    std::set<std::string> affected;
    for(const std::string& id:ids){
        const bool old_has=old_root["nodes"].isMember(id),new_has=new_root["nodes"].isMember(id);
        if(old_has&&new_has&&old_root["nodes"][id]==new_root["nodes"][id])continue;
        WorkflowNodeDiff change;change.node_id=id;
        change.kind=!old_has?WorkflowNodeChangeKind::Added:(!new_has?WorkflowNodeChangeKind::Removed:WorkflowNodeChangeKind::Modified);
        std::set<std::string> fields;
        if(old_has)for(const std::string& field:old_root["nodes"][id].getMemberNames())fields.insert(field);
        if(new_has)for(const std::string& field:new_root["nodes"][id].getMemberNames())fields.insert(field);
        for(const std::string& field:fields)
            if(!old_has||!new_has||old_root["nodes"][id][field]!=new_root["nodes"][id][field])change.changed_fields.push_back(field);
        const Json::Value old_effects=old_has?old_root["nodes"][id].get("effects",Json::Value()):Json::Value();
        const Json::Value new_effects=new_has?new_root["nodes"][id].get("effects",Json::Value()):Json::Value();
        change.effect_policy_changed=old_effects!=new_effects;
        diff.effect_policy_changed=diff.effect_policy_changed||change.effect_policy_changed;
        diff.node_changes.push_back(std::move(change));affected.insert(id);
    }
    auto expand=[&](const Json::Value& root,const WorkflowIR& ir){
        bool changed=true;
        while(changed){changed=false;
            for(const NodeIR& node:ir.nodes()){
                bool depends=affected.count(node.id())>0;
                if(depends){
                    for(const EdgeIR& edge:node.next_edges())if(affected.insert(edge.target()).second)changed=true;
                    for(const EdgeIR& edge:node.error_edges())if(affected.insert(edge.target()).second)changed=true;
                    for(const std::string& branch:node.parallel_branches())if(affected.insert(branch).second)changed=true;
                    if(!node.parallel_join().empty()&&affected.insert(node.parallel_join()).second)changed=true;
                }
                const std::string json=write_json(root["nodes"][node.id()],false);
                for(const std::string& source:affected)if(json.find("${nodes."+source+".")!=std::string::npos){depends=true;break;}
                if(depends&&affected.insert(node.id()).second)changed=true;
            }
        }
    };
    expand(old_root,*before.impl_->compiled);expand(new_root,*after.impl_->compiled);
    diff.affected_nodes.assign(affected.begin(),affected.end());
    return diff;
}

class Run::Impl {
public:
    struct ReplayState {
        std::mutex mutex;
        std::map<std::string,std::deque<ExecutionResult> > outcomes;
    };
    struct ParallelContinuation {
        std::mutex mutex;
        std::string node;
        std::string join;
        std::string mode;
        std::string fail;
        std::vector<std::string> branches;
        std::size_t minimum=1;
        std::size_t max_parallel=1;
        std::size_t next_index=0;
        std::size_t in_flight=0;
        std::size_t success_count=0;
        std::size_t failure_count=0;
        bool batch_ready=false;
        bool terminal=false;
        ExecutionResult terminal_result=ExecutionResult::ok(Value::object());
        std::vector<std::pair<std::string,ExecutionResult> > batch_results;
        std::vector<std::shared_ptr<Run> > children;
    };
    std::shared_ptr<const WorkflowIR> workflow;
    std::map<std::string,ExecutorFunction> executors;
    std::map<std::string,AsyncExecutorFunction> async_executors;
    std::map<std::string,std::shared_ptr<const WorkflowIR>> workflows;
    ExecutorPolicy policy;EventCallback events;std::shared_ptr<TraceSink> trace_sink;
    std::shared_ptr<ResourceLeaseProvider> lease_provider;
    std::atomic<bool> cancelled;std::shared_ptr<CancellationToken> cancellation;
    std::shared_ptr<Scheduler> scheduler;
    std::shared_ptr<StateStore> store;
    std::shared_ptr<TimerCoordinator> timers;
    std::function<bool(const std::string&,const std::string&)> failure_injector;
    Json::Value workflow_input,outputs,state,error,run,resume_output;
    std::uint64_t step,elapsed_ms;
    std::string workflow_fingerprint;
    std::string bundle_digest;
    std::string invocation_id="root";
    std::string trace_id,current_span_id,parent_span_id,last_span_id,causation_id;
    std::map<std::string,int> hits,loops;
    std::string current;
    std::string stop_before;
    RunOptions options;
    std::map<std::string,NodeStatus> node_status;
    std::uint64_t event_sequence;
    std::uint64_t store_version;
    std::uint32_t retry_remaining;
    bool initialized,is_suspended,has_resume,completed;
    std::mutex drive_mutex;
    std::mutex continuation_mutex;
    bool async_pending=false;
    bool async_ready=false;
    std::uint64_t async_generation=0;
    std::uint32_t async_attempt=0;
    std::string async_node;
    ExecutionResult async_result=ExecutionResult::fail("internal_error","ASYNC_NOT_READY","Async result is not ready.");
    std::unique_ptr<ResourceLease> async_lease;
    std::shared_ptr<CancellationToken> async_attempt_cancellation;
    std::shared_ptr<TimerCoordinator::Handle> async_timer;
    std::shared_ptr<RuntimeEffectCommitter> async_committer;
    std::function<void()> resume_drive;
    RunCompletion run_completion;
    Value run_input=Value::object();
    bool callback_delivered=false;
    bool driving=false;
    std::shared_ptr<ParallelContinuation> parallel_continuation;
    std::shared_ptr<ReplayState> replay_state;
    bool deterministic_replay=false;
    std::string subflow_node;
    std::shared_ptr<Impl> subflow_child_state;
    bool subflow_pending=false;
    bool subflow_ready=false;
    ExecutionResult subflow_result=ExecutionResult::fail("internal_error","SUBFLOW_NOT_READY","Subflow result is not ready.");
    std::chrono::steady_clock::time_point started;
    Impl(std::shared_ptr<const WorkflowIR> r,const std::map<std::string,ExecutorFunction>&e,
         const std::map<std::string,AsyncExecutorFunction>&ae,
         const std::map<std::string,std::shared_ptr<const WorkflowIR>>&w,
         const ExecutorPolicy&p,const EventCallback&v,std::shared_ptr<TraceSink> trace,
         std::shared_ptr<ResourceLeaseProvider> leases,std::shared_ptr<Scheduler> s,
         std::shared_ptr<TimerCoordinator> timer_service,const RunOptions& o=RunOptions(),
         std::string bundle={},std::shared_ptr<StateStore> state_store={},std::function<bool(const std::string&,const std::string&)> injector={})
      :workflow(std::move(r)),executors(e),async_executors(ae),workflows(w),policy(p),events(v),trace_sink(std::move(trace)),lease_provider(std::move(leases)),cancelled(false),cancellation(std::make_shared<CancellationToken>()),scheduler(std::move(s)),store(std::move(state_store)),timers(std::move(timer_service)),failure_injector(std::move(injector)),outputs(Json::objectValue),
       state(Json::objectValue),error(Json::objectValue),run(Json::objectValue),step(0),elapsed_ms(0),
       workflow_fingerprint(workflow->digest()),bundle_digest(std::move(bundle)),options(o),event_sequence(0),store_version(0),retry_remaining(o.retry_budget),
       initialized(false),is_suspended(false),has_resume(false),completed(false){
        for(const NodeIR& node:workflow->nodes())node_status[node.id()]=NodeStatus::Pending;
       }
    void emit(const std::string& name,const std::string& node,std::uint32_t attempt=0,
              const ExecutionResult* result=nullptr,const std::string& payload={},
              TraceCapture payload_level=TraceCapture::Full){
        if(events)events(name,node);
        if(!trace_sink||options.trace_capture==TraceCapture::Off)return;
        EventEnvelope envelope;envelope.sequence=event_sequence;envelope.event=name;
        envelope.run_id=run.get("id","").asString();envelope.invocation_id=invocation_id;
        envelope.workflow_digest=workflow_fingerprint;
        envelope.bundle_digest=bundle_digest;
        envelope.trace_id=trace_id;envelope.span_id=current_span_id;
        envelope.parent_span_id=parent_span_id;envelope.causation_id=causation_id;
        envelope.node_id=node;const NodeIR* ir=workflow->find_node(node);
        if(ir){envelope.node_type=node_type_name(ir->type());envelope.executor=ir->executor();}envelope.attempt=attempt;
        envelope.timestamp_unix_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        if(initialized)envelope.elapsed_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-started).count());
        if(result&&!result->success){envelope.category=result->error.category;envelope.code=result->error.code;}
        if(static_cast<int>(options.trace_capture)>=static_cast<int>(payload_level))envelope.payload_json=payload;
        trace_sink->emit(envelope);
    }
};

class AsyncExecutorCompletion::Impl {
public:
    std::mutex mutex;
    bool done=false;
    std::shared_ptr<const CancellationToken> cancellation;
    std::function<void(const ExecutionResult&)> callback;
};

AsyncExecutorCompletion::AsyncExecutorCompletion(std::shared_ptr<Impl> impl):impl_(std::move(impl)){}
bool AsyncExecutorCompletion::complete(const ExecutionResult& result) noexcept {
    std::function<void(const ExecutionResult&)> callback;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->done)return false;
        impl_->done=true;callback=impl_->callback;
    }
    try{if(callback)callback(result);return true;}catch(...){return false;}
}
bool AsyncExecutorCompletion::cancelled() const noexcept {
    return impl_->cancellation&&impl_->cancellation->is_cancelled();
}

Run::Run(std::unique_ptr<Impl>p):impl_(std::move(p)){}
Run::Run(std::shared_ptr<Impl>p):impl_(std::move(p)){}
Run::~Run(){}
Run::Run(Run&&v)noexcept:impl_(std::move(v.impl_)){}
Run&Run::operator=(Run&&v)noexcept{impl_=std::move(v.impl_);return *this;}
void Run::cancel(){cancel("cancelled");}
void Run::cancel(const std::string& reason){
    impl_->cancelled.store(true);impl_->cancellation->request_cancel(reason);
    std::shared_ptr<CancellationToken> attempt;
    std::shared_ptr<TimerCoordinator::Handle> timer;
    {std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
        attempt=impl_->async_attempt_cancellation;timer=impl_->async_timer;}
    if(attempt)attempt->request_cancel(reason);
    if(timer)timer->cancel();
    std::vector<std::shared_ptr<Run> > children;
    if(impl_->parallel_continuation){
        std::lock_guard<std::mutex> lock(impl_->parallel_continuation->mutex);
        children=impl_->parallel_continuation->children;
    }
    for(const std::shared_ptr<Run>& child:children)child->cancel(reason);
    std::shared_ptr<Run::Impl> subflow;
    {std::lock_guard<std::mutex> lock(impl_->continuation_mutex);subflow=impl_->subflow_child_state;}
    if(subflow)Run(subflow).cancel(reason);
    std::function<void()> resume;
    {std::lock_guard<std::mutex> lock(impl_->continuation_mutex);resume=impl_->resume_drive;}
    if(resume)try{impl_->scheduler->schedule_continuation(resume);}catch(...){}
}
bool Run::cancelled()const{return impl_->cancelled.load();}
bool Run::suspended()const{return impl_->is_suspended;}

ExecutionResult Run::execute_step(const Value& public_input) {
    std::lock_guard<std::mutex> drive_lock(impl_->drive_mutex);
    if(impl_->completed){
        const Json::Value& final=impl_->state["_run_result"];
        if(final.get("success",false).asBool())
            return ExecutionResult::ok(Value::parse(write_json(final["output"],false)));
        return ExecutionResult::fail(final["error"].get("category","execution_error").asString(),
            final["error"].get("code","RUN_FAILED").asString(),final["error"].get("message","Run failed.").asString());
    }
    if(!impl_->initialized){
        impl_->workflow_input=parse_json(public_input.to_json());
        if(!impl_->run.isMember("id"))impl_->run["id"]="run-"+std::to_string(next_run_id.fetch_add(1));
        if(impl_->trace_id.empty())impl_->trace_id=sha256_digest(impl_->run["id"].asString()+impl_->workflow_fingerprint).substr(7,32);
        impl_->run["mode"]=run_mode_name(impl_->options.mode);
        if(impl_->current.empty())impl_->current=impl_->workflow->entry();
        impl_->started=std::chrono::steady_clock::now();impl_->initialized=true;
        ++impl_->event_sequence;
        Json::Value identity(Json::objectValue);identity["runtime"]="dage/0.2.0";
        identity["workflow_digest"]=impl_->workflow_fingerprint;identity["bundle_digest"]=impl_->bundle_digest;
        identity["run_mode"]=run_mode_name(impl_->options.mode);
        if(impl_->options.trace_capture==TraceCapture::Full)identity["workflow_input"]=impl_->workflow_input;
        identity["components"]=Json::Value(Json::objectValue);
        for(const auto& component:impl_->options.component_identities)
            identity["components"][component.first]=component.second;
        impl_->emit("run_started",impl_->current,0,nullptr,write_json(identity,false),TraceCapture::Metadata);
    }
    Json::Value& workflow_input=impl_->workflow_input;Json::Value& outputs=impl_->outputs;
    Json::Value& state=impl_->state;Json::Value& error=impl_->error;Json::Value& run=impl_->run;
    std::uint64_t& step=impl_->step;std::map<std::string,int>& hits=impl_->hits;std::map<std::string,int>& loops=impl_->loops;
    std::string& current=impl_->current;
    const std::uint64_t max_steps=impl_->workflow->max_steps();
    const std::uint64_t configured_deadline=impl_->options.deadline_ms;
    const std::uint64_t workflow_timeout=impl_->workflow->timeout_ms()?
        (configured_deadline?std::min(impl_->workflow->timeout_ms(),configured_deadline):impl_->workflow->timeout_ms()):configured_deadline;
    ExecutionResult last_result=ExecutionResult::ok(Value::object());
    while(!current.empty()){
        if(impl_->cancelled.load()){++impl_->event_sequence;impl_->emit("run_cancelled",current);
            return ExecutionResult::fail("cancelled","CANCELLED",impl_->cancellation->reason());}
        bool continuing_async=false;
        bool continuing_retry=false;
        bool continuing_parallel=false;
        bool continuing_subflow=false;
        {
            std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
            if(impl_->async_pending&&impl_->async_node==current){
                if(!impl_->async_ready)
                    return ExecutionResult::fail("suspended","ASYNC_PENDING","Async executor completion is pending.");
                if(impl_->async_result.error.code=="ASYNC_RETRY_READY"){
                    continuing_retry=true;impl_->async_pending=false;impl_->async_ready=false;
                    impl_->async_timer.reset();++impl_->async_attempt;
                }else continuing_async=true;
            }
            continuing_parallel=impl_->parallel_continuation&&
                impl_->parallel_continuation->node==current;
            if(impl_->subflow_pending&&impl_->subflow_node==current){
                if(!impl_->subflow_ready)
                    return ExecutionResult::fail("suspended","ASYNC_PENDING","Subflow completion is pending.");
                continuing_subflow=true;
            }
        }
        if(!impl_->stop_before.empty()&&current==impl_->stop_before)return last_result;
        if(impl_->options.max_events&&impl_->event_sequence>=impl_->options.max_events)
            return ExecutionResult::fail("resource_limit","MAX_EVENTS","Run event quota exceeded.");
        if(!continuing_async&&!continuing_retry&&!continuing_parallel&&!continuing_subflow&&++step>max_steps)return ExecutionResult::fail("resource_limit","MAX_STEPS","Maximum steps exceeded.");
        const std::uint64_t current_elapsed=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-impl_->started).count());
        if(workflow_timeout&&impl_->elapsed_ms+current_elapsed>workflow_timeout)
            return ExecutionResult::fail("timeout","WORKFLOW_TIMEOUT","Workflow timeout.");
        const NodeIR* node=impl_->workflow->find_node(current);
        if(!node)return ExecutionResult::fail("internal_error","IR_NODE_NOT_FOUND","IR node not found: "+current);
        if(impl_->failure_injector&&impl_->failure_injector("before_node",current))
            return ExecutionResult::fail("injected_failure","INJECTED_CRASH","Failure injected before node execution.");
        run["step"]=static_cast<Json::UInt64>(step);const NodeType type=node->type();
        if(!continuing_async&&!continuing_retry&&!continuing_parallel&&!continuing_subflow){
            impl_->parent_span_id=impl_->last_span_id;impl_->causation_id=impl_->last_span_id;
            impl_->current_span_id=sha256_digest(impl_->trace_id+current+std::to_string(step)).substr(7,16);
            impl_->node_status[current]=NodeStatus::Running;++impl_->event_sequence;
            impl_->emit("node_started",current);
        }
        const EffectKind effect=node->effect_kind();const ReplayPolicy replay=node->replay_policy();
        const bool external_write=effect==EffectKind::ExternalWrite;
        const bool irreversible=effect==EffectKind::Irreversible;
        bool effect_blocked=!impl_->deterministic_replay&&((external_write&&!impl_->options.allow_external_writes)||
                            (irreversible&&!impl_->options.allow_irreversible));
        if(!impl_->deterministic_replay&&impl_->options.mode==RunMode::Shadow&&(external_write||irreversible))effect_blocked=true;
        if(!impl_->deterministic_replay&&impl_->options.mode==RunMode::Replay&&
           (replay==ReplayPolicy::AtMostOnce||replay==ReplayPolicy::Manual||replay==ReplayPolicy::Forbidden))
            effect_blocked=true;
        if(effect_blocked){
            impl_->node_status[current]=NodeStatus::Blocked;++impl_->event_sequence;
            impl_->emit("node_blocked",current);
            return ExecutionResult::fail("effect_blocked","EFFECT_REPLAY_BLOCKED","Node effect/replay policy blocks execution in this run mode.");
        }
        const bool effect_already_committed=(external_write||irreversible)&&
            state["_effects"][current].get("status","").asString()=="committed";
        const bool effect_prepared=(external_write||irreversible)&&
            state["_effects"][current].get("status","").asString()=="prepared";
        if(effect_prepared&&replay!=ReplayPolicy::Safe&&replay!=ReplayPolicy::Idempotent)
            return ExecutionResult::fail("effect_unknown","EFFECT_RECOVERY_REQUIRES_MANUAL",
                "Prepared non-idempotent effect cannot be executed automatically after recovery.");
        if(!impl_->deterministic_replay&&(external_write||irreversible)&&!effect_already_committed&&!continuing_async&&!continuing_retry){
            state["_effects"][current]["status"]="prepared";
            state["_effects"][current]["idempotency_key"]=replace_token(
                replace_token(node->idempotency_key(),"${run.id}",run["id"].asString()),"${node.id}",current);
            if(impl_->store){Json::Value persisted=parse_json(checkpoint());persisted["state_version"]=static_cast<Json::UInt64>(impl_->store_version+1);
                Result<std::uint64_t> saved=store_compare_exchange(impl_->store,run["id"].asString(),impl_->store_version,write_json(persisted,true));
                if(!saved)return ExecutionResult::fail("state_store",saved.error().code,saved.error().message);
                impl_->store_version=saved.value();}
            if(impl_->failure_injector&&impl_->failure_injector("after_prepare",current))
                return ExecutionResult::fail("injected_failure","INJECTED_CRASH","Failure injected after effect prepare.");
        }
        Json::Value node_input=resolve_template(parse_json(node->input().to_json()),workflow_input,workflow_input,
                                                outputs[current]["output"],error,outputs,state,run);
        ++impl_->event_sequence;
        Json::Value input_payload(Json::objectValue);input_payload["input"]=node_input;
        impl_->emit("node_input_resolved",current,0,nullptr,write_json(input_payload,false),TraceCapture::Inputs);
        ExecutionResult result=ExecutionResult::ok(Value::parse(write_json(node_input,false)));
        if(effect_already_committed){
            result=ExecutionResult::ok(Value::parse(write_json(outputs[current]["output"],false)));
        }else if(type==NodeType::Human&&impl_->has_resume){
            result=ExecutionResult::ok(Value::parse(write_json(impl_->resume_output,false)));
            impl_->has_resume=false;impl_->is_suspended=false;
        } else if(type==NodeType::Subflow){
            if(continuing_subflow){
                std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
                result=impl_->subflow_result;impl_->subflow_pending=false;impl_->subflow_ready=false;
                impl_->subflow_node.clear();impl_->subflow_child_state.reset();
            }else{
                const Json::Value config=parse_json(node->config().to_json());
                const std::string workflow_name=config["workflow"].asString();
                if(impl_->workflows.count(workflow_name)==0)
                    result=ExecutionResult::fail("executor_not_found","SUBFLOW_NOT_FOUND","Registered subflow was not found.");
                else{
                std::unique_ptr<Run::Impl> child_impl(new Run::Impl(impl_->workflows[workflow_name],impl_->executors,impl_->async_executors,impl_->workflows,impl_->policy,impl_->events,impl_->trace_sink,impl_->lease_provider,impl_->scheduler,impl_->timers,impl_->options,"",impl_->store,impl_->failure_injector));
                child_impl->trace_id=impl_->trace_id;child_impl->parent_span_id=impl_->current_span_id;
                child_impl->causation_id=impl_->current_span_id;
                child_impl->invocation_id=impl_->invocation_id+"/subflow:"+current+":"+std::to_string(step);
                child_impl->deterministic_replay=impl_->deterministic_replay;child_impl->replay_state=impl_->replay_state;
                const std::string entry=config.get("entry","").asString();if(!entry.empty())child_impl->current=entry;
                std::shared_ptr<Run::Impl> child_state(std::move(child_impl));
                {std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
                    impl_->subflow_pending=true;impl_->subflow_ready=false;impl_->subflow_node=current;
                    impl_->subflow_child_state=child_state;}
                std::weak_ptr<Run::Impl> weak=impl_;
                Run(child_state).execute_async(Value::parse(write_json(node_input,false)),[weak](const ExecutionResult& child_result){
                    std::shared_ptr<Run::Impl> parent=weak.lock();if(!parent)return;
                    std::function<void()> resume;
                    {std::lock_guard<std::mutex> lock(parent->continuation_mutex);
                        if(!parent->subflow_pending||parent->subflow_ready)return;
                        parent->subflow_result=child_result;parent->subflow_ready=true;
                        if(!parent->driving)resume=parent->resume_drive;}
                    if(resume)try{parent->scheduler->schedule_continuation(resume);}catch(...){}
                });
                {std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
                    if(!impl_->subflow_ready)
                        return ExecutionResult::fail("suspended","ASYNC_PENDING","Subflow completion is pending.");
                    result=impl_->subflow_result;impl_->subflow_pending=false;impl_->subflow_ready=false;
                    impl_->subflow_node.clear();impl_->subflow_child_state.reset();}
                }
            }
        } else if(needs_executor(type)){
            const std::string executor=node->executor();
            if(impl_->deterministic_replay){
                std::lock_guard<std::mutex> replay_lock(impl_->replay_state->mutex);
                auto recorded=impl_->replay_state->outcomes.find(impl_->invocation_id+"\n"+current);
                if(recorded==impl_->replay_state->outcomes.end()||recorded->second.empty())
                    result=ExecutionResult::fail("trace_replay","REPLAY_OUTCOME_MISSING","Recorded executor outcome is missing.");
                else{result=recorded->second.front();recorded->second.pop_front();}
            }else if(impl_->policy&&!impl_->policy(executor))result=ExecutionResult::fail("permission_denied","EXECUTOR_DENIED","Executor rejected by policy.");
            else if(impl_->executors.count(executor)==0&&impl_->async_executors.count(executor)==0)result=ExecutionResult::fail("executor_not_found","EXECUTOR_NOT_FOUND","Executor is not registered.");
            else if(continuing_async){
                std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
                result=impl_->async_result;
                result.error.attempt=impl_->async_attempt;result.error.node_id=current;
                if(impl_->async_lease){impl_->async_lease.reset();++impl_->event_sequence;
                    impl_->emit("resource_lease_released",current,impl_->async_attempt,nullptr,"{}",TraceCapture::Metadata);}
                if((external_write||irreversible)&&impl_->async_committer&&impl_->async_committer->committed()){
                    state["_effects"][current]["status"]=result.success?"committed":"unknown";
                    if(impl_->store){
                        Json::Value persisted=parse_json(checkpoint());
                        persisted["state_version"]=static_cast<Json::UInt64>(impl_->store_version+1);
                        Result<std::uint64_t> saved=store_compare_exchange(
                            impl_->store,run["id"].asString(),impl_->store_version,write_json(persisted,true));
                        if(!saved)result=ExecutionResult::fail("state_store",saved.error().code,saved.error().message);
                        else impl_->store_version=saved.value();
                    }
                    if(impl_->failure_injector&&impl_->failure_injector("after_commit",current))
                        return ExecutionResult::fail("injected_failure","INJECTED_CRASH","Failure injected after effect commit.");
                    if(!result.success&&result.error.category!="state_store")
                        result=ExecutionResult::fail("effect_unknown","EFFECT_COMMITTED_WITH_ERROR",
                            "Executor reported failure after committing an external effect.");
                }else if((external_write||irreversible)&&result.success){
                    state["_effects"][current]["status"]="unknown";
                    result=ExecutionResult::fail("effect_unknown","EFFECT_COMMIT_NOT_CONFIRMED",
                        "External effect executor returned success without committing the fence.");
                }
                const bool retry=result.error.retryable&&impl_->async_attempt<node->max_attempts();
                if(retry&&impl_->retry_remaining){
                    --impl_->retry_remaining;
                    std::uint64_t wait=node->retry_delay_ms();
                    if(node->retry_backoff()=="exponential"&&impl_->async_attempt>1)
                        wait*=std::uint64_t(1)<<(std::min<std::uint32_t>(impl_->async_attempt-1,20));
                    if(node->retry_jitter_ms())wait+=(trace_sample_bucket(
                        run["id"].asString()+current+std::to_string(impl_->async_attempt))%(node->retry_jitter_ms()+1));
                    const std::uint64_t elapsed=impl_->elapsed_ms+static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now()-impl_->started).count());
                    const std::uint64_t remaining=workflow_timeout&&elapsed<workflow_timeout?workflow_timeout-elapsed:0;
                    if(workflow_timeout&&wait>=remaining)
                        result=ExecutionResult::fail("timeout","RETRY_DEADLINE_EXCEEDED","Retry delay exceeds remaining deadline.");
                    else{
                        ++impl_->event_sequence;Json::Value retry_payload(Json::objectValue);
                        retry_payload["delay_ms"]=static_cast<Json::UInt64>(wait);
                        retry_payload["next_attempt"]=impl_->async_attempt+1;
                        retry_payload["budget_remaining"]=impl_->retry_remaining;
                        impl_->emit("retry_scheduled",current,impl_->async_attempt,&result,
                            write_json(retry_payload,false),TraceCapture::Metadata);
                        const std::uint64_t generation=++impl_->async_generation;
                        impl_->async_ready=false;impl_->async_attempt_cancellation.reset();impl_->async_lease.reset();
                        impl_->async_committer.reset();
                        std::weak_ptr<Run::Impl> weak=impl_;
                        impl_->async_timer=impl_->timers->schedule_after(std::max<std::uint64_t>(1,wait),[weak,generation](){
                            std::shared_ptr<Run::Impl> state=weak.lock();if(!state)return;
                            std::function<void()> resume;
                            {std::lock_guard<std::mutex> lock(state->continuation_mutex);
                                if(!state->async_pending||state->async_ready||state->async_generation!=generation)return;
                                state->async_result=ExecutionResult::fail("internal","ASYNC_RETRY_READY","Retry delay elapsed.");
                                state->async_ready=true;if(!state->driving)resume=state->resume_drive;}
                            if(resume)try{state->scheduler->schedule_continuation(resume);}catch(...){}
                        });
                        return ExecutionResult::fail("suspended","ASYNC_PENDING","Async retry delay is pending.");
                    }
                }else if(retry&&!impl_->retry_remaining)
                    result=ExecutionResult::fail("resource_limit","RETRY_BUDGET_EXHAUSTED","Run retry budget exhausted.");
                impl_->async_pending=false;impl_->async_ready=false;
                impl_->async_node.clear();impl_->async_lease.reset();impl_->async_attempt_cancellation.reset();
                impl_->async_timer.reset();impl_->async_committer.reset();impl_->async_attempt=0;
            }else if(impl_->async_executors.count(executor)){
                std::shared_ptr<RuntimeEffectCommitter> committer(new RuntimeEffectCommitter());
                std::shared_ptr<CancellationToken> attempt_cancellation(new CancellationToken());
                if(!continuing_retry)impl_->async_attempt=1;
                ExecutionContext ctx;ctx.run_id=run["id"].asString();ctx.node_id=current;ctx.node_type=node_type_name(type);
                ctx.attempt=impl_->async_attempt;ctx.cancelled=&impl_->cancelled;ctx.cancellation=attempt_cancellation.get();
                ctx.cancellation_owner=attempt_cancellation;ctx.run_mode=run_mode_name(impl_->options.mode);
                ctx.idempotency_key=replace_token(replace_token(node->idempotency_key(),"${run.id}",ctx.run_id),"${node.id}",current);
                const std::uint64_t elapsed=impl_->elapsed_ms+static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now()-impl_->started).count());
                const std::uint64_t remaining=workflow_timeout&&elapsed<workflow_timeout?workflow_timeout-elapsed:0;
                ctx.deadline_remaining_ms=remaining;ctx.effect_committer=(external_write||irreversible)?committer.get():nullptr;
                ctx.effect_committer_owner=(external_write||irreversible)?committer:std::shared_ptr<EffectCommitter>();
                ctx.resource_lease=nullptr;
                impl_->async_committer=(external_write||irreversible)?committer:std::shared_ptr<RuntimeEffectCommitter>();
                if(impl_->lease_provider){
                    ResourceRequest request;request.run_id=ctx.run_id;request.node_id=current;
                    request.node_type=ctx.node_type;request.executor=executor;request.deadline_remaining_ms=remaining;
                    const Json::Value resource_config=parse_json(node->config().to_json());
                    if(resource_config["resources"].isObject())for(const std::string& name:resource_config["resources"].getMemberNames())
                        request.resources[name]=resource_config["resources"][name].asUInt64();
                    request.priority=resource_config.get("resource_priority",0).asInt();
                    request.fairness_key=replace_token(replace_token(
                        resource_config.get("resource_fairness_key",ctx.run_id).asString(),"${run.id}",ctx.run_id),"${node.id}",current);
                    request.lease_ttl_ms=resource_config.get("resource_lease_ttl_ms",30000).asUInt64();
                    ++impl_->event_sequence;Json::Value lease_payload(Json::objectValue);
                    lease_payload["units"]=request.units;lease_payload["deadline_remaining_ms"]=static_cast<Json::UInt64>(remaining);
                    lease_payload["priority"]=request.priority;lease_payload["fairness_key"]=request.fairness_key;
                    lease_payload["ttl_ms"]=static_cast<Json::UInt64>(request.lease_ttl_ms);
                    lease_payload["resources"]=resource_config["resources"];
                    impl_->emit("resource_lease_requested",current,ctx.attempt,nullptr,
                        write_json(lease_payload,false),TraceCapture::Metadata);
                    Result<std::unique_ptr<ResourceLease>> acquired=acquire_resource_lease(
                        impl_->lease_provider,request,impl_->cancellation.get());
                    if(!acquired)result=ExecutionResult::fail(acquired.error().category,acquired.error().code,acquired.error().message,acquired.error().retryable);
                    else{impl_->async_lease=std::move(acquired.value());ctx.resource_lease=impl_->async_lease.get();
                        lease_payload["lease_id"]=impl_->async_lease->lease_id();
                        lease_payload["fencing_token"]=static_cast<Json::UInt64>(impl_->async_lease->fencing_token());
                        lease_payload["expires_at_unix_ms"]=static_cast<Json::UInt64>(impl_->async_lease->expires_at_unix_ms());
                        ++impl_->event_sequence;
                        impl_->emit("resource_lease_acquired",current,ctx.attempt,nullptr,
                            write_json(lease_payload,false),TraceCapture::Metadata);}
                }
                if(result.success){
                    std::shared_ptr<AsyncExecutorCompletion::Impl> completion_impl(new AsyncExecutorCompletion::Impl());
                    completion_impl->cancellation=attempt_cancellation;
                    std::weak_ptr<Run::Impl> weak=impl_;
                    std::uint64_t generation=0;
                    {
                        std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
                        impl_->async_pending=true;impl_->async_ready=false;impl_->async_node=current;
                        impl_->async_attempt_cancellation=attempt_cancellation;
                        generation=++impl_->async_generation;
                    }
                    completion_impl->callback=[weak,generation](const ExecutionResult& completed){
                        std::shared_ptr<Run::Impl> state=weak.lock();if(!state)return;
                        std::function<void()> resume;std::shared_ptr<TimerCoordinator::Handle> timer;
                        {
                            std::lock_guard<std::mutex> lock(state->continuation_mutex);
                            if(!state->async_pending||state->async_ready||state->async_generation!=generation)return;
                            state->async_result=completed;state->async_ready=true;timer=state->async_timer;
                            if(!state->driving)resume=state->resume_drive;
                        }
                        if(timer)timer->cancel();
                        if(resume)try{state->scheduler->schedule_continuation(resume);}catch(...){}
                    };
                    std::uint64_t timeout=node->timeout_ms();
                    if(workflow_timeout)timeout=timeout?std::min(timeout,remaining):remaining;
                    if(timeout)impl_->async_timer=impl_->timers->schedule_after(timeout,[weak,generation,attempt_cancellation](){
                        std::shared_ptr<Run::Impl> state=weak.lock();if(!state)return;
                        std::function<void()> resume;
                        {
                            std::lock_guard<std::mutex> lock(state->continuation_mutex);
                            if(!state->async_pending||state->async_ready||state->async_generation!=generation)return;
                            attempt_cancellation->request_cancel("node timeout");
                            state->async_result=ExecutionResult::fail("timeout","NODE_TIMEOUT","Node timeout.",true);
                            state->async_ready=true;if(!state->driving)resume=state->resume_drive;
                        }
                        if(resume)try{state->scheduler->schedule_continuation(resume);}catch(...){}
                    });
                    std::shared_ptr<AsyncExecutorCompletion> completion(
                        new AsyncExecutorCompletion(completion_impl));
                    try{impl_->async_executors[executor](ctx,Value::parse(write_json(node_input,false)),completion);}
                    catch(const std::exception& ex){completion->complete(ExecutionResult::fail("tool_error","ASYNC_START_FAILED",ex.what()));}
                    return ExecutionResult::fail("suspended","ASYNC_PENDING","Async executor completion is pending.");
                }
            }else{
                const int max_attempts=static_cast<int>(node->max_attempts());const int delay=static_cast<int>(node->retry_delay_ms());
                for(int attempt=1;attempt<=max_attempts;++attempt){
                    if(attempt>1){if(impl_->retry_remaining==0){result=ExecutionResult::fail("resource_limit","RETRY_BUDGET_EXHAUSTED","Run retry budget exhausted.");break;}--impl_->retry_remaining;}
                    std::shared_ptr<RuntimeEffectCommitter> committer(new RuntimeEffectCommitter());
                    ExecutionContext ctx;ctx.run_id=run["id"].asString();ctx.node_id=current;ctx.node_type=node_type_name(type);
                    ctx.attempt=static_cast<std::uint32_t>(attempt);ctx.cancelled=&impl_->cancelled;ctx.cancellation=impl_->cancellation.get();ctx.cancellation_owner=impl_->cancellation;ctx.run_mode=run_mode_name(impl_->options.mode);
                    ctx.idempotency_key=replace_token(replace_token(node->idempotency_key(),"${run.id}",ctx.run_id),"${node.id}",current);
                    const std::uint64_t elapsed=impl_->elapsed_ms+static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()-impl_->started).count());
                    const std::uint64_t remaining=workflow_timeout&&elapsed<workflow_timeout?workflow_timeout-elapsed:0;
                    ctx.deadline_remaining_ms=remaining;ctx.effect_committer=(external_write||irreversible)?committer.get():nullptr;
                    ctx.effect_committer_owner=(external_write||irreversible)?committer:std::shared_ptr<EffectCommitter>();
                    ctx.resource_lease=nullptr;
                    std::unique_ptr<ResourceLease> lease;
                    if(impl_->lease_provider){
                        ResourceRequest request;request.run_id=ctx.run_id;request.node_id=current;
                        request.node_type=ctx.node_type;request.executor=executor;request.deadline_remaining_ms=remaining;
                        const Json::Value resource_config=parse_json(node->config().to_json());
                        if(resource_config["resources"].isObject())for(const std::string& name:resource_config["resources"].getMemberNames())
                            request.resources[name]=resource_config["resources"][name].asUInt64();
                        request.priority=resource_config.get("resource_priority",0).asInt();
                        request.fairness_key=replace_token(replace_token(
                            resource_config.get("resource_fairness_key",ctx.run_id).asString(),"${run.id}",ctx.run_id),"${node.id}",current);
                        request.lease_ttl_ms=resource_config.get("resource_lease_ttl_ms",30000).asUInt64();
                        ++impl_->event_sequence;Json::Value lease_payload(Json::objectValue);
                        lease_payload["units"]=request.units;lease_payload["deadline_remaining_ms"]=static_cast<Json::UInt64>(remaining);
                        lease_payload["priority"]=request.priority;lease_payload["fairness_key"]=request.fairness_key;
                        lease_payload["ttl_ms"]=static_cast<Json::UInt64>(request.lease_ttl_ms);
                        lease_payload["resources"]=resource_config["resources"];
                        impl_->emit("resource_lease_requested",current,ctx.attempt,nullptr,
                            write_json(lease_payload,false),TraceCapture::Metadata);
                        Result<std::unique_ptr<ResourceLease>> acquired=acquire_resource_lease(
                            impl_->lease_provider,request,impl_->cancellation.get());
                        if(!acquired){result=ExecutionResult::fail(acquired.error().category,acquired.error().code,acquired.error().message,acquired.error().retryable);break;}
                        lease=std::move(acquired.value());ctx.resource_lease=lease.get();
                        lease_payload["lease_id"]=lease->lease_id();
                        lease_payload["fencing_token"]=static_cast<Json::UInt64>(lease->fencing_token());
                        lease_payload["expires_at_unix_ms"]=static_cast<Json::UInt64>(lease->expires_at_unix_ms());
                        ++impl_->event_sequence;impl_->emit("resource_lease_acquired",current,ctx.attempt,nullptr,
                            write_json(lease_payload,false),TraceCapture::Metadata);
                    }
                    std::uint64_t timeout=node->timeout_ms();if(workflow_timeout)timeout=timeout?std::min(timeout,remaining):remaining;
                    if(workflow_timeout&&remaining==0){result=ExecutionResult::fail("timeout","WORKFLOW_TIMEOUT","Workflow deadline exceeded.");break;}
                    if(timeout){
                        std::packaged_task<ExecutionResult()> task([&](){return impl_->executors[executor](ctx,Value::parse(write_json(node_input,false)));});
                        std::future<ExecutionResult> future=task.get_future();std::thread worker(std::move(task));
                        if(future.wait_for(std::chrono::milliseconds(timeout))==std::future_status::ready){result=future.get();worker.join();}
                        else{
                            // Synchronous executors must be joined for lifetime safety. The cancel token
                            // lets cooperative callbacks return promptly; non-cooperative callbacks delay return.
                            impl_->cancelled.store(true);impl_->cancellation->request_cancel("node timeout");worker.join();
                            result=ExecutionResult::fail("timeout","NODE_TIMEOUT","Node timeout.",true);
                        }
                    }else result=impl_->executors[executor](ctx,Value::parse(write_json(node_input,false)));
                    if(lease){lease.reset();++impl_->event_sequence;
                        impl_->emit("resource_lease_released",current,ctx.attempt,nullptr,"{}",TraceCapture::Metadata);}
                    if((external_write||irreversible)&&committer->committed()){
                        state["_effects"][current]["status"]=result.success?"committed":"unknown";
                        if(impl_->store){Json::Value persisted=parse_json(checkpoint());persisted["state_version"]=static_cast<Json::UInt64>(impl_->store_version+1);
                            Result<std::uint64_t> saved=store_compare_exchange(impl_->store,run["id"].asString(),impl_->store_version,write_json(persisted,true));
                            if(!saved){result=ExecutionResult::fail("state_store",saved.error().code,saved.error().message);break;}impl_->store_version=saved.value();}
                        if(impl_->failure_injector&&impl_->failure_injector("after_commit",current))
                            return ExecutionResult::fail("injected_failure","INJECTED_CRASH","Failure injected after effect commit.");
                        if(!result.success){result=ExecutionResult::fail("effect_unknown","EFFECT_COMMITTED_WITH_ERROR","Executor reported failure after committing an external effect.");break;}
                    }else if((external_write||irreversible)&&result.success){
                        state["_effects"][current]["status"]="unknown";
                        result=ExecutionResult::fail("effect_unknown","EFFECT_COMMIT_NOT_CONFIRMED","External effect executor returned success without committing the fence.");
                    }
                    result.error.attempt=static_cast<std::uint32_t>(attempt);result.error.node_id=current;
                    if(result.success||!result.error.retryable||attempt==max_attempts)break;
                    std::uint64_t wait=static_cast<std::uint64_t>(delay);
                    if(node->retry_backoff()=="exponential"&&attempt>1)wait*=std::uint64_t(1)<<(std::min(attempt-1,20));
                    if(node->retry_jitter_ms())wait+=(trace_sample_bucket(ctx.run_id+current+std::to_string(attempt))%(node->retry_jitter_ms()+1));
                    if(workflow_timeout&&wait>=ctx.deadline_remaining_ms){result=ExecutionResult::fail("timeout","RETRY_DEADLINE_EXCEEDED","Retry delay exceeds remaining deadline.");break;}
                    ++impl_->event_sequence;Json::Value retry_payload(Json::objectValue);
                    retry_payload["delay_ms"]=static_cast<Json::UInt64>(wait);retry_payload["next_attempt"]=attempt+1;
                    retry_payload["budget_remaining"]=impl_->retry_remaining;
                    impl_->emit("retry_scheduled",current,static_cast<std::uint32_t>(attempt),&result,
                        write_json(retry_payload,false),TraceCapture::Metadata);
                    if(wait>0)std::this_thread::sleep_for(std::chrono::milliseconds(wait));
                }
            }
        } else if(type==NodeType::Parallel) {
            std::shared_ptr<Run::Impl::ParallelContinuation> pending=impl_->parallel_continuation;
            if(!pending){
                pending.reset(new Run::Impl::ParallelContinuation());
                pending->node=current;pending->join=node->parallel_join();
                pending->branches=node->parallel_branches();
                const std::size_t configured=impl_->workflow->max_parallel();
                pending->max_parallel=std::max<std::size_t>(1,std::min<std::size_t>(
                    configured,impl_->options.max_in_flight_tasks?impl_->options.max_in_flight_tasks:configured));
                const Json::Value parallel_policy=parse_json(node->parallel_policy().to_json());
                pending->mode=parallel_policy.get("mode","all").asString();
                pending->fail=parallel_policy.get("fail","wait").asString();
                pending->minimum=parallel_policy.get("min_success",1).asUInt();
                ++impl_->event_sequence;Json::Value parallel_payload(Json::objectValue);
                parallel_payload["mode"]=pending->mode;parallel_payload["fail"]=pending->fail;
                parallel_payload["min_success"]=static_cast<Json::UInt64>(pending->minimum);
                parallel_payload["max_parallel"]=static_cast<Json::UInt64>(pending->max_parallel);
                parallel_payload["join"]=pending->join;parallel_payload["branches"]=Json::Value(Json::arrayValue);
                for(const std::string& branch:pending->branches)parallel_payload["branches"].append(branch);
                impl_->emit("parallel_started",current,0,nullptr,write_json(parallel_payload,false),TraceCapture::Metadata);
                if(!state["_parallel_children"].isMember(current))
                    for(const std::string& branch:pending->branches)
                        state["_parallel_children"][current][branch]=run["id"].asString()+"-p-"+current+"-"+
                            std::to_string(step)+"-"+branch;
                if(impl_->store){
                    Json::Value persisted=parse_json(checkpoint());
                    persisted["state_version"]=static_cast<Json::UInt64>(impl_->store_version+1);
                    Result<std::uint64_t> saved=store_compare_exchange(
                        impl_->store,run["id"].asString(),impl_->store_version,write_json(persisted,true));
                    if(!saved)return ExecutionResult::fail("state_store",saved.error().code,saved.error().message);
                    impl_->store_version=saved.value();
                }
                impl_->parallel_continuation=pending;
            }
            for(;;){
                std::vector<std::pair<std::string,ExecutionResult> > completed_batch;
                {
                    std::lock_guard<std::mutex> lock(pending->mutex);
                    if(pending->in_flight&&!pending->batch_ready)
                        return ExecutionResult::fail("suspended","ASYNC_PENDING","Parallel child completions are pending.");
                    if(pending->batch_ready){
                        completed_batch.swap(pending->batch_results);
                        pending->batch_ready=false;pending->children.clear();
                    }
                }
                for(const auto& completed:completed_batch){
                    const std::string& branch=completed.first;const ExecutionResult& branch_result=completed.second;
                    if(branch_result.success){
                        ++pending->success_count;
                        outputs[branch]["output"]=parse_json(branch_result.output.to_json());
                    }else{
                        ++pending->failure_count;
                        if(pending->failure_count==1)pending->terminal_result=branch_result;
                        outputs[branch]["error"]["category"]=branch_result.error.category;
                        outputs[branch]["error"]["code"]=branch_result.error.code;
                        outputs[branch]["error"]["message"]=branch_result.error.message;
                        if(pending->fail=="fast"){pending->terminal=true;pending->terminal_result=branch_result;}
                    }
                }
                if(pending->terminal){result=pending->terminal_result;break;}
                if(pending->mode=="any"&&pending->success_count>0)break;
                if(pending->next_index>=pending->branches.size())break;

                const std::size_t end=std::min(pending->branches.size(),pending->next_index+pending->max_parallel);
                while(pending->next_index<end){
                    const std::string branch=pending->branches[pending->next_index++];
                    const std::string child_id=state["_parallel_children"][current][branch].asString();
                    std::unique_ptr<Run::Impl> child_impl(new Run::Impl(impl_->workflow,impl_->executors,impl_->async_executors,impl_->workflows,impl_->policy,impl_->events,impl_->trace_sink,impl_->lease_provider,impl_->scheduler,impl_->timers,impl_->options,"",impl_->store,impl_->failure_injector));
                    child_impl->trace_id=impl_->trace_id;child_impl->parent_span_id=impl_->current_span_id;
                    child_impl->causation_id=impl_->current_span_id;child_impl->current=branch;
                    child_impl->invocation_id=impl_->invocation_id+"/parallel:"+current+":"+
                        std::to_string(step)+":"+branch;
                    child_impl->deterministic_replay=impl_->deterministic_replay;child_impl->replay_state=impl_->replay_state;
                    child_impl->stop_before=pending->join;child_impl->run["id"]=child_id;
                    if(impl_->store){
                        Result<StateRecord> stored=store_load(impl_->store,child_id);
                        if(stored){
                            Json::Value cp=parse_json(stored.value().checkpoint);
                            if(cp.get("workflow_digest","").asString()==child_impl->workflow_fingerprint){
                                child_impl->current=cp.get("current",branch).asString();
                                child_impl->workflow_input=cp["workflow_input"];child_impl->outputs=cp["outputs"];
                                child_impl->state=cp["state"];child_impl->error=cp["error"];child_impl->run=cp["run"];
                                child_impl->step=cp.get("step",0).asUInt64();child_impl->elapsed_ms=cp.get("elapsed_ms",0).asUInt64();
                                child_impl->event_sequence=cp.get("event_sequence",0).asUInt64();
                                child_impl->invocation_id=cp["trace"].get("invocation_id",child_impl->invocation_id).asString();
                                child_impl->store_version=stored.value().version;child_impl->initialized=true;
                                child_impl->started=std::chrono::steady_clock::now();
                            }
                        }else if(stored.error().code!="STATE_NOT_FOUND")
                            return ExecutionResult::fail(
                                "state_store",stored.error().code,stored.error().message);
                    }
                    std::shared_ptr<Run> child(new Run(std::move(child_impl)));
                    {std::lock_guard<std::mutex> lock(pending->mutex);
                        ++pending->in_flight;pending->children.push_back(child);}
                    std::weak_ptr<Run::Impl> weak_parent=impl_;
                    child->execute_async(Value::parse(write_json(workflow_input,false)),
                        [weak_parent,pending,branch](const ExecutionResult& branch_result){
                            std::shared_ptr<Run::Impl> parent=weak_parent.lock();if(!parent)return;
                            std::function<void()> resume;bool batch_ready=false;
                            {
                                std::lock_guard<std::mutex> lock(pending->mutex);
                                pending->batch_results.push_back(std::make_pair(branch,branch_result));
                                if(--pending->in_flight==0)pending->batch_ready=true;
                                batch_ready=pending->batch_ready;
                            }
                            {
                                std::lock_guard<std::mutex> lock(parent->continuation_mutex);
                                if(batch_ready&&!parent->driving)resume=parent->resume_drive;
                            }
                            if(resume)try{parent->scheduler->schedule_continuation(resume);}catch(...){}
                        });
                }
            }
            const bool enough=pending->mode=="any"?pending->success_count>0:
                (pending->mode=="min_success"?pending->success_count>=pending->minimum:
                 pending->success_count==pending->branches.size());
            if(!enough&&result.success)result=pending->failure_count?pending->terminal_result:
                ExecutionResult::fail("tool_error","PARALLEL_POLICY_FAILED","Parallel success policy was not met.");
            if(result.success&&impl_->failure_injector&&impl_->failure_injector("after_parallel_children",current))
                return ExecutionResult::fail("injected_failure","INJECTED_CRASH","Failure injected after parallel child completion.");
            if(result.success)state["_parallel_next"]=pending->join;
            ++impl_->event_sequence;Json::Value parallel_result(Json::objectValue);
            parallel_result["success_count"]=static_cast<Json::UInt64>(pending->success_count);
            parallel_result["failure_count"]=static_cast<Json::UInt64>(pending->failure_count);
            parallel_result["join"]=pending->join;parallel_result["policy_satisfied"]=enough;
            impl_->emit("parallel_completed",current,0,&result,write_json(parallel_result,false),TraceCapture::Metadata);
            impl_->parallel_continuation.reset();
        }
        if(!result.success&&type==NodeType::Human&&result.error.category=="suspended"){
            impl_->is_suspended=true;
            impl_->node_status[current]=NodeStatus::Suspended;++impl_->event_sequence;
            impl_->elapsed_ms+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-impl_->started).count());
            impl_->emit("run_suspended",current,0,&result);
            if(impl_->store){Json::Value persisted=parse_json(checkpoint());persisted["state_version"]=static_cast<Json::UInt64>(impl_->store_version+1);
                Result<std::uint64_t> saved=store_compare_exchange(impl_->store,run["id"].asString(),impl_->store_version,write_json(persisted,true));
                if(!saved)return ExecutionResult::fail("state_store",saved.error().code,saved.error().message);
                impl_->store_version=saved.value();}
            return result;
        }
        if(result.success&&impl_->options.max_output_bytes&&
           result.output.to_json(false).size()>impl_->options.max_output_bytes)
            result=ExecutionResult::fail("resource_limit","MAX_OUTPUT_BYTES","Node output exceeds the run output quota.");
        if(result.success){impl_->node_status[current]=NodeStatus::Succeeded;++impl_->event_sequence;outputs[current]["output"]=parse_json(result.output.to_json());error=Json::Value(Json::objectValue);impl_->emit("node_succeeded",current,result.error.attempt,&result,result.output.to_json(false));}
        else{error["category"]=result.error.category;error["code"]=result.error.code;error["message"]=result.error.message;
             impl_->node_status[current]=NodeStatus::Failed;++impl_->event_sequence;
             error["node"]=current;error["attempt"]=result.error.attempt;error["retryable"]=result.error.retryable;
             Json::Value failure_payload(Json::objectValue);failure_payload["category"]=result.error.category;
             failure_payload["code"]=result.error.code;failure_payload["message"]=result.error.message;
             failure_payload["retryable"]=result.error.retryable;
             impl_->emit("node_failed",current,result.error.attempt,&result,write_json(failure_payload,false));}
        impl_->last_span_id=impl_->current_span_id;
        if(impl_->options.max_output_bytes&&write_json(outputs,false).size()>impl_->options.max_output_bytes)
            return ExecutionResult::fail("resource_limit","MAX_OUTPUT_BYTES","Accumulated outputs exceed the run output quota.");
        if(impl_->options.max_state_bytes&&write_json(state,false).size()>impl_->options.max_state_bytes)
            return ExecutionResult::fail("resource_limit","MAX_STATE_BYTES","Run state exceeds the state quota.");
        last_result=result;
        const std::string finished_node=current;
        std::vector<std::string> committed_children;
        ExecutionResult public_result=result;
        if(type==NodeType::End&&result.success)public_result=ExecutionResult::ok(Value::parse(write_json(node_input,false)));
        const std::vector<EdgeIR>& edges=result.success?node->next_edges():node->error_edges();
        if(type==NodeType::Parallel&&result.success){const std::string parallel_node=current;
            const Json::Value child_records=state["_parallel_children"][parallel_node];
            for(const std::string& branch:child_records.getMemberNames())
                committed_children.push_back(child_records[branch].asString());
            current=state["_parallel_next"].asString();state.removeMember("_parallel_next");
            state["_parallel_children"].removeMember(parallel_node);
        }else if(type==NodeType::End&&result.success)current.clear();
        else{
            std::string next;const EdgeIR* chosen=nullptr;
            for(const EdgeIR& edge:edges){
                const NodeIR* target=impl_->workflow->find_node(edge.target());if(!target||!target->enabled())continue;
                const bool matched=edge.condition().empty()||eval_expr(edge.condition(),workflow_input,node_input,outputs[finished_node]["output"],error,outputs,state,run);
                ++impl_->event_sequence;Json::Value evaluation(Json::objectValue);
                evaluation["target"]=edge.target();evaluation["matched"]=matched;evaluation["otherwise"]=edge.otherwise();
                if(!edge.condition().empty())evaluation["condition_digest"]=sha256_digest(edge.condition());
                if(impl_->options.trace_capture==TraceCapture::Full)evaluation["condition"]=edge.condition();
                impl_->emit("edge_evaluated",finished_node,0,nullptr,write_json(evaluation,false),TraceCapture::Metadata);
                if(!matched)continue;
                next=edge.target();chosen=&edge;break;
            }
            if(chosen){
                ++impl_->event_sequence;Json::Value selected(Json::objectValue);selected["target"]=next;
                selected["outcome"]=result.success?"success":"error";
                impl_->emit("edge_selected",finished_node,0,nullptr,write_json(selected,false),TraceCapture::Metadata);
                const Json::Value set_values=parse_json(chosen->set_values().to_json());
                if(set_values.isObject()){std::vector<std::string> keys=set_values.getMemberNames();for(std::size_t i=0;i<keys.size();++i)
                    state[keys[i]]=resolve_template(set_values[keys[i]],workflow_input,node_input,outputs[finished_node]["output"],error,outputs,state,run);}
                const Json::Value loop=parse_json(chosen->loop().to_json());
                if(!loop.empty()){std::string id=loop.get("id",finished_node+"_"+next).asString();int count=++loops[id];
                    if(count>loop["max_iterations"].asInt()){next=loop.get("on_exhausted","").asString();
                        if(next.empty())public_result=ExecutionResult::fail("resource_limit","LOOP_EXHAUSTED","Loop exhausted.");}}
                if(chosen->max_hits()>0&&++hits[finished_node+"->"+next]>static_cast<int>(chosen->max_hits()))
                    public_result=ExecutionResult::fail("resource_limit","MAX_HITS","Edge max_hits exceeded.");
            }
            current=next;
        }
        if(current.empty()){
            impl_->completed=true;state["_run_result"]["success"]=public_result.success;
            if(public_result.success)state["_run_result"]["output"]=parse_json(public_result.output.to_json());
            else{state["_run_result"]["error"]["category"]=public_result.error.category;
                state["_run_result"]["error"]["code"]=public_result.error.code;state["_run_result"]["error"]["message"]=public_result.error.message;}
        }
        if(impl_->store){Json::Value persisted=parse_json(checkpoint());persisted["state_version"]=static_cast<Json::UInt64>(impl_->store_version+1);
            Result<std::uint64_t> saved=store_compare_exchange(impl_->store,run["id"].asString(),impl_->store_version,write_json(persisted,true));
            if(!saved)return ExecutionResult::fail("state_store",saved.error().code,saved.error().message);
            impl_->store_version=saved.value();}
        if(impl_->store&&impl_->options.child_record_retention==ChildRecordRetention::DeleteOnParentCommit)
            for(const std::string& child_id:committed_children){
                Result<bool> erased=store_erase(impl_->store,child_id);
                if(!erased)return ExecutionResult::fail(
                    "state_store",erased.error().code,erased.error().message);
            }
        if(impl_->failure_injector&&impl_->failure_injector("after_state_save",finished_node))
            return ExecutionResult::fail("injected_failure","INJECTED_CRASH","Failure injected after atomic state transition.");
        if(impl_->completed){
            ++impl_->event_sequence;
            impl_->emit("run_completed",finished_node,public_result.error.attempt,&public_result,
                write_json(state["_run_result"],false));
            return public_result;
        }
    }
    return ExecutionResult::ok(Value::object());
}

void Run::execute_async(const Value& input,const RunCompletion& completion){
    if(!completion)throw std::invalid_argument("run completion callback is required");
    {
        std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
        if(impl_->run_completion&&!impl_->callback_delivered)
            throw std::logic_error("run already has an active completion callback");
        impl_->run_completion=completion;impl_->run_input=input;impl_->callback_delivered=false;
    }
    std::weak_ptr<Impl> weak=impl_;
    std::shared_ptr<std::function<void()> > drive(new std::function<void()>());
    *drive=[weak,drive](){
        std::shared_ptr<Impl> state=weak.lock();if(!state){*drive=std::function<void()>();return;}
        {std::lock_guard<std::mutex> lock(state->continuation_mutex);state->driving=true;}
        ExecutionResult result=Run(state).execute_step(state->run_input);
        if(!result.success&&result.error.code=="ASYNC_PENDING"){
            bool ready=false;
            {std::lock_guard<std::mutex> lock(state->continuation_mutex);
                state->driving=false;ready=state->async_ready;}
            if(!ready&&state->parallel_continuation){
                std::lock_guard<std::mutex> lock(state->parallel_continuation->mutex);
                ready=state->parallel_continuation->batch_ready;
            }
            if(!ready){std::lock_guard<std::mutex> lock(state->continuation_mutex);
                ready=state->subflow_ready;}
            if(ready)try{state->scheduler->schedule_continuation(*drive);}catch(...){}
            return;
        }
        RunCompletion callback;
        {
            std::lock_guard<std::mutex> lock(state->continuation_mutex);
            state->driving=false;
            if(state->callback_delivered)return;
            state->async_pending=false;state->async_ready=false;state->async_node.clear();
            if(state->async_timer)state->async_timer->cancel();
            state->async_lease.reset();state->async_attempt_cancellation.reset();state->async_timer.reset();
            state->async_committer.reset();
            state->subflow_pending=false;state->subflow_ready=false;state->subflow_child_state.reset();
            state->callback_delivered=true;callback=state->run_completion;
            state->run_completion=RunCompletion();state->resume_drive=std::function<void()>();
        }
        *drive=std::function<void()>();
        if(callback)callback(result);
    };
    {
        std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
        impl_->resume_drive=*drive;
    }
    try{impl_->scheduler->schedule_continuation(*drive);}
    catch(const std::exception& ex){
        std::lock_guard<std::mutex> lock(impl_->continuation_mutex);
        impl_->callback_delivered=true;impl_->run_completion=RunCompletion();
        impl_->resume_drive=std::function<void()>();
        completion(ExecutionResult::fail("resource_limit","SCHEDULER_REJECTED",ex.what(),true));
    }
}

ExecutionResult Run::execute(const Value& input){
    std::mutex mutex;std::condition_variable ready;bool done=false;
    ExecutionResult result=ExecutionResult::fail("internal_error","NOT_COMPLETED","Run did not complete.");
    execute_async(input,[&](const ExecutionResult& value){
        {std::lock_guard<std::mutex> lock(mutex);result=value;done=true;}ready.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&](){return done;});
    return result;
}

ExecutionResult Run::resume(const Value& human_output){
    if(!impl_->is_suspended)return ExecutionResult::fail("invalid_input","RUN_NOT_SUSPENDED","Run is not suspended.");
    impl_->resume_output=parse_json(human_output.to_json());impl_->has_resume=true;
    impl_->started=std::chrono::steady_clock::now();
    return execute(Value::object());
}

std::string Run::checkpoint()const{
    Json::Value cp(Json::objectValue);cp["format"]="dage-checkpoint";cp["format_version"]="0.2.0";cp["runtime_version"]="0.2.0";
    cp["workflow_digest"]=impl_->workflow_fingerprint;
    cp["bundle_digest"]=impl_->bundle_digest;
    cp["required_capabilities"]=Json::Value(Json::arrayValue);cp["required_capabilities"].append("state.v1");
    cp["required_capabilities"].append("node_status.v1");cp["required_capabilities"].append("effects.v1");
    cp["current"]=impl_->current;
    cp["workflow_input"]=impl_->workflow_input;cp["outputs"]=impl_->outputs;cp["state"]=impl_->state;
    cp["error"]=impl_->error;cp["run"]=impl_->run;cp["step"]=static_cast<Json::UInt64>(impl_->step);
    cp["elapsed_ms"]=static_cast<Json::UInt64>(impl_->elapsed_ms);
    cp["suspended"]=impl_->is_suspended;cp["completed"]=impl_->completed;
    cp["run_mode"]=run_mode_name(impl_->options.mode);cp["allow_external_writes"]=impl_->options.allow_external_writes;
    cp["allow_irreversible"]=impl_->options.allow_irreversible;cp["event_sequence"]=static_cast<Json::UInt64>(impl_->event_sequence);
    cp["deadline_ms"]=static_cast<Json::UInt64>(impl_->options.deadline_ms);
    cp["retry_budget_remaining"]=impl_->retry_remaining;
    cp["trace_capture"]=impl_->options.trace_capture==TraceCapture::Off?"off":
        (impl_->options.trace_capture==TraceCapture::Metadata?"metadata":
         (impl_->options.trace_capture==TraceCapture::Inputs?"inputs":"full"));
    cp["component_identities"]=Json::Value(Json::objectValue);
    for(const auto& component:impl_->options.component_identities)
        cp["component_identities"][component.first]=component.second;
    cp["state_version"]=static_cast<Json::UInt64>(impl_->store_version);
    cp["trace"]["trace_id"]=impl_->trace_id;cp["trace"]["last_span_id"]=impl_->last_span_id;
    cp["trace"]["invocation_id"]=impl_->invocation_id;
    cp["trace"]["current_span_id"]=impl_->current_span_id;cp["trace"]["parent_span_id"]=impl_->parent_span_id;
    cp["trace"]["causation_id"]=impl_->causation_id;
    cp["resource_limits"]["max_output_bytes"]=static_cast<Json::UInt64>(impl_->options.max_output_bytes);
    cp["resource_limits"]["max_state_bytes"]=static_cast<Json::UInt64>(impl_->options.max_state_bytes);
    cp["resource_limits"]["max_events"]=static_cast<Json::UInt64>(impl_->options.max_events);
    cp["resource_limits"]["max_in_flight_tasks"]=impl_->options.max_in_flight_tasks;
    cp["child_record_retention"]=impl_->options.child_record_retention==ChildRecordRetention::Keep?"keep":"delete_on_parent_commit";
    cp["node_status"]=Json::Value(Json::objectValue);
    for(const auto& item:impl_->node_status)cp["node_status"][item.first]=node_status_name(item.second);
    Json::Value hits(Json::objectValue),loops(Json::objectValue);
    for(std::map<std::string,int>::const_iterator i=impl_->hits.begin();i!=impl_->hits.end();++i)hits[i->first]=i->second;
    for(std::map<std::string,int>::const_iterator i=impl_->loops.begin();i!=impl_->loops.end();++i)loops[i->first]=i->second;
    cp["hits"]=hits;cp["loops"]=loops;
    return write_json(cp,true);
}

std::unique_ptr<Run> Engine::create_run(const Workflow& w)const {
    return create_run(w,RunOptions());
}
std::unique_ptr<Run> Engine::create_run(const Workflow& w,const RunOptions& options)const {
    return std::unique_ptr<Run>(new Run(std::unique_ptr<Run::Impl>(
        new Run::Impl(w.impl_->compiled,impl_->executors,impl_->async_executors,impl_->workflows,impl_->policy,impl_->events,impl_->trace_sink,impl_->lease_provider,impl_->scheduler,impl_->timers,options,w.impl_->bundle_digest,impl_->store,impl_->failure_injector))));
}

std::unique_ptr<Run> Engine::create_replay_run(const Workflow& w,const TraceReplayPlan& plan)const{
    if(plan.workflow_digest!=w.impl_->compiled->digest())throw std::invalid_argument("replay Workflow digest mismatch");
    if(plan.bundle_digest!=w.impl_->bundle_digest)throw std::invalid_argument("replay Bundle digest mismatch");
    RunOptions options;options.mode=RunMode::Replay;options.allow_external_writes=false;
    options.allow_irreversible=false;options.trace_capture=TraceCapture::Metadata;
    std::unique_ptr<Run> run=create_run(w,options);run->impl_->deterministic_replay=true;
    run->impl_->replay_state=std::make_shared<Run::Impl::ReplayState>();
    for(const TraceReplayOutcome& outcome:plan.executor_outcomes)
        run->impl_->replay_state->outcomes[outcome.invocation_id+"\n"+outcome.node_id].push_back(outcome.result);
    return run;
}

std::unique_ptr<Run> Engine::restore_run(const Workflow& w,const std::string& checkpoint_json)const{
    Json::Value cp=parse_json(checkpoint_json);
    if(cp.get("format","").asString()!="dage-checkpoint"||cp.get("format_version","").asString()!="0.2.0")
        throw std::runtime_error("unsupported checkpoint format");
    const std::string expected=w.impl_->compiled->digest();
    if(cp.get("workflow_digest","").asString()!=expected)
        throw std::runtime_error("checkpoint workflow mismatch: checkpoint="+
            cp.get("workflow_digest","").asString()+", workflow="+expected);
    if(cp.get("bundle_digest","").asString()!=w.impl_->bundle_digest)
        throw std::runtime_error("checkpoint bundle mismatch");
    RunOptions options;const std::string mode=cp.get("run_mode","normal").asString();
    options.mode=mode=="replay"?RunMode::Replay:(mode=="shadow"?RunMode::Shadow:RunMode::Normal);
    options.allow_external_writes=cp.get("allow_external_writes",true).asBool();
    options.allow_irreversible=cp.get("allow_irreversible",false).asBool();
    options.deadline_ms=cp.get("deadline_ms",0).asUInt64();
    options.retry_budget=cp.get("retry_budget_remaining",0).asUInt();
    const std::string capture=cp.get("trace_capture","metadata").asString();
    options.trace_capture=capture=="off"?TraceCapture::Off:(capture=="inputs"?TraceCapture::Inputs:
        (capture=="full"?TraceCapture::Full:TraceCapture::Metadata));
    for(const std::string& name:cp["component_identities"].getMemberNames())
        options.component_identities[name]=cp["component_identities"][name].asString();
    options.max_output_bytes=cp["resource_limits"].get("max_output_bytes",16*1024*1024).asUInt64();
    options.max_state_bytes=cp["resource_limits"].get("max_state_bytes",32*1024*1024).asUInt64();
    options.max_events=cp["resource_limits"].get("max_events",100000).asUInt64();
    options.max_in_flight_tasks=cp["resource_limits"].get("max_in_flight_tasks",64).asUInt();
    options.child_record_retention=cp.get("child_record_retention","delete_on_parent_commit").asString()=="keep"?
        ChildRecordRetention::Keep:ChildRecordRetention::DeleteOnParentCommit;
    std::unique_ptr<Run> restored=create_run(w,options);Run::Impl& x=*restored->impl_;
    x.current=cp["current"].asString();x.workflow_input=cp["workflow_input"];x.outputs=cp["outputs"];
    x.state=cp["state"];x.error=cp["error"];x.run=cp["run"];x.step=cp["step"].asUInt64();
    x.elapsed_ms=cp["elapsed_ms"].asUInt64();x.is_suspended=cp["suspended"].asBool();
    x.completed=cp["completed"].asBool();x.initialized=true;x.started=std::chrono::steady_clock::now();
    x.event_sequence=cp.get("event_sequence",0).asUInt64();
    x.store_version=cp.get("state_version",0).asUInt64();
    x.trace_id=cp["trace"].get("trace_id","").asString();x.last_span_id=cp["trace"].get("last_span_id","").asString();
    x.invocation_id=cp["trace"].get("invocation_id","root").asString();
    x.current_span_id=cp["trace"].get("current_span_id","").asString();x.parent_span_id=cp["trace"].get("parent_span_id","").asString();
    x.causation_id=cp["trace"].get("causation_id","").asString();
    if(impl_->store&&cp["run"].isMember("id")){
        const std::string run_id=cp["run"]["id"].asString();
        Result<StateRecord> existing=store_load(impl_->store,run_id);
        if(existing){
            const std::string owner="recovery-"+std::to_string(next_recovery_owner.fetch_add(1));
            Result<StateRecord> claimed=store_claim(
                impl_->store,run_id,cp.get("state_version",0).asUInt64(),owner);
            if(!claimed)throw std::runtime_error(claimed.error().code+": "+claimed.error().message);
            x.store_version=claimed.value().version;x.run["recovery_owner"]=owner;
            x.run["recovery_epoch"]=static_cast<Json::UInt64>(claimed.value().epoch);
        }else if(existing.error().code!="STATE_NOT_FOUND")
            throw std::runtime_error(existing.error().code+": "+existing.error().message);
    }
    const std::vector<std::string> status_names=cp["node_status"].getMemberNames();
    for(const std::string& id:status_names){const std::string status=cp["node_status"][id].asString();
        x.node_status[id]=status=="running"?NodeStatus::Running:status=="succeeded"?NodeStatus::Succeeded:
            status=="failed"?NodeStatus::Failed:status=="suspended"?NodeStatus::Suspended:
            status=="skipped"?NodeStatus::Skipped:status=="blocked"?NodeStatus::Blocked:NodeStatus::Pending;}
    for(auto& item:x.node_status)if(item.second==NodeStatus::Running){
        const NodeIR* node=x.workflow->find_node(item.first);
        if(!node||node->replay_policy()==ReplayPolicy::AtMostOnce||node->replay_policy()==ReplayPolicy::Manual||
           node->replay_policy()==ReplayPolicy::Forbidden)
            throw std::runtime_error("checkpoint contains an in-flight node that cannot be safely resumed: "+item.first);
        item.second=NodeStatus::Pending;
    }
    std::vector<std::string> names=cp["hits"].getMemberNames();
    for(std::size_t i=0;i<names.size();++i)x.hits[names[i]]=cp["hits"][names[i]].asInt();
    names=cp["loops"].getMemberNames();
    for(std::size_t i=0;i<names.size();++i)x.loops[names[i]]=cp["loops"][names[i]].asInt();
    ++x.event_sequence;Json::Value restored_payload(Json::objectValue);
    restored_payload["checkpoint_sequence"]=cp.get("event_sequence",0);
    restored_payload["recovery_owner"]=x.run.get("recovery_owner","");
    restored_payload["recovery_epoch"]=x.run.get("recovery_epoch",0);
    x.causation_id=x.last_span_id;
    x.emit("run_restored",x.current,0,nullptr,write_json(restored_payload,false),TraceCapture::Metadata);
    return restored;
}

Value Run::snapshot()const{
    Json::Value snapshot(Json::objectValue);snapshot["run_id"]=impl_->run.get("id","");snapshot["mode"]=run_mode_name(impl_->options.mode);
    snapshot["current_node"]=impl_->current;snapshot["step"]=static_cast<Json::UInt64>(impl_->step);
    snapshot["event_sequence"]=static_cast<Json::UInt64>(impl_->event_sequence);snapshot["suspended"]=impl_->is_suspended;
    snapshot["completed"]=impl_->completed;snapshot["cancelled"]=impl_->cancelled.load();snapshot["workflow_digest"]=impl_->workflow_fingerprint;
    snapshot["bundle_digest"]=impl_->bundle_digest;
    snapshot["retry_budget_remaining"]=impl_->retry_remaining;
    snapshot["nodes"]=Json::Value(Json::objectValue);for(const auto& item:impl_->node_status)snapshot["nodes"][item.first]=node_status_name(item.second);
    return Value::parse(write_json(snapshot,false));
}

ExecutionResult Run::selective_rerun(const std::string& node_id){
    const NodeIR* start=impl_->workflow->find_node(node_id);
    if(!start)return ExecutionResult::fail("invalid_input","RERUN_NODE_NOT_FOUND","Selective rerun node does not exist.");
    if(start->replay_policy()==ReplayPolicy::AtMostOnce||start->replay_policy()==ReplayPolicy::Manual||
       start->replay_policy()==ReplayPolicy::Forbidden)
        return ExecutionResult::fail("effect_blocked","RERUN_POLICY_BLOCKED","Node replay policy forbids selective rerun.");
    std::set<std::string> reachable;std::vector<std::string> pending(1,node_id);
    while(!pending.empty()){const std::string id=pending.back();pending.pop_back();if(!reachable.insert(id).second)continue;
        const NodeIR* node=impl_->workflow->find_node(id);if(!node)continue;
        for(const EdgeIR& edge:node->next_edges())pending.push_back(edge.target());
        for(const EdgeIR& edge:node->error_edges())pending.push_back(edge.target());
        for(const std::string& branch:node->parallel_branches())pending.push_back(branch);
        if(!node->parallel_join().empty())pending.push_back(node->parallel_join());
    }
    for(const std::string& id:reachable){impl_->outputs.removeMember(id);impl_->state["_effects"].removeMember(id);impl_->node_status[id]=NodeStatus::Pending;}
    impl_->current=node_id;impl_->completed=false;impl_->is_suspended=false;impl_->options.mode=RunMode::Replay;
    impl_->started=std::chrono::steady_clock::now();
    return execute(Value::object());
}

} // namespace dage
