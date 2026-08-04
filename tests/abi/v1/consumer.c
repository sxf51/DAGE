#include "dage.h"

#include <stdlib.h>
#include <string.h>

int main(void) {
    const char capability[] = "core.dag.v1";
    const char workflow_json[] =
        "{\"format\":\"dage-workflow\",\"format_version\":\"0.2.0\","
        "\"entry\":\"z\",\"nodes\":{\"z\":{\"type\":\"end\",\"input\":{\"abi\":1}}}}";
    const char input[] = "{}";
    dage_engine_options_t options;
    dage_engine_handle engine = NULL;
    dage_workflow_handle workflow = NULL;
    dage_run_handle run = NULL;
    size_t required = 0;
    char* output = NULL;

    memset(&options, 0, sizeof(options));
    options.struct_size = (uint32_t)sizeof(options);
    options.api_version = DAGE_C_ABI_VERSION;
    if (!dage_runtime_has_capability((dage_string_view_t){capability, sizeof(capability) - 1})) return 1;
    if (dage_runtime_registry(NULL, 0, &required) != DAGE_STATUS_BUFFER_TOO_SMALL) return 2;
    if (dage_engine_create(&options, &engine) != DAGE_STATUS_OK) return 3;
    if (dage_engine_load(engine,
            (dage_string_view_t){workflow_json, sizeof(workflow_json) - 1}, &workflow) != DAGE_STATUS_OK) return 4;
    if (dage_run_create(engine, workflow, &run) != DAGE_STATUS_OK) return 5;
    if (dage_run_execute(run, (dage_string_view_t){input, 2}, NULL, 0, &required)
        != DAGE_STATUS_BUFFER_TOO_SMALL) return 6;
    output = (char*)malloc(required);
    if (!output || dage_run_execute(run, (dage_string_view_t){input, 2}, output, required, &required)
        != DAGE_STATUS_OK) return 7;
    if (strstr(output, "\"abi\":1") == NULL) return 8;
    free(output);
    dage_run_destroy(run);
    dage_workflow_destroy(workflow);
    dage_engine_destroy(engine);
    return 0;
}

