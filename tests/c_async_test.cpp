#include "dage/dage.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

static std::atomic<int> calls(0);
static std::atomic<dage_executor_completion_handle> pending(nullptr);

static dage_status_t immediate(const dage_execution_context_t*,dage_string_view_t,
                               dage_executor_completion_handle completion,void*){
    ++calls;
    std::thread([completion](){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        static const char json[]="{}";dage_owned_buffer_t output={json,2,nullptr,nullptr};
        dage_executor_complete(completion,DAGE_STATUS_OK,&output);
    }).detach();
    return DAGE_STATUS_OK;
}
static dage_status_t never_complete(const dage_execution_context_t*,dage_string_view_t,
                                    dage_executor_completion_handle completion,void*){
    ++calls;pending.store(completion);return DAGE_STATUS_OK;
}
int main(){
    dage_engine_handle engine=nullptr;if(dage_engine_create(nullptr,&engine))return 1;
    const char* name="async";dage_string_view_t name_view={name,5};
    if(dage_engine_register_async_executor(engine,name_view,immediate,nullptr,nullptr))return 2;
    const char* json="{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{\"a\":{\"type\":\"tool\",\"executor\":\"async\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"}}}}";
    dage_string_view_t view={json,std::strlen(json)};dage_workflow_handle workflow=nullptr;
    if(dage_engine_load(engine,view,&workflow))return 3;
    dage_run_handle run=nullptr;
    if(dage_run_create(engine,workflow,&run))return 4;
    view={"{}",2};size_t required=0;
    if(dage_run_execute(run,view,nullptr,0,&required)!=DAGE_STATUS_BUFFER_TOO_SMALL||calls.load()!=1)return 5;
    dage_run_destroy(run);dage_workflow_destroy(workflow);

    name="wait";name_view={name,4};
    if(dage_engine_register_async_executor(engine,name_view,never_complete,nullptr,nullptr))return 6;
    json="{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{\"a\":{\"type\":\"tool\",\"executor\":\"wait\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"}}}}";
    view={json,std::strlen(json)};if(dage_engine_load(engine,view,&workflow))return 7;
    if(dage_run_create(engine,workflow,&run))return 8;
    std::atomic<int> status(-1);
    std::thread runner([&](){dage_string_view_t input={"{}",2};size_t size=0;
        status=dage_run_execute(run,input,nullptr,0,&size);});
    while(!pending.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
    dage_string_view_t reason={"shutdown",8};dage_run_cancel_with_reason(run,reason);runner.join();
    if(status.load()!=DAGE_STATUS_CANCELLED||!dage_executor_completion_is_cancelled(pending.load()))return 9;
    static const char late[]="{}";dage_owned_buffer_t output={late,2,nullptr,nullptr};
    if(dage_executor_complete(pending.load(),DAGE_STATUS_OK,&output)!=DAGE_STATUS_OK)return 10;
    pending.store(nullptr);dage_run_destroy(run);dage_workflow_destroy(workflow);dage_engine_destroy(engine);
    return calls.load()==2?0:11;
}
