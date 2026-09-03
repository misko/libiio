/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_PROTOCOL_H__
#define __SPF_HOP_PROTOCOL_H__

#include <stddef.h>
#include <stdint.h>

#define SPF_HOP_PROFILE_COUNT UINT16_C(8)
#define SPF_HOP_EVENT_CAPACITY UINT16_C(8)

#define SPF_HOP_REQUEST_MAGIC UINT32_C(0x52504f48) /* HOPR */
#define SPF_HOP_SIDECAR_MAGIC UINT32_C(0x53504f48) /* HOPS */
#define SPF_HOP_STATUS_MAGIC UINT32_C(0x54504f48) /* HOPT */
#define SPF_HOP_PROTOCOL_VERSION UINT16_C(1)

#define SPF_HOP_REQUEST_BYTES UINT16_C(288)
#define SPF_HOP_SIDECAR_HEADER_BYTES UINT16_C(64)
#define SPF_HOP_EVENT_BYTES UINT16_C(80)
#define SPF_HOP_STATUS_BYTES UINT16_C(160)
#define SPF_HOP_SIDECAR_MAX_BYTES \
	(SPF_HOP_SIDECAR_HEADER_BYTES + \
	 SPF_HOP_EVENT_CAPACITY * SPF_HOP_EVENT_BYTES)

enum spf_hop_feature_v1 {
	SPF_HOP_FEATURE_DEVICE_COUNTER_BOUNDS = UINT32_C(1) << 0,
	SPF_HOP_FEATURE_ORDERED_EVENTS = UINT32_C(1) << 1,
	SPF_HOP_FEATURE_EXPLICIT_INVALID_SPANS = UINT32_C(1) << 2,
	SPF_HOP_FEATURE_FAIL_CLOSED_RESTORE = UINT32_C(1) << 3,
	SPF_HOP_FEATURE_SETTINGS_ATTESTED = UINT32_C(1) << 4,
};

#define SPF_HOP_REQUIRED_FEATURES_V1 UINT32_C(0x1f)

enum spf_hop_request_flag_v1 {
	SPF_HOP_REQUEST_FINITE = UINT32_C(1) << 0,
	SPF_HOP_REQUEST_RESTORE_REQUIRED = UINT32_C(1) << 1,
};

#define SPF_HOP_REQUEST_FLAGS_V1 UINT32_C(0x03)

enum spf_hop_status_flag_v1 {
	SPF_HOP_STATUS_TERMINAL = UINT32_C(1) << 0,
	SPF_HOP_STATUS_DEVICE_EVENT_OVERFLOW = UINT32_C(1) << 1,
	SPF_HOP_STATUS_CONTINUITY_FAULT = UINT32_C(1) << 2,
	SPF_HOP_STATUS_RESTORE_ATTEMPTED = UINT32_C(1) << 3,
	SPF_HOP_STATUS_RESTORE_SUCCEEDED = UINT32_C(1) << 4,
	SPF_HOP_STATUS_RESTORE_REQUIRED = UINT32_C(1) << 5,
};

#define SPF_HOP_STATUS_FLAGS_V1 UINT32_C(0x3f)

enum spf_hop_event_flag_v1 {
	SPF_HOP_EVENT_COUNTER_BOUNDS_ATTESTED = UINT8_C(1) << 0,
	SPF_HOP_EVENT_LO_ATTESTED = UINT8_C(1) << 1,
};

#define SPF_HOP_EVENT_FLAGS_V1 UINT8_C(0x03)
#define SPF_HOP_PROFILE_NONE UINT8_C(0xff)

enum spf_hop_event_kind_v1 {
	SPF_HOP_EVENT_STARTUP = 1,
	SPF_HOP_EVENT_RETUNE = 2,
};

enum spf_hop_state_v1 {
	SPF_HOP_STATE_IDLE = 0,
	SPF_HOP_STATE_ARMED = 1,
	SPF_HOP_STATE_RUNNING = 2,
	SPF_HOP_STATE_COMPLETED = 3,
	SPF_HOP_STATE_CANCELLED = 4,
	SPF_HOP_STATE_FAILED = 5,
};

enum spf_hop_reason_v1 {
	SPF_HOP_REASON_NONE = 0,
	SPF_HOP_REASON_PLAN_COMPLETE = 1,
	SPF_HOP_REASON_CLIENT_CLOSE = 2,
	SPF_HOP_REASON_CLIENT_DISCONNECT = 3,
	SPF_HOP_REASON_DEVICE_ERROR = 4,
	SPF_HOP_REASON_EVENT_OVERFLOW = 5,
	SPF_HOP_REASON_EVENT_SEQUENCE = 6,
	SPF_HOP_REASON_COUNTER_DISCONTINUITY = 7,
	SPF_HOP_REASON_PROTOCOL_ERROR = 8,
	SPF_HOP_REASON_RESTORE_ERROR = 9,
};

struct spf_hop_profile_v1 {
	uint8_t profile_id;
	uint8_t fastlock_slot;
	uint64_t center_frequency_hz;
	uint64_t lo_frequency_hz;
	uint32_t profile_crc32;
};

struct spf_hop_request_v1 {
	uint32_t required_features;
	uint32_t flags;
	uint64_t session_id;
	uint64_t sample_rate_hz;
	uint64_t rf_bandwidth_hz;
	int64_t if_offset_hz;
	/* Useful, target-assigned samples after each activation invalid_end. */
	uint64_t dwell_samples;
	uint64_t transition_guard_samples;
	/* Safety ceiling, not a command to run every visit. */
	uint64_t dwell_count;
	/* Device-counter envelope target from the startup transition start. */
	uint64_t capture_span_samples;
	uint8_t initial_profile;
	struct spf_hop_profile_v1 profiles[SPF_HOP_PROFILE_COUNT];
};

/* Values in this structure are emitted by the device provider.  iiOD never
 * substitutes host time or an inferred buffer boundary for these fields. */
struct spf_hop_device_event_v1 {
	uint64_t event_sequence;
	uint64_t dwell_index;
	uint64_t transition_before;
	uint64_t transition_after;
	uint64_t actual_lo_frequency_hz;
	int64_t actual_if_offset_hz;
	uint64_t device_event_id;
	uint8_t from_profile;
	uint8_t to_profile;
	uint8_t kind;
	uint8_t flags;
	uint16_t fastlock_slot;
};

struct spf_hop_event_v1 {
	struct spf_hop_device_event_v1 device;
	uint64_t invalid_start;
	uint64_t invalid_end;
};

struct spf_hop_sidecar_v1 {
	uint32_t flags;
	uint64_t session_id;
	uint64_t buffer_sequence;
	uint64_t block_first_sample;
	uint64_t block_end_sample;
	uint16_t state;
	uint16_t terminal_reason;
	int32_t error_code;
	uint16_t event_count;
	struct spf_hop_event_v1 events[SPF_HOP_EVENT_CAPACITY];
};

struct spf_hop_status_v1 {
	uint16_t state;
	uint16_t terminal_reason;
	int32_t error_code;
	uint32_t flags;
	uint64_t session_id;
	uint64_t planned_dwells;
	uint64_t visits_started;
	uint64_t events_emitted;
	uint64_t next_event_sequence;
	uint64_t last_block_sequence;
	uint64_t last_block_end;
	uint64_t first_counter;
	uint64_t final_counter;
	uint64_t restore_before;
	uint64_t restore_after;
	uint64_t restored_lo_frequency_hz;
	int32_t restore_error;
	uint8_t active_profile;
	uint8_t restored_profile;
	uint64_t startup_invalid_start;
	uint64_t startup_invalid_end;
	uint64_t device_dropped_events;
};

int spf_hop_request_v1_decode(struct spf_hop_request_v1 *request,
	const void *wire, size_t wire_bytes);
int spf_hop_request_v1_encode(void *wire, size_t wire_bytes,
	const struct spf_hop_request_v1 *request);

int spf_hop_sidecar_v1_decode(struct spf_hop_sidecar_v1 *sidecar,
	const void *wire, size_t wire_bytes);
int spf_hop_sidecar_v1_encode(void *wire, size_t wire_bytes,
	const struct spf_hop_sidecar_v1 *sidecar);

int spf_hop_status_v1_decode(struct spf_hop_status_v1 *status,
	const void *wire, size_t wire_bytes);
int spf_hop_status_v1_encode(void *wire, size_t wire_bytes,
	const struct spf_hop_status_v1 *status);

#endif
