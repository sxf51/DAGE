#ifndef DAGE_ABI_V1_FROZEN_H
#define DAGE_ABI_V1_FROZEN_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(DAGE_EXPORTS)
#define DAGE_API __declspec(dllexport)
#elif defined(_WIN32)
#define DAGE_API __declspec(dllimport)
#else
#define DAGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DAGE_C_ABI_VERSION 1u

typedef struct dage_engine_t* dage_engine_handle;
typedef struct dage_workflow_t* dage_workflow_handle;
typedef struct dage_run_t* dage_run_handle;

typedef enum dage_status_t {
    DAGE_STATUS_OK = 0,
    DAGE_STATUS_INVALID_ARGUMENT = 1,
    DAGE_STATUS_OUT_OF_MEMORY = 2,
    DAGE_STATUS_PARSE_ERROR = 3,
    DAGE_STATUS_VALIDATION_ERROR = 4,
    DAGE_STATUS_EXECUTION_ERROR = 5,
    DAGE_STATUS_CANCELLED = 6,
    DAGE_STATUS_NOT_FOUND = 7,
    DAGE_STATUS_BUFFER_TOO_SMALL = 8,
    DAGE_STATUS_SUSPENDED = 9,
    DAGE_STATUS_INTERNAL_ERROR = 100,
    DAGE_STATUS_UNKNOWN_ERROR = 101
} dage_status_t;

typedef struct dage_string_view_t {
    const char* data;
    size_t size;
} dage_string_view_t;

typedef struct dage_allocator_t dage_allocator_t;
typedef struct dage_engine_options_t {
    uint32_t struct_size;
    uint32_t api_version;
    const dage_allocator_t* output_allocator;
    void* reserved[8];
} dage_engine_options_t;

DAGE_API uint8_t dage_runtime_has_capability(dage_string_view_t capability);
DAGE_API dage_status_t dage_runtime_registry(char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_engine_create(const dage_engine_options_t* options, dage_engine_handle* out);
DAGE_API void dage_engine_destroy(dage_engine_handle engine);
DAGE_API dage_status_t dage_engine_load(
    dage_engine_handle engine, dage_string_view_t json, dage_workflow_handle* out);
DAGE_API void dage_workflow_destroy(dage_workflow_handle workflow);
DAGE_API dage_status_t dage_run_create(
    dage_engine_handle engine, dage_workflow_handle workflow, dage_run_handle* out);
DAGE_API void dage_run_destroy(dage_run_handle run);
DAGE_API dage_status_t dage_run_execute(
    dage_run_handle run, dage_string_view_t input_json,
    char* output, size_t output_size, size_t* required_size);

#ifdef __cplusplus
}
#endif
#endif

