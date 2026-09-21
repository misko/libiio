/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_ADAPTIVE_PROTOCOL_H__
#define __SPF_HOP_ADAPTIVE_PROTOCOL_H__

#include "spf-hop-protocol.h"

/* Explicit major 2. The V1 geometry structures below are reused in memory;
 * these records MUST NOT be serialized or consumed as fixed-order V1 scans. */
#define SPF_HOP_ADAPTIVE_VERSION UINT16_C(2)
#define SPF_HOP_ADAPTIVE_FEATURES UINT32_C(0x3f)
#define SPF_HOP_ELIGIBLE_TARGETS_FEATURE UINT32_C(0x40)
#define SPF_HOP_ADAPTIVE_MASK_FEATURES \
	(SPF_HOP_ADAPTIVE_FEATURES | SPF_HOP_ELIGIBLE_TARGETS_FEATURE)
#define SPF_HOP_ADAPTIVE_REQUEST_BYTES UINT16_C(352)
#define SPF_HOP_ADAPTIVE_EVENT_BYTES UINT16_C(144)
#define SPF_HOP_HOST_REQUEST_BYTES UINT16_C(416)
#define SPF_HOP_HOST_FEEDBACK_BYTES UINT16_C(160)
#define SPF_HOP_ADAPTIVE_SIDECAR_MAX_BYTES \
	(SPF_HOP_SIDECAR_HEADER_BYTES + SPF_HOP_EVENT_CAPACITY * SPF_HOP_ADAPTIVE_EVENT_BYTES)

enum spf_hop_adaptive_mode { SPF_HOP_SHADOW = 1, SPF_HOP_ADAPTIVE = 2 };
enum spf_hop_adaptive_reason {
	SPF_HOP_CHOICE_WARMUP, SPF_HOP_CHOICE_WEIGHTED, SPF_HOP_CHOICE_EXPLORATION,
	SPF_HOP_CHOICE_NONE_ACTIVE, SPF_HOP_CHOICE_FAULT_FALLBACK
};

struct spf_hop_policy_v2 {
	uint64_t generation;
	uint32_t mode;
	uint32_t warmup_visits;
	uint32_t missed_dwells;
	uint32_t active_weight;
	uint32_t quiet_weight;
	uint32_t cooldown_ms;
	uint32_t maximum_revisit_ms;
	uint32_t hop_budget_ms;
	uint32_t maximum_result_age_ms;
	uint32_t unhealthy_limit;
};

struct spf_hop_request_v2 {
	struct spf_hop_request_v1 geometry;
	struct spf_hop_policy_v2 policy;
	/* 0 is accepted only for legacy in-process initializers and means 0xff.
	 * Decoders always normalize it to one nonzero uint8 eligibility bitmap. */
	uint8_t eligible_target_mask;
	/* Private shared engine configuration. Nonzero only after explicit HOPR
	 * major-3 decoding; the published V2 encoder rejects these fields. */
	struct {
		uint32_t enabled, rx, decision_rate_hz, factor, phase, delay;
		uint32_t supported_start, supported_end;
		uint8_t configuration_sha256[32];
	} host;
};

struct spf_hop_host_feedback_v1 {
	uint64_t session, generation, stream_id, visit, event_sequence, valid_start, valid_end;
	uint32_t source_rate_hz, decision_rate_hz, rx, target, outcome, healthy;
	uint32_t screen_mask, confirmation_mask, supported_start, supported_end, factor, phase, delay;
	uint8_t configuration_sha256[32];
};

/* Recommendation describes the proposed target; the event's to_profile is the
 * actual target. They may differ ONLY in shadow mode. Visit is event.dwell_index.
 * decision_counter shares the source-counter epoch of the attested transition.
 * generation binds to the complete policy retained in HOPR V2. */
struct spf_hop_choice_v2 {
	uint64_t decision_counter;
	uint64_t basis_visit; /* UINT64_MAX: no applied observation */
	uint64_t cooldown_remaining_samples;
	uint64_t generation;
	uint32_t proposed_target;
	uint32_t reason;
	uint32_t active_mask;
	uint32_t quiet_mask;
	uint32_t consecutive_misses;
	uint32_t mode;
};

struct spf_hop_sidecar_v2 {
	struct spf_hop_sidecar_v1 geometry;
	struct spf_hop_choice_v2 choices[SPF_HOP_EVENT_CAPACITY];
};

int spf_hop_request_v2_encode(void *, size_t, const struct spf_hop_request_v2 *);
int spf_hop_request_v2_decode(struct spf_hop_request_v2 *, const void *, size_t);
int spf_hop_request_mask_v2_encode(void *, size_t, const struct spf_hop_request_v2 *);
int spf_hop_request_mask_v2_decode(struct spf_hop_request_v2 *, const void *, size_t);
int spf_hop_request_v3_encode(void *, size_t, const struct spf_hop_request_v2 *);
int spf_hop_request_v3_decode(struct spf_hop_request_v2 *, const void *, size_t);
int spf_hop_request_v4_encode(void *, size_t, const struct spf_hop_request_v2 *);
int spf_hop_request_v4_decode(struct spf_hop_request_v2 *, const void *, size_t);
/* Canonical internal configuration, padded to 416 bytes. Never a wire record. */
int spf_hop_adaptive_configuration(void *, size_t, const struct spf_hop_request_v2 *);
int spf_hop_host_feedback_v1_decode(struct spf_hop_host_feedback_v1 *, const void *, size_t);
int spf_hop_host_feedback_v1_encode(void *, size_t, const struct spf_hop_host_feedback_v1 *);
int spf_hop_host_feedback_v2_decode(struct spf_hop_host_feedback_v1 *, const void *, size_t);
int spf_hop_host_feedback_v2_encode(void *, size_t, const struct spf_hop_host_feedback_v1 *);
int spf_hop_host_feedback_encode(void *, size_t, const struct spf_hop_host_feedback_v1 *);
int spf_hop_sidecar_v2_encode(void *, size_t, const struct spf_hop_sidecar_v2 *);
int spf_hop_sidecar_v2_decode(struct spf_hop_sidecar_v2 *, const void *, size_t);
int spf_hop_sidecar_v3_encode(void *, size_t, const struct spf_hop_sidecar_v2 *);
int spf_hop_sidecar_v3_decode(struct spf_hop_sidecar_v2 *, const void *, size_t);
int spf_hop_status_v2_encode(void *, size_t, const struct spf_hop_status_v1 *);
int spf_hop_status_v2_decode(struct spf_hop_status_v1 *, const void *, size_t);
int spf_hop_status_v3_encode(void *, size_t, const struct spf_hop_status_v1 *);
int spf_hop_choice_v2_validate(const struct spf_hop_choice_v2 *, const struct spf_hop_event_v1 *);

#endif
