#ifndef DAGE_DAGE_H
#define DAGE_DAGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(DAGE_STATIC)
#define DAGE_API
#elif defined(_WIN32) && defined(DAGE_EXPORTS)
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
#define DAGE_RUNTIME_VERSION_STRING "0.2.0"

typedef struct dage_engine_t* dage_engine_handle;
typedef struct dage_workflow_t* dage_workflow_handle;
typedef struct dage_run_t* dage_run_handle;
typedef struct dage_bundle_t* dage_bundle_handle;
typedef struct dage_resolved_bundle_graph_t* dage_resolved_bundle_graph_handle;
typedef struct dage_trace_replay_plan_t* dage_trace_replay_plan_handle;
typedef struct dage_scheduler_task_t* dage_scheduler_task_handle;

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
    DAGE_STATUS_RESOURCE_EXHAUSTED = 10,
    DAGE_STATUS_INTERNAL_ERROR = 100,
    DAGE_STATUS_UNKNOWN_ERROR = 101
} dage_status_t;

typedef struct dage_string_view_t {
    const char* data; /* borrowed */
    size_t size;
} dage_string_view_t;

typedef struct dage_resource_entry_t {
    dage_string_view_t path;
    dage_string_view_t content;
} dage_resource_entry_t;

typedef void* (*dage_allocate_t)(size_t size, size_t alignment, void* userdata);
typedef void (*dage_deallocate_t)(void* pointer, size_t size, size_t alignment, void* userdata);
typedef struct dage_allocator_t {
    uint32_t struct_size;
    dage_allocate_t allocate;
    dage_deallocate_t deallocate;
    void* userdata;
    void* reserved[4];
} dage_allocator_t;

typedef struct dage_engine_options_t {
    uint32_t struct_size;
    uint32_t api_version;
    const dage_allocator_t* output_allocator; /* copied during create */
    uint64_t max_workflow_bytes;
    uint32_t max_json_depth;
    uint32_t max_nodes;
    uint64_t max_edges;
    uint64_t max_expression_bytes;
    uint64_t max_literal_bytes;
    uint64_t max_compiled_ir_bytes;
    void* reserved[2];
} dage_engine_options_t;

typedef enum dage_run_mode_t {
    DAGE_RUN_MODE_NORMAL = 0,
    DAGE_RUN_MODE_REPLAY = 1,
    DAGE_RUN_MODE_SHADOW = 2
} dage_run_mode_t;
typedef enum dage_trace_capture_t {
    DAGE_TRACE_CAPTURE_METADATA = 0,
    DAGE_TRACE_CAPTURE_OFF = 1,
    DAGE_TRACE_CAPTURE_INPUTS = 2,
    DAGE_TRACE_CAPTURE_FULL = 3
} dage_trace_capture_t;

typedef struct dage_run_options_t {
    uint32_t struct_size;
    dage_run_mode_t mode;
    uint8_t allow_external_writes;
    uint8_t allow_irreversible;
    uint8_t trace_capture;
    uint8_t reserved_bytes[5];
    uint64_t deadline_ms; /* 0 means no additional run deadline */
    uint32_t retry_budget;
    uint32_t reserved_u32;
    uint64_t max_output_bytes;
    uint64_t max_state_bytes;
    uint64_t max_events;
    uint32_t max_in_flight_tasks;
    uint32_t reserved_quota;
    void* reserved[4];
} dage_run_options_t;

typedef void (*dage_effect_commit_callback_t)(void* userdata);
typedef uint8_t (*dage_cancelled_callback_t)(void* userdata);

typedef struct dage_execution_context_t {
    uint32_t struct_size;
    dage_string_view_t run_id;
    dage_string_view_t node_id;
    dage_string_view_t node_type;
    uint32_t attempt;
    dage_string_view_t idempotency_key;
    dage_string_view_t run_mode;
    uint64_t deadline_remaining_ms; /* 0 means none or already expired */
    dage_effect_commit_callback_t commit_effect; /* NULL for non-effect nodes */
    void* commit_effect_userdata; /* borrowed for the callback duration */
    dage_cancelled_callback_t is_cancelled;
    void* cancellation_userdata; /* borrowed for the callback duration */
} dage_execution_context_t;

typedef void (*dage_buffer_release_t)(const char* data, size_t size, void* userdata);
typedef struct dage_owned_buffer_t {
    const char* data;
    size_t size;
    dage_buffer_release_t release;
    void* release_userdata;
} dage_owned_buffer_t;

typedef dage_status_t (*dage_executor_callback_t)(
    const dage_execution_context_t* context,
    dage_string_view_t input_json,
    dage_owned_buffer_t* output_json,
    void* userdata);
typedef struct dage_executor_completion_t* dage_executor_completion_handle;
typedef dage_status_t (*dage_async_executor_callback_t)(
    const dage_execution_context_t* context,
    dage_string_view_t input_json,
    dage_executor_completion_handle completion,
    void* userdata);
typedef void (*dage_userdata_destroy_t)(void* userdata);

typedef dage_status_t (*dage_scheduler_submit_t)(
    dage_scheduler_task_handle task, uint8_t continuation, void* userdata);
typedef struct dage_scheduler_vtable_t {
    uint32_t struct_size;
    dage_scheduler_submit_t submit;
    void* reserved[4];
} dage_scheduler_vtable_t;

typedef struct dage_state_record_t {
    uint32_t struct_size;
    uint64_t version;
    uint64_t epoch;
    dage_owned_buffer_t owner;
    dage_owned_buffer_t checkpoint;
    void* reserved[4];
} dage_state_record_t;
typedef struct dage_state_store_vtable_t {
    uint32_t struct_size;
    dage_status_t (*put)(dage_string_view_t run_id, dage_string_view_t checkpoint, void* userdata);
    dage_status_t (*get)(dage_string_view_t run_id, dage_owned_buffer_t* checkpoint, void* userdata);
    dage_status_t (*erase)(dage_string_view_t run_id, void* userdata);
    dage_status_t (*load)(dage_string_view_t run_id, dage_state_record_t* record, void* userdata);
    dage_status_t (*compare_exchange)(dage_string_view_t run_id, uint64_t expected_version,
                                      dage_string_view_t checkpoint, uint64_t* new_version,
                                      void* userdata);
    dage_status_t (*claim)(dage_string_view_t run_id, uint64_t expected_version,
                           dage_string_view_t owner, dage_state_record_t* record, void* userdata);
    /* Returns a UTF-8 JSON array of run-id strings. */
    dage_status_t (*list)(dage_string_view_t prefix, dage_owned_buffer_t* run_ids_json, void* userdata);
    void* reserved[4];
} dage_state_store_vtable_t;

typedef struct dage_trace_sink_vtable_t {
    uint32_t struct_size;
    /* event_json is borrowed and valid only during emit. emit must not throw across the ABI. */
    void (*emit)(dage_string_view_t event_json, void* userdata);
    void* reserved[4];
} dage_trace_sink_vtable_t;

typedef struct dage_resource_lease_t {
    uint32_t struct_size;
    dage_owned_buffer_t lease_id;
    uint64_t fencing_token;
    uint64_t expires_at_unix_ms;
    dage_status_t (*renew)(uint64_t ttl_ms, uint64_t* expires_at_unix_ms, void* lease_userdata);
    void (*release)(void* lease_userdata);
    void* lease_userdata;
    void* reserved[4];
} dage_resource_lease_t;
typedef struct dage_resource_lease_provider_vtable_t {
    uint32_t struct_size;
    /* request_json is borrowed. The successful lease transfers its fields to DAGE. */
    dage_status_t (*acquire)(dage_string_view_t request_json,
                             dage_cancelled_callback_t is_cancelled,
                             void* cancellation_userdata,
                             dage_resource_lease_t* lease,
                             void* userdata);
    void* reserved[4];
} dage_resource_lease_provider_vtable_t;

typedef struct dage_resource_provider_t {
    uint32_t struct_size;
    dage_status_t (*list_resources)(dage_owned_buffer_t* paths_json, void* userdata);
    dage_status_t (*read_resource)(dage_string_view_t path,
                                   dage_owned_buffer_t* bytes, void* userdata);
    void (*release)(void* userdata);
    void* userdata;
    void* reserved[4];
} dage_resource_provider_t;

typedef struct dage_bundle_repository_vtable_t {
    uint32_t struct_size;
    dage_status_t (*available_versions)(dage_string_view_t bundle_id, uint8_t offline,
                                        dage_owned_buffer_t* versions_json, void* userdata);
    dage_status_t (*get)(dage_string_view_t bundle_id, dage_string_view_t version,
                         uint8_t offline, dage_resource_provider_t* provider, void* userdata);
    dage_status_t (*source)(dage_owned_buffer_t* source, void* userdata);
    void* reserved[4];
} dage_bundle_repository_vtable_t;

typedef struct dage_key_provider_vtable_t {
    uint32_t struct_size;
    /* context_json contains Bundle/signature identity. public_key must contain 32 raw bytes. */
    dage_status_t (*find_key)(dage_string_view_t key_id, dage_string_view_t context_json,
                              dage_owned_buffer_t* public_key, void* userdata);
    void* reserved[4];
} dage_key_provider_vtable_t;

typedef struct dage_trust_policy_vtable_t {
    uint32_t struct_size;
    dage_status_t (*trust)(dage_string_view_t context_json, uint8_t* trusted, void* userdata);
    void* reserved[4];
} dage_trust_policy_vtable_t;

typedef enum dage_bundle_load_mode_t {
    DAGE_BUNDLE_LOAD_DEVELOPMENT = 0,
    DAGE_BUNDLE_LOAD_FROZEN = 1,
    DAGE_BUNDLE_LOAD_VERIFIED = 2,
    DAGE_BUNDLE_LOAD_OFFLINE = 3
} dage_bundle_load_mode_t;
typedef enum dage_signature_policy_kind_t {
    DAGE_SIGNATURE_DISABLED = 0,
    DAGE_SIGNATURE_AT_LEAST_ONE = 1,
    DAGE_SIGNATURE_THRESHOLD = 2,
    DAGE_SIGNATURE_ALL = 3
} dage_signature_policy_kind_t;
typedef struct dage_bundle_resolver_options_t {
    uint32_t struct_size;
    dage_bundle_load_mode_t mode;
    dage_signature_policy_kind_t signature_policy;
    uint32_t signature_threshold;
    dage_string_view_t lock_json;
    void* reserved[8];
} dage_bundle_resolver_options_t;

typedef dage_status_t (*dage_metric_evaluate_t)(
    dage_string_view_t sample_json, double* value, void* userdata);
typedef struct dage_metric_definition_t {
    uint32_t struct_size;
    dage_string_view_t name;
    double weight;
    double regression_tolerance;
    dage_metric_evaluate_t evaluate;
    void* userdata;
    void* reserved[4];
} dage_metric_definition_t;

DAGE_API dage_status_t dage_engine_create(const dage_engine_options_t* options,
                                           dage_engine_handle* out); /* out: transferred */
DAGE_API uint8_t dage_runtime_has_capability(dage_string_view_t capability);
DAGE_API dage_status_t dage_runtime_registry(
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API void dage_engine_destroy(dage_engine_handle engine);
DAGE_API dage_status_t dage_engine_set_scheduler(
    dage_engine_handle engine, const dage_scheduler_vtable_t* vtable,
    void* userdata, dage_userdata_destroy_t destroy);
DAGE_API dage_status_t dage_scheduler_task_run(dage_scheduler_task_handle task);
DAGE_API void dage_scheduler_task_abandon(dage_scheduler_task_handle task);
DAGE_API dage_status_t dage_engine_set_state_store(
    dage_engine_handle engine, const dage_state_store_vtable_t* vtable,
    void* userdata, dage_userdata_destroy_t destroy);
DAGE_API dage_status_t dage_engine_set_trace_sink(
    dage_engine_handle engine, const dage_trace_sink_vtable_t* vtable,
    void* userdata, dage_userdata_destroy_t destroy);
DAGE_API dage_status_t dage_engine_set_resource_lease_provider(
    dage_engine_handle engine, const dage_resource_lease_provider_vtable_t* vtable,
    void* userdata, dage_userdata_destroy_t destroy);
DAGE_API dage_status_t dage_engine_register_executor(dage_engine_handle engine,
                                                      dage_string_view_t name,
                                                      dage_executor_callback_t callback,
                                                      void* userdata,
                                                      dage_userdata_destroy_t destroy);
DAGE_API dage_status_t dage_engine_register_async_executor(
    dage_engine_handle engine, dage_string_view_t name,
    dage_async_executor_callback_t callback, void* userdata,
    dage_userdata_destroy_t destroy);
DAGE_API dage_status_t dage_executor_complete(dage_executor_completion_handle completion,
                                               dage_status_t status,
                                               const dage_owned_buffer_t* output_json);
DAGE_API void dage_executor_abandon(dage_executor_completion_handle completion);
DAGE_API uint8_t dage_executor_completion_is_cancelled(
    dage_executor_completion_handle completion);
DAGE_API dage_status_t dage_executor_completion_commit_effect(
    dage_executor_completion_handle completion);
DAGE_API dage_status_t dage_engine_register_workflow(dage_engine_handle engine,
                                                      dage_string_view_t name,
                                                      dage_string_view_t json);
DAGE_API dage_status_t dage_engine_load(dage_engine_handle engine,
                                        dage_string_view_t json,
                                        dage_workflow_handle* out); /* out: transferred */
DAGE_API dage_status_t dage_bundle_load(dage_engine_handle engine,
                                        const dage_resource_entry_t* entries,
                                        size_t entry_count,
                                        dage_bundle_handle* out); /* out: transferred */
DAGE_API void dage_bundle_destroy(dage_bundle_handle bundle);
DAGE_API dage_status_t dage_bundle_resolve(
    dage_engine_handle engine,
    const dage_resource_provider_t* root,
    const dage_bundle_resolver_options_t* options,
    const dage_bundle_repository_vtable_t* repository,
    void* repository_userdata,
    const dage_key_provider_vtable_t* keys,
    void* keys_userdata,
    const dage_trust_policy_vtable_t* trust,
    void* trust_userdata,
    dage_resolved_bundle_graph_handle* out);
DAGE_API void dage_resolved_bundle_graph_destroy(dage_resolved_bundle_graph_handle graph);
DAGE_API dage_status_t dage_resolved_bundle_graph_lock(
    dage_resolved_bundle_graph_handle graph,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_resolved_bundle_graph_root_id(
    dage_resolved_bundle_graph_handle graph,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_resolved_bundle_graph_load_workflow(
    dage_engine_handle engine,
    dage_resolved_bundle_graph_handle graph,
    dage_string_view_t bundle_id,
    dage_string_view_t workflow_id,
    dage_workflow_handle* out);
DAGE_API dage_status_t dage_bundle_load_workflow(dage_engine_handle engine,
                                                 dage_bundle_handle bundle,
                                                 dage_string_view_t workflow_id,
                                                 dage_workflow_handle* out); /* out: transferred */
DAGE_API void dage_workflow_destroy(dage_workflow_handle workflow);
DAGE_API dage_status_t dage_workflow_apply_patch(dage_engine_handle engine,
                                                 dage_workflow_handle workflow,
                                                 dage_string_view_t patch_json,
                                                 dage_workflow_handle* out);
DAGE_API dage_status_t dage_workflow_dry_run_patch(
    dage_engine_handle engine, dage_workflow_handle workflow,
    dage_string_view_t patch_json,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_workflow_analyze_patch(
    dage_engine_handle engine, dage_workflow_handle workflow,
    dage_string_view_t patch_json,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_workflow_diff(
    dage_engine_handle engine,
    dage_workflow_handle before, dage_workflow_handle after,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_workflow_format(dage_workflow_handle workflow,
                                            char* buffer, size_t buffer_size,
                                            size_t* required_size);
DAGE_API dage_status_t dage_workflow_export_mermaid(dage_workflow_handle workflow,
                                                     char* buffer, size_t buffer_size,
                                                     size_t* required_size);
DAGE_API dage_status_t dage_workflow_export_dot(dage_workflow_handle workflow,
                                                 char* buffer, size_t buffer_size,
                                                 size_t* required_size);
DAGE_API dage_status_t dage_run_create(dage_engine_handle engine,
                                       dage_workflow_handle workflow,
                                       dage_run_handle* out); /* out: transferred */
DAGE_API dage_status_t dage_run_create_with_options(dage_engine_handle engine,
                                                    dage_workflow_handle workflow,
                                                    const dage_run_options_t* options,
                                                    dage_run_handle* out);
DAGE_API void dage_run_destroy(dage_run_handle run);
DAGE_API dage_status_t dage_run_execute(dage_run_handle run,
                                        dage_string_view_t input_json,
                                        char* output, size_t output_size,
                                        size_t* required_size);
DAGE_API dage_status_t dage_run_resume(dage_run_handle run,
                                       dage_string_view_t human_output_json,
                                       char* output, size_t output_size,
                                       size_t* required_size);
DAGE_API dage_status_t dage_run_selective_rerun(
    dage_run_handle run, dage_string_view_t node_id,
    char* output, size_t output_size, size_t* required_size);
DAGE_API dage_status_t dage_trace_replay_prepare(
    dage_engine_handle engine, dage_string_view_t events_json,
    dage_string_view_t expected_workflow_digest,
    dage_string_view_t expected_bundle_digest,
    dage_trace_replay_plan_handle* out);
DAGE_API void dage_trace_replay_plan_destroy(dage_trace_replay_plan_handle plan);
DAGE_API dage_status_t dage_trace_replay_plan_input(
    dage_trace_replay_plan_handle plan,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_trace_replay_plan_describe(
    dage_trace_replay_plan_handle plan,
    char* buffer, size_t buffer_size, size_t* required_size);
DAGE_API dage_status_t dage_run_create_replay(
    dage_engine_handle engine, dage_workflow_handle workflow,
    dage_trace_replay_plan_handle plan, dage_run_handle* out);
DAGE_API dage_status_t dage_shadow_compare(
    dage_engine_handle engine,
    dage_string_view_t baseline_sample_json,
    dage_string_view_t candidate_sample_json,
    const dage_metric_definition_t* metrics, size_t metric_count,
    dage_owned_buffer_t* report_json);
DAGE_API dage_status_t dage_run_checkpoint(dage_run_handle run,
                                           char* buffer, size_t buffer_size,
                                           size_t* required_size);
DAGE_API dage_status_t dage_run_snapshot(dage_run_handle run,
                                         char* buffer, size_t buffer_size,
                                         size_t* required_size);
DAGE_API dage_status_t dage_run_restore(dage_engine_handle engine,
                                        dage_workflow_handle workflow,
                                        dage_string_view_t checkpoint_json,
                                        dage_run_handle* out); /* out: transferred */
DAGE_API void dage_run_cancel(dage_run_handle run);
DAGE_API dage_status_t dage_run_cancel_with_reason(dage_run_handle run,
                                                    dage_string_view_t reason);
DAGE_API dage_status_t dage_last_error(dage_engine_handle engine,
                                       char* buffer, size_t buffer_size,
                                       size_t* required_size);

#ifdef __cplusplus
}
#endif
#endif
