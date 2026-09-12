/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_ADAPTIVE_PROTOCOL_H__
#define __SPF_HOP_ADAPTIVE_PROTOCOL_H__

#include "spf-hop-protocol.h"

/* Explicit major 2. The V1 geometry structures below are reused in memory;
 * these records MUST NOT be serialized or consumed as fixed-order V1 scans. */
#define SPF_HOP_ADAPTIVE_VERSION UINT16_C(2)
#define SPF_HOP_ADAPTIVE_FEATURES UINT32_C(0x3f)
#define SPF_HOP_ADAPTIVE_REQUEST_BYTES UINT16_C(352)
#define SPF_HOP_ADAPTIVE_EVENT_BYTES UINT16_C(144)
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
int spf_hop_sidecar_v2_encode(void *, size_t, const struct spf_hop_sidecar_v2 *);
int spf_hop_sidecar_v2_decode(struct spf_hop_sidecar_v2 *, const void *, size_t);
int spf_hop_status_v2_encode(void *, size_t, const struct spf_hop_status_v1 *);
int spf_hop_status_v2_decode(struct spf_hop_status_v1 *, const void *, size_t);
int spf_hop_choice_v2_validate(const struct spf_hop_choice_v2 *, const struct spf_hop_event_v1 *);

#endif
