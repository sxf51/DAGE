#include "dage/dage.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK_VALUE(label, actual, expected) do { \
    const size_t observed = (size_t)(actual); \
    if (observed != (size_t)(expected)) { \
        fprintf(stderr, "%s: expected %lu, observed %lu\n", (label), \
                (unsigned long)(expected), (unsigned long)observed); \
        return 1; \
    } \
} while (0)
#define CHECK_SIZE(type, expected) CHECK_VALUE(#type ".size", sizeof(type), expected)
#define CHECK_FIELD(type, field, expected) \
    CHECK_VALUE(#type "." #field, offsetof(type, field), expected)

int main(void) {
    const uint16_t endian_probe = 1;
    if (*(const uint8_t*)&endian_probe != 1) {
        fprintf(stderr, "the DAGE 1.0 Tier-1 ABI requires little-endian\n");
        return 2;
    }
    CHECK_VALUE("pointer.size", sizeof(void*), 8);
    CHECK_VALUE("size_t.size", sizeof(size_t), 8);
    CHECK_VALUE("enum.status.size", sizeof(dage_status_t), 4);
    CHECK_VALUE("enum.run_mode.size", sizeof(dage_run_mode_t), 4);
    CHECK_VALUE("enum.bundle_mode.size", sizeof(dage_bundle_load_mode_t), 4);

    CHECK_SIZE(dage_string_view_t, 16);
    CHECK_FIELD(dage_string_view_t, data, 0);
    CHECK_FIELD(dage_string_view_t, size, 8);
    CHECK_SIZE(dage_resource_entry_t, 32);
    CHECK_FIELD(dage_resource_entry_t, content, 16);
    CHECK_FIELD(dage_resource_entry_t, path, 0);

    CHECK_SIZE(dage_allocator_t, 64);
    CHECK_FIELD(dage_allocator_t, struct_size, 0);
    CHECK_FIELD(dage_allocator_t, allocate, 8);
    CHECK_FIELD(dage_allocator_t, deallocate, 16);
    CHECK_FIELD(dage_allocator_t, userdata, 24);
    CHECK_FIELD(dage_allocator_t, reserved, 32);
    CHECK_SIZE(dage_engine_options_t, 80);
    CHECK_FIELD(dage_engine_options_t, struct_size, 0);
    CHECK_FIELD(dage_engine_options_t, api_version, 4);
    CHECK_FIELD(dage_engine_options_t, output_allocator, 8);
    CHECK_FIELD(dage_engine_options_t, max_workflow_bytes, 16);
    CHECK_FIELD(dage_engine_options_t, max_json_depth, 24);
    CHECK_FIELD(dage_engine_options_t, max_nodes, 28);
    CHECK_FIELD(dage_engine_options_t, max_edges, 32);
    CHECK_FIELD(dage_engine_options_t, max_expression_bytes, 40);
    CHECK_FIELD(dage_engine_options_t, max_literal_bytes, 48);
    CHECK_FIELD(dage_engine_options_t, max_compiled_ir_bytes, 56);
    CHECK_FIELD(dage_engine_options_t, reserved, 64);

    CHECK_SIZE(dage_run_options_t, 96);
    CHECK_FIELD(dage_run_options_t, struct_size, 0);
    CHECK_FIELD(dage_run_options_t, mode, 4);
    CHECK_FIELD(dage_run_options_t, allow_external_writes, 8);
    CHECK_FIELD(dage_run_options_t, allow_irreversible, 9);
    CHECK_FIELD(dage_run_options_t, trace_capture, 10);
    CHECK_FIELD(dage_run_options_t, reserved_bytes, 11);
    CHECK_FIELD(dage_run_options_t, deadline_ms, 16);
    CHECK_FIELD(dage_run_options_t, retry_budget, 24);
    CHECK_FIELD(dage_run_options_t, reserved_u32, 28);
    CHECK_FIELD(dage_run_options_t, max_output_bytes, 32);
    CHECK_FIELD(dage_run_options_t, max_state_bytes, 40);
    CHECK_FIELD(dage_run_options_t, max_events, 48);
    CHECK_FIELD(dage_run_options_t, max_in_flight_tasks, 56);
    CHECK_FIELD(dage_run_options_t, reserved_quota, 60);
    CHECK_FIELD(dage_run_options_t, reserved, 64);

    CHECK_SIZE(dage_execution_context_t, 136);
    CHECK_FIELD(dage_execution_context_t, struct_size, 0);
    CHECK_FIELD(dage_execution_context_t, run_id, 8);
    CHECK_FIELD(dage_execution_context_t, node_id, 24);
    CHECK_FIELD(dage_execution_context_t, node_type, 40);
    CHECK_FIELD(dage_execution_context_t, attempt, 56);
    CHECK_FIELD(dage_execution_context_t, idempotency_key, 64);
    CHECK_FIELD(dage_execution_context_t, run_mode, 80);
    CHECK_FIELD(dage_execution_context_t, deadline_remaining_ms, 96);
    CHECK_FIELD(dage_execution_context_t, commit_effect, 104);
    CHECK_FIELD(dage_execution_context_t, commit_effect_userdata, 112);
    CHECK_FIELD(dage_execution_context_t, is_cancelled, 120);
    CHECK_FIELD(dage_execution_context_t, cancellation_userdata, 128);

    CHECK_SIZE(dage_owned_buffer_t, 32);
    CHECK_FIELD(dage_owned_buffer_t, data, 0);
    CHECK_FIELD(dage_owned_buffer_t, size, 8);
    CHECK_FIELD(dage_owned_buffer_t, release, 16);
    CHECK_FIELD(dage_owned_buffer_t, release_userdata, 24);
    CHECK_SIZE(dage_scheduler_vtable_t, 48);
    CHECK_FIELD(dage_scheduler_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_scheduler_vtable_t, submit, 8);
    CHECK_FIELD(dage_scheduler_vtable_t, reserved, 16);

    CHECK_SIZE(dage_state_record_t, 120);
    CHECK_FIELD(dage_state_record_t, struct_size, 0);
    CHECK_FIELD(dage_state_record_t, version, 8);
    CHECK_FIELD(dage_state_record_t, epoch, 16);
    CHECK_FIELD(dage_state_record_t, owner, 24);
    CHECK_FIELD(dage_state_record_t, checkpoint, 56);
    CHECK_FIELD(dage_state_record_t, reserved, 88);
    CHECK_SIZE(dage_state_store_vtable_t, 96);
    CHECK_FIELD(dage_state_store_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_state_store_vtable_t, put, 8);
    CHECK_FIELD(dage_state_store_vtable_t, get, 16);
    CHECK_FIELD(dage_state_store_vtable_t, erase, 24);
    CHECK_FIELD(dage_state_store_vtable_t, load, 32);
    CHECK_FIELD(dage_state_store_vtable_t, compare_exchange, 40);
    CHECK_FIELD(dage_state_store_vtable_t, claim, 48);
    CHECK_FIELD(dage_state_store_vtable_t, list, 56);
    CHECK_FIELD(dage_state_store_vtable_t, reserved, 64);

    CHECK_SIZE(dage_trace_sink_vtable_t, 48);
    CHECK_FIELD(dage_trace_sink_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_trace_sink_vtable_t, emit, 8);
    CHECK_FIELD(dage_trace_sink_vtable_t, reserved, 16);
    CHECK_SIZE(dage_resource_lease_t, 112);
    CHECK_FIELD(dage_resource_lease_t, struct_size, 0);
    CHECK_FIELD(dage_resource_lease_t, lease_id, 8);
    CHECK_FIELD(dage_resource_lease_t, fencing_token, 40);
    CHECK_FIELD(dage_resource_lease_t, expires_at_unix_ms, 48);
    CHECK_FIELD(dage_resource_lease_t, renew, 56);
    CHECK_FIELD(dage_resource_lease_t, release, 64);
    CHECK_FIELD(dage_resource_lease_t, lease_userdata, 72);
    CHECK_FIELD(dage_resource_lease_t, reserved, 80);

    CHECK_SIZE(dage_resource_lease_provider_vtable_t, 48);
    CHECK_FIELD(dage_resource_lease_provider_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_resource_lease_provider_vtable_t, acquire, 8);
    CHECK_FIELD(dage_resource_lease_provider_vtable_t, reserved, 16);
    CHECK_SIZE(dage_resource_provider_t, 72);
    CHECK_FIELD(dage_resource_provider_t, struct_size, 0);
    CHECK_FIELD(dage_resource_provider_t, list_resources, 8);
    CHECK_FIELD(dage_resource_provider_t, read_resource, 16);
    CHECK_FIELD(dage_resource_provider_t, release, 24);
    CHECK_FIELD(dage_resource_provider_t, userdata, 32);
    CHECK_FIELD(dage_resource_provider_t, reserved, 40);

    CHECK_SIZE(dage_bundle_repository_vtable_t, 64);
    CHECK_FIELD(dage_bundle_repository_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_bundle_repository_vtable_t, available_versions, 8);
    CHECK_FIELD(dage_bundle_repository_vtable_t, get, 16);
    CHECK_FIELD(dage_bundle_repository_vtable_t, source, 24);
    CHECK_FIELD(dage_bundle_repository_vtable_t, reserved, 32);
    CHECK_SIZE(dage_key_provider_vtable_t, 48);
    CHECK_FIELD(dage_key_provider_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_key_provider_vtable_t, find_key, 8);
    CHECK_FIELD(dage_key_provider_vtable_t, reserved, 16);
    CHECK_SIZE(dage_trust_policy_vtable_t, 48);
    CHECK_FIELD(dage_trust_policy_vtable_t, struct_size, 0);
    CHECK_FIELD(dage_trust_policy_vtable_t, trust, 8);
    CHECK_FIELD(dage_trust_policy_vtable_t, reserved, 16);

    CHECK_SIZE(dage_bundle_resolver_options_t, 96);
    CHECK_FIELD(dage_bundle_resolver_options_t, struct_size, 0);
    CHECK_FIELD(dage_bundle_resolver_options_t, mode, 4);
    CHECK_FIELD(dage_bundle_resolver_options_t, signature_policy, 8);
    CHECK_FIELD(dage_bundle_resolver_options_t, signature_threshold, 12);
    CHECK_FIELD(dage_bundle_resolver_options_t, lock_json, 16);
    CHECK_FIELD(dage_bundle_resolver_options_t, reserved, 32);
    CHECK_SIZE(dage_metric_definition_t, 88);
    CHECK_FIELD(dage_metric_definition_t, struct_size, 0);
    CHECK_FIELD(dage_metric_definition_t, name, 8);
    CHECK_FIELD(dage_metric_definition_t, weight, 24);
    CHECK_FIELD(dage_metric_definition_t, regression_tolerance, 32);
    CHECK_FIELD(dage_metric_definition_t, evaluate, 40);
    CHECK_FIELD(dage_metric_definition_t, userdata, 48);
    CHECK_FIELD(dage_metric_definition_t, reserved, 56);

    CHECK_VALUE("status.ok", DAGE_STATUS_OK, 0);
    CHECK_VALUE("status.invalid_argument", DAGE_STATUS_INVALID_ARGUMENT, 1);
    CHECK_VALUE("status.out_of_memory", DAGE_STATUS_OUT_OF_MEMORY, 2);
    CHECK_VALUE("status.parse_error", DAGE_STATUS_PARSE_ERROR, 3);
    CHECK_VALUE("status.validation_error", DAGE_STATUS_VALIDATION_ERROR, 4);
    CHECK_VALUE("status.execution_error", DAGE_STATUS_EXECUTION_ERROR, 5);
    CHECK_VALUE("status.cancelled", DAGE_STATUS_CANCELLED, 6);
    CHECK_VALUE("status.not_found", DAGE_STATUS_NOT_FOUND, 7);
    CHECK_VALUE("status.buffer_too_small", DAGE_STATUS_BUFFER_TOO_SMALL, 8);
    CHECK_VALUE("status.suspended", DAGE_STATUS_SUSPENDED, 9);
    CHECK_VALUE("status.resource_exhausted", DAGE_STATUS_RESOURCE_EXHAUSTED, 10);
    CHECK_VALUE("status.internal", DAGE_STATUS_INTERNAL_ERROR, 100);
    CHECK_VALUE("status.unknown", DAGE_STATUS_UNKNOWN_ERROR, 101);
    CHECK_VALUE("run.normal", DAGE_RUN_MODE_NORMAL, 0);
    CHECK_VALUE("run.replay", DAGE_RUN_MODE_REPLAY, 1);
    CHECK_VALUE("run.shadow", DAGE_RUN_MODE_SHADOW, 2);
    CHECK_VALUE("trace.metadata", DAGE_TRACE_CAPTURE_METADATA, 0);
    CHECK_VALUE("trace.off", DAGE_TRACE_CAPTURE_OFF, 1);
    CHECK_VALUE("trace.inputs", DAGE_TRACE_CAPTURE_INPUTS, 2);
    CHECK_VALUE("trace.full", DAGE_TRACE_CAPTURE_FULL, 3);
    CHECK_VALUE("bundle.development", DAGE_BUNDLE_LOAD_DEVELOPMENT, 0);
    CHECK_VALUE("bundle.frozen", DAGE_BUNDLE_LOAD_FROZEN, 1);
    CHECK_VALUE("bundle.verified", DAGE_BUNDLE_LOAD_VERIFIED, 2);
    CHECK_VALUE("bundle.offline", DAGE_BUNDLE_LOAD_OFFLINE, 3);
    CHECK_VALUE("signature.disabled", DAGE_SIGNATURE_DISABLED, 0);
    CHECK_VALUE("signature.at_least_one", DAGE_SIGNATURE_AT_LEAST_ONE, 1);
    CHECK_VALUE("signature.threshold", DAGE_SIGNATURE_THRESHOLD, 2);
    CHECK_VALUE("signature.all", DAGE_SIGNATURE_ALL, 3);
    puts("verified DAGE v1 candidate ABI layout: little-endian, 64-bit");
    return 0;
}
