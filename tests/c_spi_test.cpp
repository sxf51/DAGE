#include "dage/dage.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

struct Host {
    std::mutex mutex;
    std::map<std::string,std::pair<uint64_t,std::string> > states;
    int scheduled=0,traces=0,leases=0,releases=0;
};

static std::string text(dage_string_view_t value){return std::string(value.data?value.data:"",value.size);}
static void free_buffer(const char* data,size_t,void*){std::free(const_cast<char*>(data));}
static dage_owned_buffer_t owned(const std::string& value){
    dage_owned_buffer_t result{};char* data=static_cast<char*>(std::malloc(value.size()));
    if(!data&&value.size())return result;
    if(value.size())std::memcpy(data,value.data(),value.size());
    result.data=data;result.size=value.size();result.release=&free_buffer;return result;
}
static dage_status_t schedule(dage_scheduler_task_handle task,uint8_t,void* userdata){
    ++static_cast<Host*>(userdata)->scheduled;return dage_scheduler_task_run(task);
}
static void trace(dage_string_view_t event,void* userdata){
    if(text(event).find("\"event\"")!=std::string::npos)++static_cast<Host*>(userdata)->traces;
}
static dage_status_t state_put(dage_string_view_t id,dage_string_view_t checkpoint,void* userdata){
    Host& host=*static_cast<Host*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    auto& item=host.states[text(id)];++item.first;item.second=text(checkpoint);return DAGE_STATUS_OK;
}
static dage_status_t state_get(dage_string_view_t id,dage_owned_buffer_t* output,void* userdata){
    Host& host=*static_cast<Host*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    auto found=host.states.find(text(id));if(found==host.states.end())return DAGE_STATUS_NOT_FOUND;
    *output=owned(found->second.second);return output->data||!output->size?DAGE_STATUS_OK:DAGE_STATUS_OUT_OF_MEMORY;
}
static dage_status_t state_erase(dage_string_view_t id,void* userdata){
    Host& host=*static_cast<Host*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    host.states.erase(text(id));return DAGE_STATUS_OK;
}
static dage_status_t state_load(dage_string_view_t id,dage_state_record_t* output,void* userdata){
    Host& host=*static_cast<Host*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    auto found=host.states.find(text(id));if(found==host.states.end())return DAGE_STATUS_NOT_FOUND;
    output->version=found->second.first;output->checkpoint=owned(found->second.second);
    return output->checkpoint.data||!output->checkpoint.size?DAGE_STATUS_OK:DAGE_STATUS_OUT_OF_MEMORY;
}
static dage_status_t state_cas(dage_string_view_t id,uint64_t expected,dage_string_view_t checkpoint,
                               uint64_t* version,void* userdata){
    Host& host=*static_cast<Host*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    auto& item=host.states[text(id)];if(item.first!=expected)return DAGE_STATUS_EXECUTION_ERROR;
    item.first=expected+1;item.second=text(checkpoint);*version=item.first;return DAGE_STATUS_OK;
}
static dage_status_t state_claim(dage_string_view_t id,uint64_t expected,dage_string_view_t owner,
                                 dage_state_record_t* output,void* userdata){
    Host& host=*static_cast<Host*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    auto found=host.states.find(text(id));if(found==host.states.end())return DAGE_STATUS_NOT_FOUND;
    if(found->second.first!=expected)return DAGE_STATUS_EXECUTION_ERROR;
    output->version=++found->second.first;output->epoch=1;output->owner=owned(text(owner));
    output->checkpoint=owned(found->second.second);return DAGE_STATUS_OK;
}
static dage_status_t state_list(dage_string_view_t,dage_owned_buffer_t* output,void*){
    *output=owned("[]");return output->data?DAGE_STATUS_OK:DAGE_STATUS_OUT_OF_MEMORY;
}
static dage_status_t renew(uint64_t ttl,uint64_t* expires,void*){*expires=ttl+1;return DAGE_STATUS_OK;}
static void release_lease(void* userdata){++static_cast<Host*>(userdata)->releases;}
static dage_status_t acquire(dage_string_view_t request,dage_cancelled_callback_t cancelled,
                             void* cancellation,dage_resource_lease_t* lease,void* userdata){
    Host& host=*static_cast<Host*>(userdata);if(cancelled(cancellation))return DAGE_STATUS_CANCELLED;
    if(text(request).find("\"resources\"")==std::string::npos)return DAGE_STATUS_INVALID_ARGUMENT;
    ++host.leases;lease->lease_id=owned("host-lease");lease->fencing_token=7;lease->expires_at_unix_ms=9;
    lease->renew=&renew;lease->release=&release_lease;lease->lease_userdata=&host;return DAGE_STATUS_OK;
}
static dage_status_t executor(const dage_execution_context_t*,dage_string_view_t,
                              dage_owned_buffer_t* output,void*){
    static const char json[]="{}";output->data=json;output->size=2;return DAGE_STATUS_OK;
}

int main(){
    Host host;dage_engine_handle engine=nullptr;
    if(dage_engine_create(nullptr,&engine)!=DAGE_STATUS_OK)return 1;
    dage_scheduler_vtable_t scheduler{};scheduler.struct_size=sizeof(scheduler);scheduler.submit=&schedule;
    dage_trace_sink_vtable_t sink{};sink.struct_size=sizeof(sink);sink.emit=&trace;
    dage_state_store_vtable_t store{};store.struct_size=sizeof(store);store.put=&state_put;store.get=&state_get;
    store.erase=&state_erase;store.load=&state_load;store.compare_exchange=&state_cas;
    store.claim=&state_claim;store.list=&state_list;
    dage_resource_lease_provider_vtable_t leases{};leases.struct_size=sizeof(leases);leases.acquire=&acquire;
    if(dage_engine_set_scheduler(engine,&scheduler,&host,nullptr)||
       dage_engine_set_trace_sink(engine,&sink,&host,nullptr)||
       dage_engine_set_state_store(engine,&store,&host,nullptr)||
       dage_engine_set_resource_lease_provider(engine,&leases,&host,nullptr))return 2;
    const char name[]="echo";if(dage_engine_register_executor(engine,{name,4},&executor,nullptr,nullptr))return 3;
    const std::string workflow=
        "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{"
        "\"a\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\"}}}";
    dage_workflow_handle graph=nullptr;if(dage_engine_load(engine,{workflow.data(),workflow.size()},&graph))return 4;
    dage_run_handle run=nullptr;if(dage_run_create(engine,graph,&run))return 5;
    size_t required=0;const char input[]="{}";
    if(dage_run_execute(run,{input,2},nullptr,0,&required)!=DAGE_STATUS_BUFFER_TOO_SMALL)return 6;
    std::string output(required,'\0');
    if(dage_run_execute(run,{input,2},&output[0],output.size(),&required)!=DAGE_STATUS_OK)return 7;
    dage_run_destroy(run);dage_workflow_destroy(graph);dage_engine_destroy(engine);
    if(host.scheduled<1||host.traces<1||host.leases!=1||host.releases!=1||host.states.empty())return 8;
    return 0;
}
