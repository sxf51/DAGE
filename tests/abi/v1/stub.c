#define DAGE_EXPORTS
#include "dage.h"

#include <stdlib.h>
#include <string.h>

struct dage_engine_t { int value; };
struct dage_workflow_t { int value; };
struct dage_run_t { int value; };

uint8_t dage_runtime_has_capability(dage_string_view_t capability) {
    (void)capability;
    return 1;
}
dage_status_t dage_runtime_registry(char* buffer, size_t size, size_t* required) {
    const char value[] = "{}";
    if (!required) return DAGE_STATUS_INVALID_ARGUMENT;
    *required = sizeof(value);
    if (!buffer || size < sizeof(value)) return DAGE_STATUS_BUFFER_TOO_SMALL;
    memcpy(buffer, value, sizeof(value));
    return DAGE_STATUS_OK;
}
dage_status_t dage_engine_create(const dage_engine_options_t* options, dage_engine_handle* out) {
    (void)options;
    if (!out) return DAGE_STATUS_INVALID_ARGUMENT;
    *out = (dage_engine_handle)malloc(sizeof(struct dage_engine_t));
    return *out ? DAGE_STATUS_OK : DAGE_STATUS_OUT_OF_MEMORY;
}
void dage_engine_destroy(dage_engine_handle engine) { free(engine); }
dage_status_t dage_engine_load(
    dage_engine_handle engine, dage_string_view_t json, dage_workflow_handle* out) {
    (void)json;
    if (!engine || !out) return DAGE_STATUS_INVALID_ARGUMENT;
    *out = (dage_workflow_handle)malloc(sizeof(struct dage_workflow_t));
    return *out ? DAGE_STATUS_OK : DAGE_STATUS_OUT_OF_MEMORY;
}
void dage_workflow_destroy(dage_workflow_handle workflow) { free(workflow); }
dage_status_t dage_run_create(
    dage_engine_handle engine, dage_workflow_handle workflow, dage_run_handle* out) {
    if (!engine || !workflow || !out) return DAGE_STATUS_INVALID_ARGUMENT;
    *out = (dage_run_handle)malloc(sizeof(struct dage_run_t));
    return *out ? DAGE_STATUS_OK : DAGE_STATUS_OUT_OF_MEMORY;
}
void dage_run_destroy(dage_run_handle run) { free(run); }
dage_status_t dage_run_execute(
    dage_run_handle run, dage_string_view_t input,
    char* output, size_t size, size_t* required) {
    const char value[] = "{\"abi\":1}";
    (void)input;
    if (!run || !required) return DAGE_STATUS_INVALID_ARGUMENT;
    *required = sizeof(value);
    if (!output || size < sizeof(value)) return DAGE_STATUS_BUFFER_TOO_SMALL;
    memcpy(output, value, sizeof(value));
    return DAGE_STATUS_OK;
}

