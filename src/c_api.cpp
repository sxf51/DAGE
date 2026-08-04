#include "dage/dage.h"
#include "dage/dage.hpp"
#include "dage/bundle.hpp"
#include "dage/bundle_resolver.hpp"
#include "dage/resource_provider.hpp"
#include <json/json.h>
#include <algorithm>

#include <cstring>
#include <cstddef>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <initializer_list>
#include <mutex>

struct CallbackOwner {
    void* userdata;
    dage_userdata_destroy_t destroy;
    CallbackOwner(void* u, dage_userdata_destroy_t d) : userdata(u), destroy(d) {}
    ~CallbackOwner() { if (destroy) { try { destroy(userdata); } catch (...) {} } }
};
struct OutputAllocator {
    dage_allocate_t allocate=nullptr;
    dage_deallocate_t deallocate=nullptr;
    void* userdata=nullptr;
};

struct dage_engine_t {
    dage::Engine engine;
    std::string error;
    std::map<std::string, std::shared_ptr<CallbackOwner> > callbacks;
    OutputAllocator output_allocator;
};
struct dage_workflow_t { std::unique_ptr<dage::Workflow> workflow; };
struct dage_bundle_t { std::unique_ptr<dage::Bundle> bundle; };
struct dage_resolved_bundle_graph_t { std::unique_ptr<dage::ResolvedBundleGraph> graph; };
struct dage_trace_replay_plan_t { dage::TraceReplayPlan plan; };
struct dage_run_t {
    std::unique_ptr<dage::Run> run;
    dage_engine_t* engine;
    std::string pending_output;
    bool has_pending_output;
    dage_run_t() : engine(NULL), has_pending_output(false) {}
};
struct AsyncCompletionState {
    std::shared_ptr<dage::AsyncExecutorCompletion> completion;
    std::shared_ptr<dage::EffectCommitter> committer;
    AsyncCompletionState(std::shared_ptr<dage::AsyncExecutorCompletion> value,
                         std::shared_ptr<dage::EffectCommitter> effect)
        :completion(std::move(value)),committer(std::move(effect)){}
};
struct dage_executor_completion_t {
    std::shared_ptr<AsyncCompletionState> state;
    explicit dage_executor_completion_t(std::shared_ptr<AsyncCompletionState> value):state(std::move(value)){}
};
struct dage_scheduler_task_t {
    std::function<void()> task;
    std::shared_ptr<std::promise<void> > promise;
    dage_scheduler_task_t(std::function<void()> value,std::shared_ptr<std::promise<void> > result)
        :task(std::move(value)),promise(std::move(result)){}
};

namespace {
class CCallbackBase {
public:
    CCallbackBase(void* userdata,dage_userdata_destroy_t destroy):owner_(new CallbackOwner(userdata,destroy)){}
protected:
    std::shared_ptr<CallbackOwner> owner_;
};
template<class F>dage_status_t safe_callback(const F& callback)noexcept{
    try{return callback();}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
class CScheduler final:public dage::Scheduler,private CCallbackBase {
public:
    CScheduler(dage_scheduler_vtable_t table,void* userdata,dage_userdata_destroy_t destroy)
        :CCallbackBase(userdata,destroy),table_(table){}
    std::future<void> schedule(std::function<void()> task)override{return submit(std::move(task),false);}
    std::future<void> schedule_continuation(std::function<void()> task)override{return submit(std::move(task),true);}
private:
    std::future<void> submit(std::function<void()> task,bool continuation){
        std::shared_ptr<std::promise<void> > promise(new std::promise<void>());
        std::future<void> future=promise->get_future();
        std::unique_ptr<dage_scheduler_task_t> handle(new dage_scheduler_task_t(std::move(task),promise));
        const dage_status_t status=safe_callback(
            [&](){return table_.submit(handle.get(),continuation?1:0,owner_->userdata);});
        if(status!=DAGE_STATUS_OK)throw std::runtime_error("C scheduler rejected task");
        handle.release();
        return future;
    }
    dage_scheduler_vtable_t table_;
};
struct OwnedBuffers {
    std::vector<dage_owned_buffer_t> values;
    ~OwnedBuffers(){for(auto& value:values)if(value.release)
        try{value.release(value.data,value.size,value.release_userdata);}catch(...){}}
    std::string take(const dage_owned_buffer_t& value){
        values.push_back(value);
        if(!value.data&&value.size)throw std::invalid_argument("C SPI returned null owned buffer");
        return std::string(value.data?value.data:"",value.size);
    }
};
dage::Error spi_error(dage_status_t status,const char* operation){
    dage::Error error;error.category="c_spi";error.code="CALLBACK_FAILED";
    error.message=std::string(operation)+" callback failed with status "+std::to_string(static_cast<int>(status));
    return error;
}
dage_string_view_t borrowed(const std::string& value){return {value.data(),value.size()};}
std::string compact_json(const Json::Value& value){
    Json::StreamWriterBuilder builder;builder["indentation"]="";builder["emitUTF8"]=true;
    return Json::writeString(builder,value);
}
void release_library_buffer(const char* data,size_t,void*){delete[] data;}
struct alignas(std::max_align_t) AllocatedOutputHeader {
    dage_deallocate_t deallocate;
    void* userdata;
    std::size_t total_size;
    std::size_t alignment;
};
void release_allocated_buffer(const char*,size_t,void* userdata){
    AllocatedOutputHeader* header=static_cast<AllocatedOutputHeader*>(userdata);
    try{header->deallocate(header,header->total_size,header->alignment,header->userdata);}catch(...){}
}
dage_status_t transfer_output(
    const std::string& text,dage_owned_buffer_t* output,const OutputAllocator* allocator=nullptr){
    if(!output)return DAGE_STATUS_INVALID_ARGUMENT;
    *output={};
    try{
        if(allocator&&allocator->allocate){
            const std::size_t alignment=alignof(std::max_align_t);
            const std::size_t total=sizeof(AllocatedOutputHeader)+text.size();
            void* memory=allocator->allocate(total,alignment,allocator->userdata);
            if(!memory)return DAGE_STATUS_OUT_OF_MEMORY;
            auto* header=new(memory) AllocatedOutputHeader{
                allocator->deallocate,allocator->userdata,total,alignment};
            char* data=reinterpret_cast<char*>(header+1);
            if(!text.empty())std::memcpy(data,text.data(),text.size());
            output->data=data;output->size=text.size();output->release=&release_allocated_buffer;
            output->release_userdata=header;return DAGE_STATUS_OK;
        }
        char* data=text.empty()?nullptr:new char[text.size()];
        if(!text.empty())std::memcpy(data,text.data(),text.size());
        output->data=data;output->size=text.size();output->release=&release_library_buffer;
        return DAGE_STATUS_OK;
    }catch(const std::bad_alloc&){return DAGE_STATUS_OUT_OF_MEMORY;}
    catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
class CStateStore final:public dage::StateStore,private CCallbackBase {
public:
    CStateStore(dage_state_store_vtable_t table,void* userdata,dage_userdata_destroy_t destroy)
        :CCallbackBase(userdata,destroy),table_(table){}
    dage::Result<bool> put(const std::string& id,const std::string& checkpoint)override{
        const auto status=safe_callback([&](){return table_.put(borrowed(id),borrowed(checkpoint),owner_->userdata);});
        return status==DAGE_STATUS_OK?dage::Result<bool>::success(true):
            dage::Result<bool>::failure(spi_error(status,"put"));
    }
    dage::Result<std::string> get(const std::string& id)const override{
        dage_owned_buffer_t output{};OwnedBuffers owned;owned.values.push_back(output);
        const auto status=safe_callback([&](){return table_.get(borrowed(id),&output,owner_->userdata);});
        owned.values.back()=output;
        if(status!=DAGE_STATUS_OK)return dage::Result<std::string>::failure(spi_error(status,"get"));
        try{
            if(!output.data&&output.size)throw std::invalid_argument("C SPI returned null owned buffer");
            return dage::Result<std::string>::success(std::string(output.data?output.data:"",output.size));}
        catch(const std::exception& ex){dage::Error error=spi_error(DAGE_STATUS_INVALID_ARGUMENT,"get");error.message=ex.what();
            return dage::Result<std::string>::failure(std::move(error));}
    }
    dage::Result<bool> erase(const std::string& id)override{
        const auto status=safe_callback([&](){return table_.erase(borrowed(id),owner_->userdata);});
        return status==DAGE_STATUS_OK?dage::Result<bool>::success(true):
            dage::Result<bool>::failure(spi_error(status,"erase"));
    }
    dage::Result<dage::StateRecord> load(const std::string& id)const override{return record("load",[&](dage_state_record_t* out){
        return table_.load(borrowed(id),out,owner_->userdata);});}
    dage::Result<std::uint64_t> compare_exchange(const std::string& id,std::uint64_t expected,
                                                 const std::string& checkpoint)override{
        std::uint64_t version=0;const auto status=safe_callback([&](){return table_.compare_exchange(
            borrowed(id),expected,borrowed(checkpoint),&version,owner_->userdata);});
        return status==DAGE_STATUS_OK?dage::Result<std::uint64_t>::success(version):
            dage::Result<std::uint64_t>::failure(spi_error(status,"compare_exchange"));
    }
    dage::Result<dage::StateRecord> claim(const std::string& id,std::uint64_t expected,
                                          const std::string& owner)override{return record("claim",[&](dage_state_record_t* out){
        return table_.claim(borrowed(id),expected,borrowed(owner),out,owner_->userdata);});}
    dage::Result<std::vector<std::string> > list(const std::string& prefix)const override{
        dage_owned_buffer_t output{};OwnedBuffers owned;owned.values.push_back(output);
        const auto status=safe_callback([&](){return table_.list(borrowed(prefix),&output,owner_->userdata);});
        owned.values.back()=output;
        if(status!=DAGE_STATUS_OK)return dage::Result<std::vector<std::string> >::failure(spi_error(status,"list"));
        try{
            if(!output.data&&output.size)throw std::invalid_argument("C SPI returned null owned buffer");
            const std::string json(output.data?output.data:"",output.size);
            Json::CharReaderBuilder builder;Json::Value value;std::string errors;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if(!reader->parse(json.data(),json.data()+json.size(),&value,&errors)||!value.isArray())
                throw std::invalid_argument("StateStore list output must be a JSON array");
            std::vector<std::string> result;for(const Json::Value& item:value){
                if(!item.isString())throw std::invalid_argument("run id must be a string");
                result.push_back(item.asString());}
            return dage::Result<std::vector<std::string> >::success(std::move(result));
        }catch(const std::exception& ex){dage::Error error=spi_error(DAGE_STATUS_INVALID_ARGUMENT,"list");error.message=ex.what();
            return dage::Result<std::vector<std::string> >::failure(std::move(error));}
    }
private:
    template<class F>dage::Result<dage::StateRecord> record(const char* operation,const F& callback)const{
        dage_state_record_t output{};output.struct_size=sizeof(output);
        OwnedBuffers owned;owned.values.push_back(output.owner);owned.values.push_back(output.checkpoint);
        const auto status=safe_callback([&](){return callback(&output);});
        owned.values[0]=output.owner;owned.values[1]=output.checkpoint;
        if(status!=DAGE_STATUS_OK)return dage::Result<dage::StateRecord>::failure(spi_error(status,operation));
        try{dage::StateRecord result;result.version=output.version;result.epoch=output.epoch;
            if((!output.owner.data&&output.owner.size)||(!output.checkpoint.data&&output.checkpoint.size))
                throw std::invalid_argument("C SPI returned null StateRecord buffer");
            result.owner.assign(output.owner.data?output.owner.data:"",output.owner.size);
            result.checkpoint.assign(output.checkpoint.data?output.checkpoint.data:"",output.checkpoint.size);
            return dage::Result<dage::StateRecord>::success(std::move(result));
        }catch(const std::exception& ex){dage::Error error=spi_error(DAGE_STATUS_INVALID_ARGUMENT,operation);error.message=ex.what();
            return dage::Result<dage::StateRecord>::failure(std::move(error));}
    }
    dage_state_store_vtable_t table_;
};
class CTraceSink final:public dage::TraceSink,private CCallbackBase {
public:
    CTraceSink(dage_trace_sink_vtable_t table,void* userdata,dage_userdata_destroy_t destroy)
        :CCallbackBase(userdata,destroy),table_(table){}
    void emit(const dage::EventEnvelope& event)noexcept override{
        try{Json::Value value(Json::objectValue);
            value["schema_version"]=event.schema_version;value["sequence"]=Json::UInt64(event.sequence);
            value["timestamp_unix_ms"]=Json::UInt64(event.timestamp_unix_ms);
            value["elapsed_ms"]=Json::UInt64(event.elapsed_ms);value["attempt"]=event.attempt;
            value["event"]=event.event;value["run_id"]=event.run_id;value["invocation_id"]=event.invocation_id;
            value["workflow_digest"]=event.workflow_digest;value["bundle_digest"]=event.bundle_digest;
            value["node_id"]=event.node_id;value["node_type"]=event.node_type;value["executor"]=event.executor;
            value["category"]=event.category;value["code"]=event.code;
            Json::CharReaderBuilder builder;std::string errors;Json::Value payload;
            const std::string payload_text=event.payload_json.empty()?"{}":event.payload_json;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if(!reader->parse(payload_text.data(),payload_text.data()+payload_text.size(),&payload,&errors))
                payload=Json::Value(Json::objectValue);
            value["payload"]=std::move(payload);value["trace_id"]=event.trace_id;value["span_id"]=event.span_id;
            value["parent_span_id"]=event.parent_span_id;value["causation_id"]=event.causation_id;
            const std::string json=compact_json(value);table_.emit(borrowed(json),owner_->userdata);
        }catch(...){}
    }
private:dage_trace_sink_vtable_t table_;
};
class CResourceLease final:public dage::ResourceLease {
public:
    explicit CResourceLease(dage_resource_lease_t lease):lease_(lease){
        if(!lease_.lease_id.data&&lease_.lease_id.size)throw std::invalid_argument("lease id is null");
        id_.assign(lease_.lease_id.data?lease_.lease_id.data:"",lease_.lease_id.size);
        if(lease_.lease_id.release)try{lease_.lease_id.release(
            lease_.lease_id.data,lease_.lease_id.size,lease_.lease_id.release_userdata);}catch(...){}
        lease_.lease_id={};
    }
    ~CResourceLease()override{if(lease_.release){try{lease_.release(lease_.lease_userdata);}catch(...){}}}
    const std::string& lease_id()const noexcept override{return id_;}
    std::uint64_t fencing_token()const noexcept override{return lease_.fencing_token;}
    std::uint64_t expires_at_unix_ms()const noexcept override{return lease_.expires_at_unix_ms;}
    dage::Result<std::uint64_t> renew(std::uint64_t ttl)override{
        if(!lease_.renew)return dage::Result<std::uint64_t>::failure(spi_error(DAGE_STATUS_INVALID_ARGUMENT,"renew"));
        std::uint64_t expires=0;const auto status=safe_callback(
            [&](){return lease_.renew(ttl,&expires,lease_.lease_userdata);});
        if(status!=DAGE_STATUS_OK)return dage::Result<std::uint64_t>::failure(spi_error(status,"renew"));
        lease_.expires_at_unix_ms=expires;return dage::Result<std::uint64_t>::success(expires);
    }
private:dage_resource_lease_t lease_;std::string id_;
};
class CResourceLeaseProvider final:public dage::ResourceLeaseProvider,private CCallbackBase {
public:
    CResourceLeaseProvider(dage_resource_lease_provider_vtable_t table,void* userdata,dage_userdata_destroy_t destroy)
        :CCallbackBase(userdata,destroy),table_(table){}
    dage::Result<std::unique_ptr<dage::ResourceLease> > acquire(
        const dage::ResourceRequest& request,const dage::CancellationToken* cancellation)override{
        struct Cancel {const dage::CancellationToken* token;static uint8_t check(void* value){
            const Cancel* self=static_cast<const Cancel*>(value);return self&&self->token&&self->token->is_cancelled()?1:0;
        }} cancel{cancellation};
        Json::Value value(Json::objectValue);value["run_id"]=request.run_id;value["node_id"]=request.node_id;
        value["node_type"]=request.node_type;value["executor"]=request.executor;value["units"]=request.units;
        value["priority"]=request.priority;value["fairness_key"]=request.fairness_key;
        value["lease_ttl_ms"]=Json::UInt64(request.lease_ttl_ms);
        value["deadline_remaining_ms"]=Json::UInt64(request.deadline_remaining_ms);
        value["resources"]=Json::Value(Json::objectValue);for(const auto& item:request.resources)
            value["resources"][item.first]=Json::UInt64(item.second);
        const std::string json=compact_json(value);
        dage_resource_lease_t lease{};lease.struct_size=sizeof(lease);
        const auto status=safe_callback(
            [&](){return table_.acquire(borrowed(json),&Cancel::check,&cancel,&lease,owner_->userdata);});
        if(status!=DAGE_STATUS_OK){
            if(lease.lease_id.release)try{lease.lease_id.release(
                lease.lease_id.data,lease.lease_id.size,lease.lease_id.release_userdata);}catch(...){}
            if(lease.release)try{lease.release(lease.lease_userdata);}catch(...){}
            return dage::Result<std::unique_ptr<dage::ResourceLease> >::failure(spi_error(status,"acquire"));
        }
        if(lease.struct_size<offsetof(dage_resource_lease_t,reserved)||!lease.release){
            if(lease.lease_id.release)try{lease.lease_id.release(
                lease.lease_id.data,lease.lease_id.size,lease.lease_id.release_userdata);}catch(...){}
            if(lease.release)try{lease.release(lease.lease_userdata);}catch(...){}
            return dage::Result<std::unique_ptr<dage::ResourceLease> >::failure(spi_error(DAGE_STATUS_INVALID_ARGUMENT,"acquire"));
        }
        try{return dage::Result<std::unique_ptr<dage::ResourceLease> >::success(
            std::unique_ptr<dage::ResourceLease>(new CResourceLease(lease)));
        }catch(...){
            if(lease.lease_id.release)try{lease.lease_id.release(
                lease.lease_id.data,lease.lease_id.size,lease.lease_id.release_userdata);}catch(...){}
            if(lease.release)try{lease.release(lease.lease_userdata);}catch(...){}
            throw;}
    }
private:dage_resource_lease_provider_vtable_t table_;
};
class CResourceProvider final:public dage::ResourceProvider {
public:
    CResourceProvider(dage_resource_provider_t provider,bool owns)
        :provider_(provider),owns_(owns){}
    ~CResourceProvider()override{
        if(owns_&&provider_.release)try{provider_.release(provider_.userdata);}catch(...){}
    }
    dage::Result<std::vector<std::string> > list_resources()const override{
        dage_owned_buffer_t output{};OwnedBuffers owned;owned.values.push_back(output);
        const auto status=safe_callback(
            [&](){return provider_.list_resources(&output,provider_.userdata);});
        owned.values.back()=output;
        if(status!=DAGE_STATUS_OK)return dage::Result<std::vector<std::string> >::failure(
            spi_error(status,"resource_provider.list"));
        try{
            if(!output.data&&output.size)throw std::invalid_argument("provider returned null list");
            const std::string json(output.data?output.data:"",output.size);
            Json::CharReaderBuilder builder;Json::Value value;std::string errors;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if(!reader->parse(json.data(),json.data()+json.size(),&value,&errors)||!value.isArray())
                throw std::invalid_argument("provider list must be a JSON array");
            std::vector<std::string> result;for(const Json::Value& item:value){
                if(!item.isString())throw std::invalid_argument("resource path must be a string");
                result.push_back(item.asString());}
            return dage::Result<std::vector<std::string> >::success(std::move(result));
        }catch(const std::exception& ex){dage::Error error=spi_error(DAGE_STATUS_INVALID_ARGUMENT,"resource_provider.list");
            error.message=ex.what();return dage::Result<std::vector<std::string> >::failure(std::move(error));}
    }
    dage::Result<dage::ResourceBytes> read_resource(const std::string& path)const override{
        dage_owned_buffer_t output{};OwnedBuffers owned;owned.values.push_back(output);
        const auto status=safe_callback(
            [&](){return provider_.read_resource(borrowed(path),&output,provider_.userdata);});
        owned.values.back()=output;
        if(status!=DAGE_STATUS_OK)return dage::Result<dage::ResourceBytes>::failure(
            spi_error(status,"resource_provider.read"));
        if(!output.data&&output.size)return dage::Result<dage::ResourceBytes>::failure(
            spi_error(DAGE_STATUS_INVALID_ARGUMENT,"resource_provider.read"));
        if(!output.size)return dage::Result<dage::ResourceBytes>::success({});
        const auto* begin=reinterpret_cast<const std::uint8_t*>(output.data);
        return dage::Result<dage::ResourceBytes>::success(
            dage::ResourceBytes(begin,begin+output.size));
    }
private:dage_resource_provider_t provider_;bool owns_;
};
class CBundleRepository final:public dage::BundleRepository {
public:
    CBundleRepository(dage_bundle_repository_vtable_t table,void* userdata)
        :table_(table),userdata_(userdata){}
    dage::Result<std::vector<std::string> > available_versions(
        const std::string& id,bool offline)const override{
        dage_owned_buffer_t output{};OwnedBuffers owned;owned.values.push_back(output);
        const auto status=safe_callback([&](){return table_.available_versions(
            borrowed(id),offline?1:0,&output,userdata_);});owned.values.back()=output;
        if(status!=DAGE_STATUS_OK)return dage::Result<std::vector<std::string> >::failure(
            spi_error(status,"repository.available_versions"));
        try{
            if(!output.data&&output.size)throw std::invalid_argument("repository returned null versions");
            const std::string json(output.data?output.data:"",output.size);
            Json::CharReaderBuilder builder;Json::Value value;std::string errors;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if(!reader->parse(json.data(),json.data()+json.size(),&value,&errors)||!value.isArray())
                throw std::invalid_argument("versions must be a JSON array");
            std::vector<std::string> result;for(const Json::Value& item:value){
                if(!item.isString())throw std::invalid_argument("version must be a string");
                result.push_back(item.asString());}
            return dage::Result<std::vector<std::string> >::success(std::move(result));
        }catch(const std::exception& ex){dage::Error error=spi_error(DAGE_STATUS_INVALID_ARGUMENT,"repository.available_versions");
            error.message=ex.what();return dage::Result<std::vector<std::string> >::failure(std::move(error));}
    }
    dage::Result<std::shared_ptr<const dage::ResourceProvider> > get(
        const std::string& id,const std::string& version,bool offline)const override{
        dage_resource_provider_t provider{};provider.struct_size=sizeof(provider);
        const auto status=safe_callback([&](){return table_.get(
            borrowed(id),borrowed(version),offline?1:0,&provider,userdata_);});
        auto cleanup=[&](){if(provider.release)try{provider.release(provider.userdata);}catch(...){}};
        if(status!=DAGE_STATUS_OK){cleanup();return dage::Result<std::shared_ptr<const dage::ResourceProvider> >::failure(
            spi_error(status,"repository.get"));}
        if(provider.struct_size<offsetof(dage_resource_provider_t,reserved)||
           !provider.list_resources||!provider.read_resource||!provider.release){
            cleanup();return dage::Result<std::shared_ptr<const dage::ResourceProvider> >::failure(
                spi_error(DAGE_STATUS_INVALID_ARGUMENT,"repository.get"));}
        return dage::Result<std::shared_ptr<const dage::ResourceProvider> >::success(
            std::make_shared<CResourceProvider>(provider,true));
    }
    std::string source()const override{
        dage_owned_buffer_t output{};OwnedBuffers owned;owned.values.push_back(output);
        const auto status=safe_callback([&](){return table_.source(&output,userdata_);});
        owned.values.back()=output;if(status!=DAGE_STATUS_OK)throw std::runtime_error("REPOSITORY_SOURCE_FAILED");
        if(!output.data&&output.size)throw std::runtime_error("REPOSITORY_SOURCE_INVALID");
        return std::string(output.data?output.data:"",output.size);
    }
private:dage_bundle_repository_vtable_t table_;void* userdata_;
};
std::string signature_context_json(const dage::SignatureContext& context){
    Json::Value value(Json::objectValue);value["bundle_id"]=context.bundle_id;
    value["bundle_version"]=context.bundle_version;value["bundle_digest"]=context.bundle_digest;
    value["key_id"]=context.key_id;value["algorithm"]=context.algorithm;value["signed_at"]=context.signed_at;
    return compact_json(value);
}
Json::Value diagnostics_json(const std::vector<dage::Diagnostic>& diagnostics){
    Json::Value result(Json::arrayValue);for(const auto& diagnostic:diagnostics){
        Json::Value value(Json::objectValue);
        value["severity"]=diagnostic.severity==dage::Severity::Error?"error":
            diagnostic.severity==dage::Severity::Warning?"warning":"info";
        value["code"]=diagnostic.code;value["path"]=diagnostic.path;
        value["message"]=diagnostic.message;value["suggestion"]=diagnostic.suggestion;
        result.append(std::move(value));}
    return result;
}
Json::Value strings_json(const std::vector<std::string>& values){
    Json::Value result(Json::arrayValue);for(const std::string& value:values)result.append(value);return result;
}
Json::Value execution_result_json(const dage::ExecutionResult& result){
    Json::Value value(Json::objectValue);value["success"]=result.success;
    if(result.success)value["output"]=Json::Value(Json::nullValue);
    if(result.success){
        Json::CharReaderBuilder builder;std::string errors;std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const std::string json=result.output.to_json(false);
        if(!reader->parse(json.data(),json.data()+json.size(),&value["output"],&errors))
            value["output"]=Json::Value(Json::nullValue);
    }else{
        value["error"]["category"]=result.error.category;value["error"]["code"]=result.error.code;
        value["error"]["message"]=result.error.message;value["error"]["node_id"]=result.error.node_id;
        value["error"]["attempt"]=result.error.attempt;value["error"]["retryable"]=result.error.retryable;
    }
    return value;
}
dage::ExecutionResult parse_execution_result(const Json::Value& value){
    if(!value.isObject()||!value["success"].isBool())throw std::invalid_argument("sample result is invalid");
    if(value["success"].asBool()){
        if(!value.isMember("output"))throw std::invalid_argument("successful sample has no output");
        return dage::ExecutionResult::ok(dage::Value::parse(compact_json(value["output"])));
    }
    const Json::Value& error=value["error"];if(!error.isObject())throw std::invalid_argument("failed sample has no error");
    dage::ExecutionResult result=dage::ExecutionResult::fail(
        error.get("category","execution_error").asString(),error.get("code","FAILED").asString(),
        error.get("message","Execution failed.").asString(),error.get("retryable",false).asBool());
    result.error.node_id=error.get("node_id","").asString();result.error.attempt=error.get("attempt",0).asUInt();
    return result;
}
dage::EventEnvelope parse_event(const Json::Value& value){
    if(!value.isObject()||!value["event"].isString())throw std::invalid_argument("trace event is invalid");
    dage::EventEnvelope event;event.schema_version=value.get("schema_version",1).asUInt();
    event.sequence=value.get("sequence",0).asUInt64();event.timestamp_unix_ms=value.get("timestamp_unix_ms",0).asUInt64();
    event.elapsed_ms=value.get("elapsed_ms",0).asUInt64();event.attempt=value.get("attempt",0).asUInt();
    event.event=value["event"].asString();event.run_id=value.get("run_id","").asString();
    event.invocation_id=value.get("invocation_id","").asString();event.workflow_digest=value.get("workflow_digest","").asString();
    event.bundle_digest=value.get("bundle_digest","").asString();event.node_id=value.get("node_id","").asString();
    event.node_type=value.get("node_type","").asString();event.executor=value.get("executor","").asString();
    event.category=value.get("category","").asString();event.code=value.get("code","").asString();
    event.trace_id=value.get("trace_id","").asString();event.span_id=value.get("span_id","").asString();
    event.parent_span_id=value.get("parent_span_id","").asString();event.causation_id=value.get("causation_id","").asString();
    if(value["payload_json"].isString())event.payload_json=value["payload_json"].asString();
    else event.payload_json=compact_json(value.isMember("payload")?value["payload"]:Json::Value(Json::objectValue));
    return event;
}
std::vector<dage::EventEnvelope> parse_events(const Json::Value& value){
    if(!value.isArray())throw std::invalid_argument("trace must be a JSON array");
    std::vector<dage::EventEnvelope> events;events.reserve(value.size());
    for(const Json::Value& event:value)events.push_back(parse_event(event));
    return events;
}
Json::Value event_json(const dage::EventEnvelope& event){
    Json::Value item(Json::objectValue);item["schema_version"]=event.schema_version;
    item["sequence"]=Json::UInt64(event.sequence);item["timestamp_unix_ms"]=Json::UInt64(event.timestamp_unix_ms);
    item["elapsed_ms"]=Json::UInt64(event.elapsed_ms);item["attempt"]=event.attempt;item["event"]=event.event;
    item["run_id"]=event.run_id;item["invocation_id"]=event.invocation_id;
    item["workflow_digest"]=event.workflow_digest;item["bundle_digest"]=event.bundle_digest;
    item["node_id"]=event.node_id;item["node_type"]=event.node_type;item["executor"]=event.executor;
    item["category"]=event.category;item["code"]=event.code;item["trace_id"]=event.trace_id;
    item["span_id"]=event.span_id;item["parent_span_id"]=event.parent_span_id;
    item["causation_id"]=event.causation_id;item["payload_json"]=event.payload_json;return item;
}
dage::EvaluationSample parse_sample(const std::string& json){
    Json::CharReaderBuilder builder;Json::Value value;std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(!reader->parse(json.data(),json.data()+json.size(),&value,&errors)||!value.isObject())
        throw std::invalid_argument("evaluation sample is invalid JSON");
    dage::EvaluationSample sample;sample.label=value.get("label","").asString();
    sample.workflow_digest=value.get("workflow_digest","").asString();
    sample.result=parse_execution_result(value["result"]);
    sample.trace=parse_events(value["trace"]);return sample;
}
std::string sample_json(const dage::EvaluationSample& sample){
    Json::Value value(Json::objectValue);value["label"]=sample.label;value["workflow_digest"]=sample.workflow_digest;
    value["result"]=execution_result_json(sample.result);value["trace"]=Json::Value(Json::arrayValue);
    for(const auto& event:sample.trace)value["trace"].append(event_json(event));
    return compact_json(value);
}
class CKeyProvider final:public dage::KeyProvider {
public:
    CKeyProvider(dage_key_provider_vtable_t table,void* userdata):table_(table),userdata_(userdata){}
    dage::Result<dage::PublicKey> find_key(
        const std::string& id,const dage::SignatureContext& context)const override{
        const std::string json=signature_context_json(context);dage_owned_buffer_t output{};
        OwnedBuffers owned;owned.values.push_back(output);const auto status=safe_callback(
            [&](){return table_.find_key(borrowed(id),borrowed(json),&output,userdata_);});
        owned.values.back()=output;if(status!=DAGE_STATUS_OK)
            return dage::Result<dage::PublicKey>::failure(spi_error(status,"key_provider.find_key"));
        if(!output.data||output.size!=32)return dage::Result<dage::PublicKey>::failure(
            spi_error(DAGE_STATUS_INVALID_ARGUMENT,"key_provider.find_key"));
        dage::PublicKey key;const auto* begin=reinterpret_cast<const std::uint8_t*>(output.data);
        key.bytes.assign(begin,begin+output.size);return dage::Result<dage::PublicKey>::success(std::move(key));
    }
private:dage_key_provider_vtable_t table_;void* userdata_;
};
class CTrustPolicy final:public dage::TrustPolicy {
public:
    CTrustPolicy(dage_trust_policy_vtable_t table,void* userdata):table_(table),userdata_(userdata){}
    dage::Result<bool> trust(const dage::SignatureContext& context)const override{
        const std::string json=signature_context_json(context);std::uint8_t trusted=0;
        const auto status=safe_callback([&](){return table_.trust(borrowed(json),&trusted,userdata_);});
        if(status!=DAGE_STATUS_OK)return dage::Result<bool>::failure(spi_error(status,"trust_policy.trust"));
        if(trusted>1)return dage::Result<bool>::failure(spi_error(DAGE_STATUS_INVALID_ARGUMENT,"trust_policy.trust"));
        return dage::Result<bool>::success(trusted!=0);
    }
private:dage_trust_policy_vtable_t table_;void* userdata_;
};
dage_status_t copy_output(const std::string& text, char* buffer, size_t size, size_t* required) {
    if (!required) return DAGE_STATUS_INVALID_ARGUMENT;
    *required = text.size() + 1;
    if (!buffer || size < *required) return DAGE_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(buffer, text.c_str(), *required);
    return DAGE_STATUS_OK;
}
std::string string_of(dage_string_view_t v) {
    if (!v.data && v.size) throw std::invalid_argument("null string data");
    return std::string(v.data ? v.data : "", v.size);
}
dage::ExecutionResult owned_result(dage_status_t status,const dage_owned_buffer_t* output) {
    struct Release {const dage_owned_buffer_t* value;~Release(){
        if(value&&value->release)value->release(value->data,value->size,value->release_userdata);}} release={output};
    if(status!=DAGE_STATUS_OK)
        return dage::ExecutionResult::fail(status==DAGE_STATUS_CANCELLED?"cancelled":"tool_error",
            status==DAGE_STATUS_CANCELLED?"ASYNC_CANCELLED":"ASYNC_CALLBACK_FAILED","Async C executor failed.");
    if(!output||(!output->data&&output->size))
        return dage::ExecutionResult::fail("invalid_output","NULL_OUTPUT","Async executor returned invalid output.");
    try{return dage::ExecutionResult::ok(dage::Value::parse(std::string(output->data?output->data:"",output->size)));}
    catch(...){return dage::ExecutionResult::fail("invalid_output","INVALID_JSON","Async executor output is not JSON.");}
}
template <typename F>
dage_status_t guard(dage_engine_t* engine, const F& function) {
    try { return function(); }
    catch (const std::bad_alloc&) { if(engine)engine->error="out of memory"; return DAGE_STATUS_OUT_OF_MEMORY; }
    catch (const std::invalid_argument& e) { if(engine)engine->error=e.what(); return DAGE_STATUS_INVALID_ARGUMENT; }
    catch (const std::exception& e) { if(engine)engine->error=e.what();
        return std::string(e.what()).find("WORKFLOW_")==0?
            DAGE_STATUS_RESOURCE_EXHAUSTED:DAGE_STATUS_VALIDATION_ERROR; }
    catch (...) { if(engine)engine->error="unknown exception"; return DAGE_STATUS_UNKNOWN_ERROR; }
}
const std::vector<std::string>& runtime_capabilities(){
    static const std::vector<std::string> values={
        "core.dag.v1","c.async_executor.v1","c.runtime_spi.v1","c.bundle_resolver.v1",
        "c.analysis.v1","c.trace_replay.v1","c.shadow_metrics.v1","c.output_allocator.v1"};
    return values;
}
std::string runtime_registry_json(){
    Json::Value root(Json::objectValue);root["schema_version"]=1;
    root["runtime_version"]=DAGE_RUNTIME_VERSION_STRING;root["c_abi_version"]=DAGE_C_ABI_VERSION;
    root["capabilities"]=Json::Value(Json::arrayValue);
    for(const std::string& capability:runtime_capabilities())root["capabilities"].append(capability);
    const std::pair<const char*,int> statuses[]={
        {"ok",DAGE_STATUS_OK},{"invalid_argument",DAGE_STATUS_INVALID_ARGUMENT},
        {"out_of_memory",DAGE_STATUS_OUT_OF_MEMORY},{"parse_error",DAGE_STATUS_PARSE_ERROR},
        {"validation_error",DAGE_STATUS_VALIDATION_ERROR},{"execution_error",DAGE_STATUS_EXECUTION_ERROR},
        {"cancelled",DAGE_STATUS_CANCELLED},{"not_found",DAGE_STATUS_NOT_FOUND},
        {"buffer_too_small",DAGE_STATUS_BUFFER_TOO_SMALL},{"suspended",DAGE_STATUS_SUSPENDED},
        {"resource_exhausted",DAGE_STATUS_RESOURCE_EXHAUSTED},
        {"internal_error",DAGE_STATUS_INTERNAL_ERROR},{"unknown_error",DAGE_STATUS_UNKNOWN_ERROR}};
    for(const auto& status:statuses)root["statuses"][status.first]=status.second;
    root["enums"]["run_mode"]["normal"]=DAGE_RUN_MODE_NORMAL;
    root["enums"]["run_mode"]["replay"]=DAGE_RUN_MODE_REPLAY;
    root["enums"]["run_mode"]["shadow"]=DAGE_RUN_MODE_SHADOW;
    root["enums"]["trace_capture"]["metadata"]=DAGE_TRACE_CAPTURE_METADATA;
    root["enums"]["trace_capture"]["off"]=DAGE_TRACE_CAPTURE_OFF;
    root["enums"]["trace_capture"]["inputs"]=DAGE_TRACE_CAPTURE_INPUTS;
    root["enums"]["trace_capture"]["full"]=DAGE_TRACE_CAPTURE_FULL;
    root["enums"]["bundle_load_mode"]["development"]=DAGE_BUNDLE_LOAD_DEVELOPMENT;
    root["enums"]["bundle_load_mode"]["frozen"]=DAGE_BUNDLE_LOAD_FROZEN;
    root["enums"]["bundle_load_mode"]["verified"]=DAGE_BUNDLE_LOAD_VERIFIED;
    root["enums"]["bundle_load_mode"]["offline"]=DAGE_BUNDLE_LOAD_OFFLINE;
    root["enums"]["signature_policy"]["disabled"]=DAGE_SIGNATURE_DISABLED;
    root["enums"]["signature_policy"]["at_least_one"]=DAGE_SIGNATURE_AT_LEAST_ONE;
    root["enums"]["signature_policy"]["threshold"]=DAGE_SIGNATURE_THRESHOLD;
    root["enums"]["signature_policy"]["all"]=DAGE_SIGNATURE_ALL;
    root["minimum_struct_size"]["engine_options"]=Json::UInt64(offsetof(dage_engine_options_t,reserved));
    root["minimum_struct_size"]["run_options"]=Json::UInt64(offsetof(dage_run_options_t,reserved));
    root["minimum_struct_size"]["allocator"]=Json::UInt64(offsetof(dage_allocator_t,reserved));
    root["minimum_struct_size"]["scheduler_vtable"]=Json::UInt64(offsetof(dage_scheduler_vtable_t,reserved));
    root["minimum_struct_size"]["state_store_vtable"]=Json::UInt64(offsetof(dage_state_store_vtable_t,reserved));
    root["minimum_struct_size"]["trace_sink_vtable"]=Json::UInt64(offsetof(dage_trace_sink_vtable_t,reserved));
    root["minimum_struct_size"]["resource_lease_provider_vtable"]=
        Json::UInt64(offsetof(dage_resource_lease_provider_vtable_t,reserved));
    root["analysis_result_kinds"]=Json::Value(Json::arrayValue);
    for(const char* kind:{"patch_dry_run","patch_analysis","workflow_diff","trace_replay_plan","shadow_evaluation"})
        root["analysis_result_kinds"].append(kind);
    root["node_types"]=Json::Value(Json::arrayValue);
    for(const char* type:{"tool","llm","transform","condition","parallel","join","subflow","human","noop","end","fail","switch"})
        root["node_types"].append(type);
    auto fields=[&](const char* schema,std::initializer_list<const char*> names){
        root["fields"][schema]=Json::Value(Json::arrayValue);
        for(const char* name:names)root["fields"][schema].append(name);};
    fields("event_envelope",{"schema_version","sequence","timestamp_unix_ms","elapsed_ms","attempt",
        "event","run_id","invocation_id","workflow_digest","bundle_digest","node_id","node_type",
        "executor","category","code","payload","trace_id","span_id","parent_span_id","causation_id"});
    fields("execution_context",{"struct_size","run_id","node_id","node_type","attempt","idempotency_key",
        "run_mode","deadline_remaining_ms","commit_effect","commit_effect_userdata","is_cancelled",
        "cancellation_userdata"});
    fields("engine_options",{"struct_size","api_version","output_allocator","max_workflow_bytes",
        "max_json_depth","max_nodes","max_edges","max_expression_bytes","max_literal_bytes",
        "max_compiled_ir_bytes"});
    fields("patch_dry_run",{"schema_version","kind","valid","base_digest","candidate_digest",
        "candidate_revision","diagnostics"});
    fields("patch_analysis",{"schema_version","kind","valid","base_digest","candidate_digest",
        "directly_changed_nodes","affected_nodes","effect_policy_changed",
        "checkpoint_resume_compatible","inverse_patch_json","diagnostics"});
    fields("workflow_diff",{"schema_version","kind","before_digest","after_digest",
        "changed_workflow_fields","node_changes","affected_nodes","effect_policy_changed",
        "checkpoint_resume_compatible"});
    fields("trace_replay_plan",{"schema_version","kind","source_run_id","trace_id","workflow_digest",
        "bundle_digest","workflow_input","executor_outcome_count","expected_result"});
    fields("shadow_evaluation",{"schema_version","kind","baseline_label","candidate_label",
        "candidate_is_shadow","outputs_equal","recommended","weighted_score_delta","metrics"});
    auto callback=[&](const char* name,const char* threads,bool concurrent,bool may_block,const char* ownership){
        root["callbacks"][name]["threads"]=threads;root["callbacks"][name]["concurrent"]=concurrent;
        root["callbacks"][name]["may_block"]=may_block;root["callbacks"][name]["ownership"]=ownership;};
    callback("executor","scheduler_worker",true,true,"inputs borrowed; output transferred");
    callback("async_executor","scheduler_worker",true,false,"completion transferred one-shot");
    callback("scheduler.submit","submitting_thread",true,false,"task transferred only on OK");
    callback("state_store","scheduler_worker",true,true,"inputs borrowed; outputs transferred");
    callback("trace_sink.emit","emitting_thread",true,false,"event borrowed");
    callback("resource_lease.acquire","scheduler_worker",true,true,"lease transferred on OK");
    callback("repository","resolver_caller",false,true,"dependency provider transferred");
    callback("key_provider","resolver_caller",false,true,"public key transferred");
    callback("trust_policy","resolver_caller",false,true,"context borrowed");
    callback("metric","shadow_compare_caller",false,true,"sample borrowed");
    return compact_json(root);
}
}

extern "C" {

uint8_t dage_runtime_has_capability(dage_string_view_t capability){
    if(!capability.data&&capability.size)return 0;
    try{const std::string value(capability.data?capability.data:"",capability.size);
        return std::find(runtime_capabilities().begin(),runtime_capabilities().end(),value)
            !=runtime_capabilities().end()?1:0;
    }catch(...){return 0;}
}
dage_status_t dage_runtime_registry(char* buffer,size_t size,size_t* required){
    try{return copy_output(runtime_registry_json(),buffer,size,required);}
    catch(const std::bad_alloc&){return DAGE_STATUS_OUT_OF_MEMORY;}
    catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}

dage_status_t dage_engine_create(const dage_engine_options_t* options, dage_engine_handle* out) {
    if (!out) return DAGE_STATUS_INVALID_ARGUMENT;
    *out = NULL;
    if(options&&(options->struct_size<offsetof(dage_engine_options_t,reserved)||
       options->api_version>DAGE_C_ABI_VERSION))
        return DAGE_STATUS_INVALID_ARGUMENT;
    if(options&&options->output_allocator&&
       (options->output_allocator->struct_size<offsetof(dage_allocator_t,reserved)||
        !options->output_allocator->allocate||!options->output_allocator->deallocate))
        return DAGE_STATUS_INVALID_ARGUMENT;
    try {
        std::unique_ptr<dage_engine_t> engine(new dage_engine_t());
        if(options&&options->output_allocator){
            engine->output_allocator.allocate=options->output_allocator->allocate;
            engine->output_allocator.deallocate=options->output_allocator->deallocate;
            engine->output_allocator.userdata=options->output_allocator->userdata;
        }
        if(options){
            dage::WorkflowResourceLimits limits;
            if(options->max_workflow_bytes)limits.max_workflow_bytes=options->max_workflow_bytes;
            if(options->max_json_depth)limits.max_json_depth=options->max_json_depth;
            if(options->max_nodes)limits.max_nodes=options->max_nodes;
            if(options->max_edges)limits.max_edges=options->max_edges;
            if(options->max_expression_bytes)limits.max_expression_bytes=options->max_expression_bytes;
            if(options->max_literal_bytes)limits.max_literal_bytes=options->max_literal_bytes;
            if(options->max_compiled_ir_bytes)limits.max_compiled_ir_bytes=options->max_compiled_ir_bytes;
            engine->engine.set_workflow_resource_limits(limits);
        }
        *out=engine.release();return DAGE_STATUS_OK;
    }
    catch (const std::bad_alloc&) { return DAGE_STATUS_OUT_OF_MEMORY; }
    catch (...) { return DAGE_STATUS_UNKNOWN_ERROR; }
}
void dage_engine_destroy(dage_engine_handle engine) { delete engine; }
dage_status_t dage_engine_set_scheduler(dage_engine_handle engine,const dage_scheduler_vtable_t* vtable,
                                        void* userdata,dage_userdata_destroy_t destroy){
    if(!engine||!vtable||vtable->struct_size<offsetof(dage_scheduler_vtable_t,reserved)||!vtable->submit)
        return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        engine->engine.set_scheduler(std::make_shared<CScheduler>(*vtable,userdata,destroy));
        return DAGE_STATUS_OK;
    });
}
dage_status_t dage_scheduler_task_run(dage_scheduler_task_handle task){
    if(!task)return DAGE_STATUS_INVALID_ARGUMENT;
    std::unique_ptr<dage_scheduler_task_t> owned(task);
    try{owned->task();owned->promise->set_value();return DAGE_STATUS_OK;}
    catch(...){try{owned->promise->set_exception(std::current_exception());}catch(...){}
        return DAGE_STATUS_EXECUTION_ERROR;}
}
void dage_scheduler_task_abandon(dage_scheduler_task_handle task){
    if(!task)return;
    std::unique_ptr<dage_scheduler_task_t> owned(task);
    try{owned->promise->set_exception(std::make_exception_ptr(std::runtime_error("C scheduler abandoned task")));}
    catch(...){}
}
dage_status_t dage_engine_set_state_store(dage_engine_handle engine,const dage_state_store_vtable_t* vtable,
                                          void* userdata,dage_userdata_destroy_t destroy){
    if(!engine||!vtable||vtable->struct_size<offsetof(dage_state_store_vtable_t,reserved)||
       !vtable->put||!vtable->get||!vtable->erase||!vtable->load||
       !vtable->compare_exchange||!vtable->claim||!vtable->list)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        engine->engine.set_state_store(std::make_shared<CStateStore>(*vtable,userdata,destroy));
        return DAGE_STATUS_OK;
    });
}
dage_status_t dage_engine_set_trace_sink(dage_engine_handle engine,const dage_trace_sink_vtable_t* vtable,
                                         void* userdata,dage_userdata_destroy_t destroy){
    if(!engine||!vtable||vtable->struct_size<offsetof(dage_trace_sink_vtable_t,reserved)||!vtable->emit)
        return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        engine->engine.set_trace_sink(std::make_shared<CTraceSink>(*vtable,userdata,destroy));
        return DAGE_STATUS_OK;
    });
}
dage_status_t dage_engine_set_resource_lease_provider(
    dage_engine_handle engine,const dage_resource_lease_provider_vtable_t* vtable,
    void* userdata,dage_userdata_destroy_t destroy){
    if(!engine||!vtable||vtable->struct_size<offsetof(dage_resource_lease_provider_vtable_t,reserved)||!vtable->acquire)
        return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        engine->engine.set_resource_lease_provider(
            std::make_shared<CResourceLeaseProvider>(*vtable,userdata,destroy));
        return DAGE_STATUS_OK;
    });
}

dage_status_t dage_engine_register_executor(dage_engine_handle engine, dage_string_view_t name,
                                             dage_executor_callback_t callback, void* userdata,
                                             dage_userdata_destroy_t destroy) {
    if (!engine || !callback) return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine, [&]() -> dage_status_t {
        const std::string n = string_of(name);
        std::shared_ptr<CallbackOwner> owner(new CallbackOwner(userdata, destroy));
        engine->callbacks[n] = owner;
        engine->engine.register_executor(n, [callback, owner](const dage::ExecutionContext& ctx, const dage::Value& input) {
            const std::string in = input.to_json(false);
            struct CommitBridge {
                dage::EffectCommitter* committer;
                const dage::CancellationToken* cancellation;
                static void commit(void* value) {
                    CommitBridge* bridge=static_cast<CommitBridge*>(value);
                    if(bridge&&bridge->committer)bridge->committer->commit();
                }
                static uint8_t cancelled(void* value) {
                    CommitBridge* bridge=static_cast<CommitBridge*>(value);
                    return bridge&&bridge->cancellation&&bridge->cancellation->is_cancelled()?1:0;
                }
            } bridge={ctx.effect_committer,ctx.cancellation};
            dage_execution_context_t context;
            std::memset(&context,0,sizeof(context));context.struct_size=sizeof(context);
            context.run_id={ctx.run_id.data(),ctx.run_id.size()};context.node_id={ctx.node_id.data(),ctx.node_id.size()};
            context.node_type={ctx.node_type.data(),ctx.node_type.size()};context.attempt=ctx.attempt;
            context.idempotency_key={ctx.idempotency_key.data(),ctx.idempotency_key.size()};
            context.run_mode={ctx.run_mode.data(),ctx.run_mode.size()};
            context.deadline_remaining_ms=ctx.deadline_remaining_ms;
            context.commit_effect=ctx.effect_committer?&CommitBridge::commit:NULL;
            context.commit_effect_userdata=ctx.effect_committer?&bridge:NULL;
            context.is_cancelled=&CommitBridge::cancelled;
            context.cancellation_userdata=&bridge;
            dage_string_view_t input_view = {in.data(), in.size()};
            dage_owned_buffer_t output;
            std::memset(&output,0,sizeof(output));
            dage_status_t status = callback(&context, input_view, &output, owner->userdata);
            if(status==DAGE_STATUS_SUSPENDED)
                return dage::ExecutionResult::fail("suspended","AWAITING_HUMAN","Executor suspended the run.");
            if (status != DAGE_STATUS_OK)
                return dage::ExecutionResult::fail("tool_error", "CALLBACK_FAILED", "C executor callback failed.");
            struct OutputRelease { dage_owned_buffer_t* value;~OutputRelease(){
                if(value->release)value->release(value->data,value->size,value->release_userdata);}} release={&output};
            if(!output.data&&output.size)return dage::ExecutionResult::fail("invalid_output","NULL_OUTPUT","Executor returned a null output buffer.");
            try { return dage::ExecutionResult::ok(dage::Value::parse(std::string(output.data?output.data:"",output.size))); }
            catch (...) { return dage::ExecutionResult::fail("invalid_output", "INVALID_JSON", "Executor output is not JSON."); }
        });
        return DAGE_STATUS_OK;
    });
}
dage_status_t dage_engine_register_async_executor(dage_engine_handle engine,dage_string_view_t name,
                                                   dage_async_executor_callback_t callback,void* userdata,
                                                   dage_userdata_destroy_t destroy){
    if(!engine||!callback)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        const std::string n=string_of(name);std::shared_ptr<CallbackOwner> owner(new CallbackOwner(userdata,destroy));
        engine->callbacks[n]=owner;
        engine->engine.register_async_executor(n,[callback,owner](const dage::ExecutionContext& ctx,const dage::Value& input,
                                                                  const std::shared_ptr<dage::AsyncExecutorCompletion>& native_completion){
            std::shared_ptr<AsyncCompletionState> state(new AsyncCompletionState(native_completion,ctx.effect_committer_owner));
            dage_executor_completion_handle completion=new dage_executor_completion_t(state);
            dage_execution_context_t context;std::memset(&context,0,sizeof(context));context.struct_size=sizeof(context);
            context.run_id={ctx.run_id.data(),ctx.run_id.size()};context.node_id={ctx.node_id.data(),ctx.node_id.size()};
            context.node_type={ctx.node_type.data(),ctx.node_type.size()};context.attempt=ctx.attempt;
            context.idempotency_key={ctx.idempotency_key.data(),ctx.idempotency_key.size()};
            context.run_mode={ctx.run_mode.data(),ctx.run_mode.size()};context.deadline_remaining_ms=ctx.deadline_remaining_ms;
            const std::string in=input.to_json(false);dage_string_view_t input_view={in.data(),in.size()};
            dage_status_t started=callback(&context,input_view,completion,owner->userdata);
            if(started!=DAGE_STATUS_OK){
                native_completion->complete(dage::ExecutionResult::fail("tool_error","ASYNC_START_FAILED","Async C executor did not start."));
                delete completion;
            }
        });return DAGE_STATUS_OK;
    });
}
dage_status_t dage_executor_complete(dage_executor_completion_handle completion,dage_status_t status,
                                     const dage_owned_buffer_t* output){
    if(!completion)return DAGE_STATUS_INVALID_ARGUMENT;
    std::unique_ptr<dage_executor_completion_t> owned(completion);
    try{return owned->state->completion->complete(owned_result(status,output))?DAGE_STATUS_OK:DAGE_STATUS_INVALID_ARGUMENT;}
    catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
void dage_executor_abandon(dage_executor_completion_handle completion){
    if(!completion)return;
    std::unique_ptr<dage_executor_completion_t> owned(completion);
    try{owned->state->completion->complete(dage::ExecutionResult::fail("cancelled","ASYNC_ABANDONED","Async operation abandoned."));}catch(...){}
}
uint8_t dage_executor_completion_is_cancelled(dage_executor_completion_handle completion){
    return completion&&completion->state->completion->cancelled()?1:0;
}
dage_status_t dage_executor_completion_commit_effect(dage_executor_completion_handle completion){
    if(!completion||!completion->state->committer)return DAGE_STATUS_INVALID_ARGUMENT;
    completion->state->committer->commit();return DAGE_STATUS_OK;
}
dage_status_t dage_engine_register_workflow(dage_engine_handle engine,dage_string_view_t name,dage_string_view_t json){
    if(!engine)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{engine->engine.register_workflow(string_of(name),string_of(json));return DAGE_STATUS_OK;});
}

dage_status_t dage_engine_load(dage_engine_handle engine, dage_string_view_t json, dage_workflow_handle* out) {
    if (!engine || !out) return DAGE_STATUS_INVALID_ARGUMENT;
    *out = NULL;
    return guard(engine, [&]() -> dage_status_t {
        std::unique_ptr<dage_workflow_t> handle(new dage_workflow_t());
        handle->workflow = engine->engine.load(string_of(json));
        *out = handle.release();
        return DAGE_STATUS_OK;
    });
}
dage_status_t dage_bundle_load(dage_engine_handle engine,const dage_resource_entry_t* entries,
                               size_t entry_count,dage_bundle_handle* out){
    if(!engine||!out||(!entries&&entry_count))return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{
        dage::MemoryResourceProvider provider;
        for(size_t i=0;i<entry_count;++i){
            const std::string path=string_of(entries[i].path);const std::string content=string_of(entries[i].content);
            provider.add(path,dage::ResourceBytes(content.begin(),content.end()));
        }
        std::unique_ptr<dage_bundle_t> handle(new dage_bundle_t());handle->bundle=engine->engine.load_bundle(provider);
        *out=handle.release();return DAGE_STATUS_OK;
    });
}
void dage_bundle_destroy(dage_bundle_handle bundle){delete bundle;}
dage_status_t dage_bundle_resolve(
    dage_engine_handle engine,const dage_resource_provider_t* root,
    const dage_bundle_resolver_options_t* options,
    const dage_bundle_repository_vtable_t* repository,void* repository_userdata,
    const dage_key_provider_vtable_t* keys,void* keys_userdata,
    const dage_trust_policy_vtable_t* trust,void* trust_userdata,
    dage_resolved_bundle_graph_handle* out){
    if(!engine||!root||!out||
       root->struct_size<offsetof(dage_resource_provider_t,reserved)||
       !root->list_resources||!root->read_resource)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    if(options&&options->struct_size<offsetof(dage_bundle_resolver_options_t,reserved))
        return DAGE_STATUS_INVALID_ARGUMENT;
    if(repository&&(repository->struct_size<offsetof(dage_bundle_repository_vtable_t,reserved)||
       !repository->available_versions||!repository->get||!repository->source))
        return DAGE_STATUS_INVALID_ARGUMENT;
    if(keys&&(keys->struct_size<offsetof(dage_key_provider_vtable_t,reserved)||!keys->find_key))
        return DAGE_STATUS_INVALID_ARGUMENT;
    if(trust&&(trust->struct_size<offsetof(dage_trust_policy_vtable_t,reserved)||!trust->trust))
        return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        dage::BundleResolverOptions cpp;
        if(options){
            switch(options->mode){
            case DAGE_BUNDLE_LOAD_DEVELOPMENT:cpp.mode=dage::BundleLoadMode::Development;break;
            case DAGE_BUNDLE_LOAD_FROZEN:cpp.mode=dage::BundleLoadMode::Frozen;break;
            case DAGE_BUNDLE_LOAD_VERIFIED:cpp.mode=dage::BundleLoadMode::Verified;break;
            case DAGE_BUNDLE_LOAD_OFFLINE:cpp.mode=dage::BundleLoadMode::Offline;break;
            default:return DAGE_STATUS_INVALID_ARGUMENT;
            }
            switch(options->signature_policy){
            case DAGE_SIGNATURE_DISABLED:cpp.signature_policy.kind=dage::SignaturePolicyKind::Disabled;break;
            case DAGE_SIGNATURE_AT_LEAST_ONE:cpp.signature_policy.kind=dage::SignaturePolicyKind::AtLeastOne;break;
            case DAGE_SIGNATURE_THRESHOLD:cpp.signature_policy.kind=dage::SignaturePolicyKind::Threshold;break;
            case DAGE_SIGNATURE_ALL:cpp.signature_policy.kind=dage::SignaturePolicyKind::All;break;
            default:return DAGE_STATUS_INVALID_ARGUMENT;
            }
            cpp.signature_policy.threshold=options->signature_threshold;
            cpp.lock_json=string_of(options->lock_json);
        }
        std::unique_ptr<CBundleRepository> repository_adapter;
        std::unique_ptr<CKeyProvider> key_adapter;std::unique_ptr<CTrustPolicy> trust_adapter;
        if(repository){repository_adapter.reset(new CBundleRepository(*repository,repository_userdata));
            cpp.repository=repository_adapter.get();}
        if(keys){key_adapter.reset(new CKeyProvider(*keys,keys_userdata));cpp.keys=key_adapter.get();}
        if(trust){trust_adapter.reset(new CTrustPolicy(*trust,trust_userdata));cpp.trust=trust_adapter.get();}
        CResourceProvider root_adapter(*root,false);dage::BundleResolver resolver(cpp);
        auto resolved=resolver.resolve(root_adapter);
        if(!resolved){engine->error=resolved.error().code+": "+resolved.error().message;
            return DAGE_STATUS_VALIDATION_ERROR;}
        std::unique_ptr<dage_resolved_bundle_graph_t> result(new dage_resolved_bundle_graph_t());
        result->graph=std::move(resolved.value());*out=result.release();return DAGE_STATUS_OK;
    });
}
void dage_resolved_bundle_graph_destroy(dage_resolved_bundle_graph_handle graph){delete graph;}
dage_status_t dage_resolved_bundle_graph_lock(
    dage_resolved_bundle_graph_handle graph,char* buffer,size_t size,size_t* required){
    if(!graph)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(graph->graph->lock_json(),buffer,size,required);}
    catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_resolved_bundle_graph_root_id(
    dage_resolved_bundle_graph_handle graph,char* buffer,size_t size,size_t* required){
    if(!graph)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(graph->graph->root().id(),buffer,size,required);}
    catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_resolved_bundle_graph_load_workflow(
    dage_engine_handle engine,dage_resolved_bundle_graph_handle graph,
    dage_string_view_t bundle_id,dage_string_view_t workflow_id,dage_workflow_handle* out){
    if(!engine||!graph||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{
        const std::string id=string_of(bundle_id);
        const dage::Bundle* bundle=id.empty()?&graph->graph->root():graph->graph->find(id);
        if(!bundle){engine->error="BUNDLE_NOT_FOUND: "+id;return DAGE_STATUS_NOT_FOUND;}
        std::unique_ptr<dage_workflow_t> result(new dage_workflow_t());
        result->workflow=engine->engine.load_workflow(*bundle,string_of(workflow_id));
        *out=result.release();return DAGE_STATUS_OK;
    });
}
dage_status_t dage_bundle_load_workflow(dage_engine_handle engine,dage_bundle_handle bundle,
                                        dage_string_view_t workflow_id,dage_workflow_handle*out){
    if(!engine||!bundle||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{
        std::unique_ptr<dage_workflow_t> handle(new dage_workflow_t());
        handle->workflow=engine->engine.load_workflow(*bundle->bundle,string_of(workflow_id));
        *out=handle.release();return DAGE_STATUS_OK;
    });
}
void dage_workflow_destroy(dage_workflow_handle workflow) { delete workflow; }
dage_status_t dage_workflow_apply_patch(dage_engine_handle engine,dage_workflow_handle workflow,
                                        dage_string_view_t patch,dage_workflow_handle*out){
    if(!engine||!workflow||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{std::unique_ptr<dage_workflow_t> result(new dage_workflow_t());
        result->workflow=engine->engine.apply_patch(*workflow->workflow,string_of(patch));*out=result.release();return DAGE_STATUS_OK;});
}
dage_status_t dage_workflow_dry_run_patch(
    dage_engine_handle engine,dage_workflow_handle workflow,dage_string_view_t patch,
    char* buffer,size_t size,size_t* required){
    if(!engine||!workflow)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        const dage::PatchDryRunResult result=engine->engine.dry_run_patch(
            *workflow->workflow,string_of(patch));Json::Value value(Json::objectValue);
        value["schema_version"]=1;value["kind"]="patch_dry_run";value["valid"]=result.valid;
        value["base_digest"]=result.base_digest;value["candidate_digest"]=result.candidate_digest;
        value["candidate_revision"]=Json::UInt64(result.candidate_revision);
        value["diagnostics"]=diagnostics_json(result.diagnostics);
        return copy_output(compact_json(value),buffer,size,required);
    });
}
dage_status_t dage_workflow_analyze_patch(
    dage_engine_handle engine,dage_workflow_handle workflow,dage_string_view_t patch,
    char* buffer,size_t size,size_t* required){
    if(!engine||!workflow)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        const dage::PatchAnalysis result=engine->engine.analyze_patch(
            *workflow->workflow,string_of(patch));Json::Value value(Json::objectValue);
        value["schema_version"]=1;value["kind"]="patch_analysis";value["valid"]=result.valid;
        value["base_digest"]=result.base_digest;value["candidate_digest"]=result.candidate_digest;
        value["directly_changed_nodes"]=strings_json(result.directly_changed_nodes);
        value["affected_nodes"]=strings_json(result.affected_nodes);
        value["effect_policy_changed"]=result.effect_policy_changed;
        value["checkpoint_resume_compatible"]=result.checkpoint_resume_compatible;
        value["inverse_patch_json"]=result.inverse_patch_json;
        value["diagnostics"]=diagnostics_json(result.diagnostics);
        return copy_output(compact_json(value),buffer,size,required);
    });
}
dage_status_t dage_workflow_diff(
    dage_engine_handle engine,dage_workflow_handle before,dage_workflow_handle after,
    char* buffer,size_t size,size_t* required){
    if(!engine||!before||!after)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(engine,[&]()->dage_status_t{
        const dage::WorkflowDiff result=engine->engine.diff_workflows(
            *before->workflow,*after->workflow);Json::Value value(Json::objectValue);
        value["schema_version"]=1;value["kind"]="workflow_diff";
        value["before_digest"]=result.before_digest;value["after_digest"]=result.after_digest;
        value["changed_workflow_fields"]=strings_json(result.changed_workflow_fields);
        value["affected_nodes"]=strings_json(result.affected_nodes);
        value["effect_policy_changed"]=result.effect_policy_changed;
        value["checkpoint_resume_compatible"]=result.checkpoint_resume_compatible;
        value["node_changes"]=Json::Value(Json::arrayValue);
        for(const auto& change:result.node_changes){Json::Value item(Json::objectValue);
            item["node_id"]=change.node_id;item["kind"]=change.kind==dage::WorkflowNodeChangeKind::Added?"added":
                change.kind==dage::WorkflowNodeChangeKind::Removed?"removed":"modified";
            item["changed_fields"]=strings_json(change.changed_fields);
            item["effect_policy_changed"]=change.effect_policy_changed;
            value["node_changes"].append(std::move(item));}
        return copy_output(compact_json(value),buffer,size,required);
    });
}
dage_status_t dage_workflow_format(dage_workflow_handle w,char*b,size_t s,size_t*r) {
    if(!w)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(w->workflow->normalized_json(),b,s,r);}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_workflow_export_mermaid(dage_workflow_handle w,char*b,size_t s,size_t*r) {
    if(!w)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(w->workflow->export_mermaid(),b,s,r);}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_workflow_export_dot(dage_workflow_handle w,char*b,size_t s,size_t*r) {
    if(!w)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(w->workflow->export_dot(),b,s,r);}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_run_create(dage_engine_handle engine,dage_workflow_handle workflow,dage_run_handle*out) {
    return dage_run_create_with_options(engine,workflow,NULL,out);
}
dage_status_t dage_run_create_with_options(dage_engine_handle engine,dage_workflow_handle workflow,
                                            const dage_run_options_t* options,dage_run_handle*out) {
    if(!engine||!workflow||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    if(options&&options->struct_size<offsetof(dage_run_options_t,reserved))
        return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{std::unique_ptr<dage_run_t>h(new dage_run_t());dage::RunOptions cpp_options;
        if(options){cpp_options.mode=options->mode==DAGE_RUN_MODE_REPLAY?dage::RunMode::Replay:
            options->mode==DAGE_RUN_MODE_SHADOW?dage::RunMode::Shadow:dage::RunMode::Normal;
            cpp_options.allow_external_writes=options->allow_external_writes!=0;cpp_options.allow_irreversible=options->allow_irreversible!=0;
            switch(options->trace_capture){
            case DAGE_TRACE_CAPTURE_METADATA:cpp_options.trace_capture=dage::TraceCapture::Metadata;break;
            case DAGE_TRACE_CAPTURE_OFF:cpp_options.trace_capture=dage::TraceCapture::Off;break;
            case DAGE_TRACE_CAPTURE_INPUTS:cpp_options.trace_capture=dage::TraceCapture::Inputs;break;
            case DAGE_TRACE_CAPTURE_FULL:cpp_options.trace_capture=dage::TraceCapture::Full;break;
            default:return DAGE_STATUS_INVALID_ARGUMENT;
            }}
        if(options){cpp_options.deadline_ms=options->deadline_ms;
            cpp_options.retry_budget=options->retry_budget;
            if(options->max_output_bytes)cpp_options.max_output_bytes=options->max_output_bytes;
            if(options->max_state_bytes)cpp_options.max_state_bytes=options->max_state_bytes;
            if(options->max_events)cpp_options.max_events=options->max_events;
            if(options->max_in_flight_tasks)cpp_options.max_in_flight_tasks=options->max_in_flight_tasks;}
        h->run=engine->engine.create_run(*workflow->workflow,cpp_options);
        h->engine=engine;*out=h.release();return DAGE_STATUS_OK;});
}
void dage_run_destroy(dage_run_handle run){delete run;}
dage_status_t dage_run_execute(dage_run_handle h,dage_string_view_t in,char*b,size_t s,size_t*r) {
    if(!h)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(h->engine,[&]()->dage_status_t{
        if(!h->has_pending_output){
            std::mutex mutex;std::condition_variable ready;bool done=false;
            dage::ExecutionResult result=dage::ExecutionResult::fail("internal_error","NOT_COMPLETED","Run did not complete.");
            h->run->execute_async(dage::Value::parse(string_of(in)),[&](const dage::ExecutionResult& value){
                {std::lock_guard<std::mutex> lock(mutex);result=value;done=true;}ready.notify_one();
            });
            {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&](){return done;});}
            if(!result.success){h->engine->error=result.error.code+": "+result.error.message;
                if(result.error.category=="cancelled")return DAGE_STATUS_CANCELLED;
                if(result.error.category=="suspended")return DAGE_STATUS_SUSPENDED;
                return DAGE_STATUS_EXECUTION_ERROR;}
            h->pending_output=result.output.to_json(false);h->has_pending_output=true;
        }
        dage_status_t status=copy_output(h->pending_output,b,s,r);
        if(status==DAGE_STATUS_OK){h->pending_output.clear();h->has_pending_output=false;}
        return status;});
}
static dage_status_t copy_run_result(dage_run_handle h,const dage::ExecutionResult& result,
                                     char*b,size_t s,size_t*r){
    if(!result.success){h->engine->error=result.error.code+": "+result.error.message;
        if(result.error.category=="cancelled")return DAGE_STATUS_CANCELLED;
        if(result.error.category=="suspended")return DAGE_STATUS_SUSPENDED;
        return DAGE_STATUS_EXECUTION_ERROR;}
    h->pending_output=result.output.to_json(false);h->has_pending_output=true;
    dage_status_t status=copy_output(h->pending_output,b,s,r);
    if(status==DAGE_STATUS_OK){h->pending_output.clear();h->has_pending_output=false;}
    return status;
}
dage_status_t dage_run_resume(dage_run_handle h,dage_string_view_t value,char*b,size_t s,size_t*r){
    if(!h)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(h->engine,[&]()->dage_status_t{
        if(h->has_pending_output){
            dage_status_t status=copy_output(h->pending_output,b,s,r);
            if(status==DAGE_STATUS_OK){h->pending_output.clear();h->has_pending_output=false;}
            return status;
        }
        return copy_run_result(h,h->run->resume(dage::Value::parse(string_of(value))),b,s,r);
    });
}
dage_status_t dage_run_selective_rerun(
    dage_run_handle h,dage_string_view_t node_id,char* buffer,size_t size,size_t* required){
    if(!h)return DAGE_STATUS_INVALID_ARGUMENT;
    return guard(h->engine,[&]()->dage_status_t{
        if(h->has_pending_output){
            const dage_status_t status=copy_output(h->pending_output,buffer,size,required);
            if(status==DAGE_STATUS_OK){h->pending_output.clear();h->has_pending_output=false;}
            return status;
        }
        return copy_run_result(h,h->run->selective_rerun(string_of(node_id)),buffer,size,required);
    });
}
dage_status_t dage_trace_replay_prepare(
    dage_engine_handle engine,dage_string_view_t events_json,
    dage_string_view_t workflow_digest,dage_string_view_t bundle_digest,
    dage_trace_replay_plan_handle* out){
    if(!engine||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{
        const std::string json=string_of(events_json);Json::CharReaderBuilder builder;
        if(json.size()>64u*1024u*1024u){engine->error="REPLAY_TRACE_TOO_LARGE";
            return DAGE_STATUS_INVALID_ARGUMENT;}
        Json::Value value;std::string errors;std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        if(!reader->parse(json.data(),json.data()+json.size(),&value,&errors)){
            engine->error="REPLAY_TRACE_INVALID: "+errors;return DAGE_STATUS_PARSE_ERROR;}
        auto prepared=dage::prepare_trace_replay(
            parse_events(value),string_of(workflow_digest),string_of(bundle_digest));
        if(!prepared){engine->error=prepared.error().code+": "+prepared.error().message;
            return DAGE_STATUS_VALIDATION_ERROR;}
        std::unique_ptr<dage_trace_replay_plan_t> result(new dage_trace_replay_plan_t());
        result->plan=std::move(prepared.value());*out=result.release();return DAGE_STATUS_OK;
    });
}
void dage_trace_replay_plan_destroy(dage_trace_replay_plan_handle plan){delete plan;}
dage_status_t dage_trace_replay_plan_input(
    dage_trace_replay_plan_handle plan,char* buffer,size_t size,size_t* required){
    if(!plan)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(plan->plan.workflow_input.to_json(false),buffer,size,required);}
    catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_trace_replay_plan_describe(
    dage_trace_replay_plan_handle plan,char* buffer,size_t size,size_t* required){
    if(!plan)return DAGE_STATUS_INVALID_ARGUMENT;
    try{Json::Value value(Json::objectValue);value["schema_version"]=1;value["kind"]="trace_replay_plan";
        value["source_run_id"]=plan->plan.source_run_id;value["trace_id"]=plan->plan.trace_id;
        value["workflow_digest"]=plan->plan.workflow_digest;value["bundle_digest"]=plan->plan.bundle_digest;
        value["workflow_input"]=Json::Value(Json::nullValue);
        Json::CharReaderBuilder builder;std::string errors;std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const std::string input=plan->plan.workflow_input.to_json(false);
        reader->parse(input.data(),input.data()+input.size(),&value["workflow_input"],&errors);
        value["executor_outcome_count"]=Json::UInt64(plan->plan.executor_outcomes.size());
        value["expected_result"]=execution_result_json(plan->plan.expected_result);
        return copy_output(compact_json(value),buffer,size,required);
    }catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_run_create_replay(
    dage_engine_handle engine,dage_workflow_handle workflow,
    dage_trace_replay_plan_handle plan,dage_run_handle* out){
    if(!engine||!workflow||!plan||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{
        std::unique_ptr<dage_run_t> result(new dage_run_t());
        result->run=engine->engine.create_replay_run(*workflow->workflow,plan->plan);
        result->engine=engine;*out=result.release();return DAGE_STATUS_OK;
    });
}
dage_status_t dage_shadow_compare(
    dage_engine_handle engine,dage_string_view_t baseline_json,dage_string_view_t candidate_json,
    const dage_metric_definition_t* metrics,size_t metric_count,
    dage_owned_buffer_t* report_json){
    if(!engine||!report_json||(!metrics&&metric_count)||metric_count>1024)
        return DAGE_STATUS_INVALID_ARGUMENT;
    *report_json={};
    return guard(engine,[&]()->dage_status_t{
        const std::string baseline_text=string_of(baseline_json),candidate_text=string_of(candidate_json);
        if(baseline_text.size()>64u*1024u*1024u||candidate_text.size()>64u*1024u*1024u)
            return DAGE_STATUS_INVALID_ARGUMENT;
        const dage::EvaluationSample baseline=parse_sample(baseline_text);
        const dage::EvaluationSample candidate=parse_sample(candidate_text);
        std::vector<dage::MetricDefinition> definitions;definitions.reserve(metric_count);
        for(size_t i=0;i<metric_count;++i){
            const dage_metric_definition_t& metric=metrics[i];
            if(metric.struct_size<offsetof(dage_metric_definition_t,reserved)||!metric.evaluate)
                return DAGE_STATUS_INVALID_ARGUMENT;
            dage::MetricDefinition definition;definition.name=string_of(metric.name);
            definition.weight=metric.weight;definition.regression_tolerance=metric.regression_tolerance;
            definition.evaluate=[metric](const dage::EvaluationSample& sample)->dage::Result<double>{
                const std::string json=sample_json(sample);double value=0.0;
                const dage_status_t status=safe_callback(
                    [&](){return metric.evaluate(borrowed(json),&value,metric.userdata);});
                if(status!=DAGE_STATUS_OK)return dage::Result<double>::failure(
                    spi_error(status,"metric.evaluate"));
                return dage::Result<double>::success(value);
            };
            definitions.push_back(std::move(definition));
        }
        auto compared=dage::compare_shadow_evaluation(baseline,candidate,definitions);
        if(!compared){engine->error=compared.error().code+": "+compared.error().message;
            return DAGE_STATUS_VALIDATION_ERROR;}
        const dage::ShadowEvaluationReport& report=compared.value();Json::Value value(Json::objectValue);
        value["schema_version"]=1;value["kind"]="shadow_evaluation";
        value["baseline_label"]=report.baseline_label;value["candidate_label"]=report.candidate_label;
        value["candidate_is_shadow"]=report.candidate_is_shadow;value["outputs_equal"]=report.outputs_equal;
        value["recommended"]=report.recommended;value["weighted_score_delta"]=report.weighted_score_delta;
        value["metrics"]=Json::Value(Json::arrayValue);
        for(const auto& comparison:report.metrics){Json::Value item(Json::objectValue);
            item["name"]=comparison.name;item["baseline"]=comparison.baseline;
            item["candidate"]=comparison.candidate;item["delta"]=comparison.delta;
            item["weighted_delta"]=comparison.weighted_delta;item["regressed"]=comparison.regressed;
            value["metrics"].append(std::move(item));}
        return transfer_output(compact_json(value),report_json,&engine->output_allocator);
    });
}
dage_status_t dage_run_checkpoint(dage_run_handle h,char*b,size_t s,size_t*r){
    if(!h)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(h->run->checkpoint(),b,s,r);}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_run_snapshot(dage_run_handle h,char*b,size_t s,size_t*r){
    if(!h)return DAGE_STATUS_INVALID_ARGUMENT;
    try{return copy_output(h->run->snapshot().to_json(false),b,s,r);}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_run_restore(dage_engine_handle engine,dage_workflow_handle workflow,
                               dage_string_view_t checkpoint,dage_run_handle*out){
    if(!engine||!workflow||!out)return DAGE_STATUS_INVALID_ARGUMENT;
    *out=NULL;
    return guard(engine,[&]()->dage_status_t{
        std::unique_ptr<dage_run_t> h(new dage_run_t());
        h->run=engine->engine.restore_run(*workflow->workflow,string_of(checkpoint));
        h->engine=engine;*out=h.release();return DAGE_STATUS_OK;
    });
}
void dage_run_cancel(dage_run_handle run){if(run)run->run->cancel();}
dage_status_t dage_run_cancel_with_reason(dage_run_handle run,dage_string_view_t reason){
    if(!run)return DAGE_STATUS_INVALID_ARGUMENT;
    try{run->run->cancel(string_of(reason));return DAGE_STATUS_OK;}catch(...){return DAGE_STATUS_UNKNOWN_ERROR;}
}
dage_status_t dage_last_error(dage_engine_handle e,char*b,size_t s,size_t*r){
    if(!e)return DAGE_STATUS_INVALID_ARGUMENT;
    return copy_output(e->error,b,s,r);
}
}
