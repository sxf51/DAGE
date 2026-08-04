#include "dage/dage.h"
#include <stdlib.h>
#include <string.h>

static int destroyed = 0;
static int committed = 0;
static int executor_calls = 0;
static void destroy_userdata(void* userdata) { destroyed = *(int*)userdata; }
static void release_output(const char* data, size_t size, void* userdata) {
    (void)size; (void)userdata; free((void*)data);
}
static dage_status_t echo_executor(const dage_execution_context_t* context, dage_string_view_t input,
                                   dage_owned_buffer_t* output, void* userdata) {
    (void)context; (void)userdata;
    ++executor_calls;
    output->data = (const char*)malloc(input.size);
    if (!output->data) return DAGE_STATUS_OUT_OF_MEMORY;
    memcpy((void*)output->data, input.data, input.size);
    output->size = input.size; output->release = release_output;
    return DAGE_STATUS_OK;
}
static dage_status_t suspend_executor(const dage_execution_context_t* context, dage_string_view_t input,
                                      dage_owned_buffer_t* output, void* userdata) {
    (void)context; (void)input; (void)output; (void)userdata;
    return DAGE_STATUS_SUSPENDED;
}
static dage_status_t write_executor(const dage_execution_context_t* context, dage_string_view_t input,
                                    dage_owned_buffer_t* output, void* userdata) {
    (void)input; (void)userdata;
    if (context->deadline_remaining_ms == 0) return DAGE_STATUS_EXECUTION_ERROR;
    ++executor_calls;
    output->data = (const char*)malloc(2);
    if (!output->data) return DAGE_STATUS_OUT_OF_MEMORY;
    memcpy((void*)output->data, "{}", 2);
    output->size = 2; output->release = release_output;
    if (!context->commit_effect) return DAGE_STATUS_EXECUTION_ERROR;
    context->commit_effect(context->commit_effect_userdata);
    ++committed;
    return DAGE_STATUS_OK;
}
int main(void) {
    dage_engine_handle engine = 0;
    int marker = 7;
    dage_engine_options_t options;
    memset(&options, 0, sizeof(options));
    options.struct_size = (uint32_t)sizeof(options);
    options.api_version = 1;
    if (dage_engine_create(&options, &engine) != DAGE_STATUS_OK) return 1;
    {
        const char* name = "echo";
        const char* json = "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{\"a\":{\"type\":\"tool\",\"executor\":\"echo\",\"effects\":{\"kind\":\"pure\",\"replay\":\"safe\"},\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"ok\":true}}}}";
        dage_string_view_t name_view = {name, strlen(name)};
        dage_string_view_t json_view = {json, strlen(json)};
        dage_workflow_handle workflow = 0;
        dage_run_handle run = 0;
        size_t required = 0;
        char* output;
        if (dage_engine_register_executor(engine, name_view, echo_executor, &marker, destroy_userdata) != DAGE_STATUS_OK) return 2;
        if (dage_engine_load(engine, json_view, &workflow) != DAGE_STATUS_OK) return 3;
        if (dage_workflow_export_mermaid(workflow, 0, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 4;
        output = (char*)malloc(required);
        if (!output || dage_workflow_export_mermaid(workflow, output, required, &required) != DAGE_STATUS_OK) return 5;
        free(output);
        if (dage_run_create(engine, workflow, &run) != DAGE_STATUS_OK) return 6;
        json_view.data = "{}"; json_view.size = 2;
        if (dage_run_execute(run, json_view, 0, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 7;
        output = (char*)malloc(required);
        if (!output || dage_run_execute(run, json_view, output, required, &required) != DAGE_STATUS_OK) return 8;
        if (executor_calls != 1) return 29;
        free(output);
        if (dage_run_snapshot(run, 0, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 22;
        output = (char*)malloc(required);
        if (!output || dage_run_snapshot(run, output, required, &required) != DAGE_STATUS_OK) return 23;
        free(output);
        dage_run_destroy(run);
        dage_workflow_destroy(workflow);
    }
    {
        const char* name = "approval";
        const char* json = "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{\"a\":{\"type\":\"human\",\"executor\":\"approval\",\"next\":\"z\"},\"z\":{\"type\":\"end\",\"input\":{\"ok\":true}}}}";
        dage_string_view_t name_view = {name, strlen(name)};
        dage_string_view_t json_view = {json, strlen(json)};
        dage_workflow_handle workflow = 0;
        dage_run_handle run = 0, restored = 0;
        size_t required = 0;
        char* checkpoint;
        char* output;
        if (dage_engine_register_executor(engine, name_view, suspend_executor, 0, 0) != DAGE_STATUS_OK) return 10;
        if (dage_engine_load(engine, json_view, &workflow) != DAGE_STATUS_OK) return 11;
        if (dage_run_create(engine, workflow, &run) != DAGE_STATUS_OK) return 12;
        json_view.data = "{}"; json_view.size = 2;
        if (dage_run_execute(run, json_view, 0, 0, &required) != DAGE_STATUS_SUSPENDED) return 13;
        if (dage_run_checkpoint(run, 0, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 14;
        checkpoint = (char*)malloc(required);
        if (!checkpoint || dage_run_checkpoint(run, checkpoint, required, &required) != DAGE_STATUS_OK) return 15;
        json_view.data = checkpoint; json_view.size = strlen(checkpoint);
        if (dage_run_restore(engine, workflow, json_view, &restored) != DAGE_STATUS_OK) return 16;
        json_view.data = "{\"approved\":true}"; json_view.size = strlen(json_view.data);
        if (dage_run_resume(restored, json_view, 0, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 17;
        output = (char*)malloc(required);
        if (!output || dage_run_resume(restored, json_view, output, required, &required) != DAGE_STATUS_OK) return 18;
        free(output); free(checkpoint);
        dage_run_destroy(restored); dage_run_destroy(run); dage_workflow_destroy(workflow);
    }
    {
        const char* name = "write";
        const char* json = "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"a\",\"nodes\":{\"a\":{\"type\":\"tool\",\"executor\":\"write\",\"effects\":{\"kind\":\"external_write\",\"replay\":\"idempotent\",\"idempotency_key\":\"${run.id}:a\"},\"next\":\"z\"},\"z\":{\"type\":\"end\"}}}";
        dage_string_view_t name_view = {name, strlen(name)};
        dage_string_view_t json_view = {json, strlen(json)};
        dage_workflow_handle workflow = 0;
        dage_run_handle run = 0;
        dage_run_options_t run_options;
        size_t required = 0;
        memset(&run_options, 0, sizeof(run_options));
        run_options.struct_size = (uint32_t)sizeof(run_options);
        run_options.allow_external_writes = 1;
        run_options.deadline_ms = 1000;
        run_options.retry_budget = 3;
        if (dage_engine_register_executor(engine, name_view, write_executor, 0, 0) != DAGE_STATUS_OK) return 24;
        if (dage_engine_load(engine, json_view, &workflow) != DAGE_STATUS_OK) return 25;
        if (dage_run_create_with_options(engine, workflow, &run_options, &run) != DAGE_STATUS_OK) return 26;
        json_view.data = "{}"; json_view.size = 2;
        if (dage_run_execute(run, json_view, 0, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 27;
        if (committed != 1 || executor_calls != 2) return 28;
        dage_run_destroy(run); dage_workflow_destroy(workflow);
    }
    {
        const char* manifest="{\"format\":\"dage-bundle\",\"format_version\":\"0.2.0\",\"id\":\"org.example.c\",\"version\":\"1.0.0\",\"workflows\":{\"main\":\"workflows/main.json\"}}";
        const char* workflow_json="{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"z\",\"nodes\":{\"z\":{\"type\":\"end\"}}}";
        dage_resource_entry_t entries[2];
        dage_bundle_handle bundle=0;dage_workflow_handle workflow=0;
        entries[0].path.data="manifest.json";entries[0].path.size=strlen(entries[0].path.data);
        entries[0].content.data=manifest;entries[0].content.size=strlen(manifest);
        entries[1].path.data="workflows/main.json";entries[1].path.size=strlen(entries[1].path.data);
        entries[1].content.data=workflow_json;entries[1].content.size=strlen(workflow_json);
        if(dage_bundle_load(engine,entries,2,&bundle)!=DAGE_STATUS_OK)return 20;
        { dage_string_view_t id={"main",4};if(dage_bundle_load_workflow(engine,bundle,id,&workflow)!=DAGE_STATUS_OK)return 21; }
        dage_workflow_destroy(workflow);dage_bundle_destroy(bundle);
    }
    {
        const char* oversized="{\"format\":\"dage-workflow\"}";
        dage_engine_options_t tight;dage_engine_handle limited=0;dage_workflow_handle rejected=0;
        dage_string_view_t input={oversized,strlen(oversized)};
        memset(&tight,0,sizeof(tight));tight.struct_size=(uint32_t)sizeof(tight);
        tight.api_version=DAGE_C_ABI_VERSION;tight.max_workflow_bytes=1;
        if(dage_engine_create(&tight,&limited)!=DAGE_STATUS_OK)return 40;
        if(dage_engine_load(limited,input,&rejected)!=DAGE_STATUS_RESOURCE_EXHAUSTED)return 41;
        if(rejected!=0)return 42;
        dage_engine_destroy(limited);
    }
    {
        const char* workflow_json="{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\",\"entry\":\"z\",\"nodes\":{\"z\":{\"type\":\"end\"}}}";
        dage_engine_options_t tight;dage_engine_handle limited=0;dage_workflow_handle rejected=0;
        dage_string_view_t input={workflow_json,strlen(workflow_json)};
        memset(&tight,0,sizeof(tight));tight.struct_size=(uint32_t)sizeof(tight);
        tight.api_version=DAGE_C_ABI_VERSION;tight.max_compiled_ir_bytes=128;
        if(dage_engine_create(&tight,&limited)!=DAGE_STATUS_OK)return 43;
        if(dage_engine_load(limited,input,&rejected)!=DAGE_STATUS_RESOURCE_EXHAUSTED)return 44;
        if(rejected!=0)return 45;
        dage_engine_destroy(limited);
    }
    dage_engine_destroy(engine);
    return destroyed == 7 ? 0 : 9;
}
