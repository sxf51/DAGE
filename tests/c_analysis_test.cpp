#include "dage/dage.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct TraceHost { std::mutex mutex; std::vector<std::string> events; int executor_calls=0; };
struct AllocatorHost { int allocations=0,deallocations=0; size_t alignment=0,allocated_size=0; };
static void* allocate_output(size_t size,size_t alignment,void* userdata){
    AllocatorHost& host=*static_cast<AllocatorHost*>(userdata);
    ++host.allocations;host.alignment=alignment;host.allocated_size=size;
    return ::operator new(size,std::align_val_t(alignment),std::nothrow);
}
static void deallocate_output(void* pointer,size_t,size_t alignment,void* userdata){
    ++static_cast<AllocatorHost*>(userdata)->deallocations;
    ::operator delete(pointer,std::align_val_t(alignment));
}
static std::string text(dage_string_view_t value){return std::string(value.data?value.data:"",value.size);}
static void trace(dage_string_view_t event,void* userdata){
    TraceHost& host=*static_cast<TraceHost*>(userdata);std::lock_guard<std::mutex> lock(host.mutex);
    host.events.push_back(text(event));
}
static dage_status_t executor(const dage_execution_context_t*,dage_string_view_t input,
                              dage_owned_buffer_t* output,void* userdata){
    ++static_cast<TraceHost*>(userdata)->executor_calls;
    char* copy=static_cast<char*>(std::malloc(input.size));if(!copy&&input.size)return DAGE_STATUS_OUT_OF_MEMORY;
    if(input.size)std::memcpy(copy,input.data,input.size);
    output->data=copy;output->size=input.size;
    output->release=[](const char* data,size_t,void*){std::free(const_cast<char*>(data));};
    return DAGE_STATUS_OK;
}
static std::string field(const std::string& json,const std::string& name){
    const std::string marker="\""+name+"\":\"";size_t start=json.find(marker);
    if(start==std::string::npos)return {};
    start+=marker.size();const size_t end=json.find('"',start);
    return end==std::string::npos?std::string():json.substr(start,end-start);
}
template<class First,class Second>
static std::string string_output(const First& first,const Second& second){
    size_t required=0;if(first(nullptr,0,&required)!=DAGE_STATUS_BUFFER_TOO_SMALL)return {};
    std::string result(required,'\0');if(second(&result[0],result.size(),&required))return {};
    result.resize(required-1);return result;
}
static std::string execute(dage_run_handle run,const std::string& input){
    size_t required=0;dage_string_view_t view={input.data(),input.size()};
    if(dage_run_execute(run,view,nullptr,0,&required)!=DAGE_STATUS_BUFFER_TOO_SMALL)return {};
    std::string result(required,'\0');if(dage_run_execute(run,view,&result[0],result.size(),&required))return {};
    result.resize(required-1);return result;
}
static dage_status_t metric(dage_string_view_t sample,double* value,void* userdata){
    // Each comparison invokes a metric exactly once for baseline and once for candidate.
    ++*static_cast<int*>(userdata);
    *value=text(sample).find("\"label\":\"candidate\"")!=std::string::npos?2.0:1.0;
    return DAGE_STATUS_OK;
}

int main(){
    const char capability[]="c.output_allocator.v1";
    if(!dage_runtime_has_capability({capability,sizeof(capability)-1})||
       dage_runtime_has_capability({"missing",7}))return 1;
    size_t registry_size=0;
    if(dage_runtime_registry(nullptr,0,&registry_size)!=DAGE_STATUS_BUFFER_TOO_SMALL)return 1;
    std::string registry(registry_size,'\0');
    if(dage_runtime_registry(&registry[0],registry.size(),&registry_size)||
       registry.find("\"callbacks\"")==std::string::npos||
       registry.find("\"statuses\"")==std::string::npos)return 1;
    AllocatorHost allocator_host;dage_allocator_t allocator{};allocator.struct_size=sizeof(allocator);
    allocator.allocate=&allocate_output;allocator.deallocate=&deallocate_output;allocator.userdata=&allocator_host;
    dage_engine_options_t engine_options{};engine_options.struct_size=sizeof(engine_options);
    engine_options.api_version=DAGE_C_ABI_VERSION;engine_options.output_allocator=&allocator;
    dage_engine_handle engine=nullptr;if(dage_engine_create(&engine_options,&engine))return 1;
    const std::string workflow_json=
        "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{"
        "\"a\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"z\"},"
        "\"z\":{\"type\":\"end\",\"input\":{\"version\":1}}}}";
    dage_workflow_handle workflow=nullptr;
    if(dage_engine_load(engine,{workflow_json.data(),workflow_json.size()},&workflow))return 2;

    const std::string same_diff=string_output(
        [&](char* out,size_t size,size_t* required){return dage_workflow_diff(engine,workflow,workflow,out,size,required);},
        [&](char* out,size_t size,size_t* required){return dage_workflow_diff(engine,workflow,workflow,out,size,required);});
    const std::string digest=field(same_diff,"before_digest");if(digest.empty())return 3;
    const std::string patch="{\"base_digest\":\""+digest+"\",\"changes\":["
        "{\"action\":\"set_field\",\"node_id\":\"z\",\"field\":\"input\",\"value\":{\"version\":2}}]}";
    const std::string dry=string_output(
        [&](char* out,size_t size,size_t* required){return dage_workflow_dry_run_patch(
            engine,workflow,{patch.data(),patch.size()},out,size,required);},
        [&](char* out,size_t size,size_t* required){return dage_workflow_dry_run_patch(
            engine,workflow,{patch.data(),patch.size()},out,size,required);});
    const std::string analysis=string_output(
        [&](char* out,size_t size,size_t* required){return dage_workflow_analyze_patch(
            engine,workflow,{patch.data(),patch.size()},out,size,required);},
        [&](char* out,size_t size,size_t* required){return dage_workflow_analyze_patch(
            engine,workflow,{patch.data(),patch.size()},out,size,required);});
    if(dry.find("\"valid\":true")==std::string::npos||
       analysis.find("\"affected_nodes\"")==std::string::npos)return 4;
    dage_workflow_handle changed=nullptr;
    if(dage_workflow_apply_patch(engine,workflow,{patch.data(),patch.size()},&changed))return 5;
    const std::string changed_diff=string_output(
        [&](char* out,size_t size,size_t* required){return dage_workflow_diff(engine,workflow,changed,out,size,required);},
        [&](char* out,size_t size,size_t* required){return dage_workflow_diff(engine,workflow,changed,out,size,required);});
    if(changed_diff.find("\"node_id\":\"z\"")==std::string::npos)return 6;

    TraceHost host;dage_trace_sink_vtable_t sink{};sink.struct_size=sizeof(sink);sink.emit=&trace;
    if(dage_engine_set_trace_sink(engine,&sink,&host,nullptr))return 7;
    const char echo[]="echo";if(dage_engine_register_executor(engine,{echo,4},&executor,&host,nullptr))return 8;
    dage_run_options_t options{};options.struct_size=sizeof(options);options.trace_capture=DAGE_TRACE_CAPTURE_FULL;
    dage_run_handle run=nullptr;if(dage_run_create_with_options(engine,workflow,&options,&run))return 9;
    if(execute(run,"{\"request\":1}").empty()||host.executor_calls!=1)return 10;
    const std::vector<std::string> source_events=host.events;
    const char node[]="a";size_t rerun_size=0;
    if(dage_run_selective_rerun(run,{node,1},nullptr,0,&rerun_size)!=DAGE_STATUS_BUFFER_TOO_SMALL)return 11;
    std::string rerun(rerun_size,'\0');
    if(dage_run_selective_rerun(run,{node,1},&rerun[0],rerun.size(),&rerun_size)||host.executor_calls!=2)return 12;
    dage_run_destroy(run);

    std::string events="[";for(size_t i=0;i<source_events.size();++i){
        if(i)events+=",";
        events+=source_events[i];}events+="]";
    const std::string trace_digest=field(source_events.front(),"workflow_digest");
    dage_trace_replay_plan_handle plan=nullptr;
    if(dage_trace_replay_prepare(engine,{events.data(),events.size()},
        {trace_digest.data(),trace_digest.size()},{nullptr,0},&plan))return 13;
    const std::string replay_input=string_output(
        [&](char* out,size_t size,size_t* required){return dage_trace_replay_plan_input(plan,out,size,required);},
        [&](char* out,size_t size,size_t* required){return dage_trace_replay_plan_input(plan,out,size,required);});
    if(replay_input!="{\"request\":1}")return 14;
    const std::string plan_description=string_output(
        [&](char* out,size_t size,size_t* required){return dage_trace_replay_plan_describe(plan,out,size,required);},
        [&](char* out,size_t size,size_t* required){return dage_trace_replay_plan_describe(plan,out,size,required);});
    if(plan_description.find("\"kind\":\"trace_replay_plan\"")==std::string::npos)return 15;
    dage_run_handle replay=nullptr;if(dage_run_create_replay(engine,workflow,plan,&replay))return 15;
    if(execute(replay,replay_input).empty()||host.executor_calls!=2)return 16;
    dage_run_destroy(replay);dage_trace_replay_plan_destroy(plan);

    const std::string baseline="{\"label\":\"baseline\",\"workflow_digest\":\"sha256:b\","
        "\"result\":{\"success\":true,\"output\":{\"quality\":1}},\"trace\":[]}";
    const std::string candidate="{\"label\":\"candidate\",\"workflow_digest\":\"sha256:c\","
        "\"result\":{\"success\":true,\"output\":{\"quality\":2}},"
        "\"trace\":[{\"event\":\"run_started\",\"payload\":{\"run_mode\":\"shadow\"}}]}";
    const char quality[]="quality";int metric_calls=0;dage_metric_definition_t definition{};definition.struct_size=sizeof(definition);
    definition.name={quality,7};definition.weight=1.0;definition.regression_tolerance=0.0;
    definition.evaluate=&metric;definition.userdata=&metric_calls;
    dage_owned_buffer_t report_buffer{};
    if(dage_shadow_compare(engine,{baseline.data(),baseline.size()},{candidate.data(),candidate.size()},
                           &definition,1,&report_buffer))return 17;
    const std::string report(report_buffer.data?report_buffer.data:"",report_buffer.size);
    if(report_buffer.release)report_buffer.release(
        report_buffer.data,report_buffer.size,report_buffer.release_userdata);
    if(report.find("\"recommended\":true")==std::string::npos||
       report.find("\"weighted_score_delta\":1.0")==std::string::npos||metric_calls!=2)return 17;
    dage_workflow_destroy(changed);dage_workflow_destroy(workflow);dage_engine_destroy(engine);
    if(allocator_host.allocations!=1||allocator_host.deallocations!=1||
       allocator_host.alignment<alignof(void*)||allocator_host.allocated_size<=report.size())return 18;
    return 0;
}
