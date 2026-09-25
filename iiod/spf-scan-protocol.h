/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_SCAN_PROTOCOL_H
#define SPF_SCAN_PROTOCOL_H

#include "spf-scan-policy.h"
#include "spf-visit-queue.h"

#include <stddef.h>
#include <stdint.h>

#include "spf-scan-rate.h"
#define SPF_SCAN_RATE_MODE_SETUP_VALIDATED UINT32_C(1)
#define SPF_SCAN_RATE_MODE_GAIN_OBSERVATION UINT32_C(2)
#define SPF_SCAN_PROTOCOL_FEATURES UINT32_C(0x000000ff)
#define SPF_SCAN_SETUP_FLAGS UINT32_C(0x00000001)
#define SPF_SCAN_VISIT_FLAGS UINT32_C(0x00000001)
#define SPF_SCAN_VISIT_DEADLINE_FORCED UINT32_C(0x00000002)
#define SPF_SCAN_VISIT_FLAG_MASK \
	(SPF_SCAN_VISIT_FLAGS | SPF_SCAN_VISIT_DEADLINE_FORCED)
#define SPF_SCAN_TERMINAL_FLAGS UINT32_C(0x00000001)
#define SPF_SCAN_FORMAT_CI16 UINT32_C(1)
/* v1 originally allocated bits 0..3 to 10/15/20/30 MS/s.  2.5 MS/s is an
 * extension and must append bit 4; renumbering would make an old client read
 * a 10 MS/s-only endpoint as 2.5 MS/s capable. */
#define SPF_SCAN_RATE_10M (UINT32_C(1) << 0)
#define SPF_SCAN_RATE_15M (UINT32_C(1) << 1)
#define SPF_SCAN_RATE_20M (UINT32_C(1) << 2)
#define SPF_SCAN_RATE_30M (UINT32_C(1) << 3)
#define SPF_SCAN_RATE_2P5M (UINT32_C(1) << 4)
#define SPF_SCAN_RATE_5M (UINT32_C(1) << 5)
#define SPF_SCAN_RATE_7P5M (UINT32_C(1) << 6)
#define SPF_SCAN_RATE_8M (UINT32_C(1) << 7)
#define SPF_SCAN_RATE_MASK_FIXED \
	(SPF_SCAN_RATE_2P5M | SPF_SCAN_RATE_10M | SPF_SCAN_RATE_15M | \
	 SPF_SCAN_RATE_20M | SPF_SCAN_RATE_30M)
#define SPF_SCAN_RATE_MASK_RANDOM_DWELL \
	(SPF_SCAN_RATE_2P5M | SPF_SCAN_RATE_5M | SPF_SCAN_RATE_7P5M | \
	 SPF_SCAN_RATE_10M)

/* RX1 is the classifier source.  RX2 may only be added as a synchronous
 * capture stream; there is never a per-receiver tuning decision. */
#define SPF_SCAN_RX1 UINT32_C(1)
#define SPF_SCAN_RX1_RX2 (SPF_SCAN_RX1 | UINT32_C(2))

#define SPF_SCAN_CAPS_BYTES 96U
#define SPF_SCAN_SETUP_BYTES 352U
#define SPF_SCAN_VISIT_BYTES 160U
#define SPF_SCAN_FEEDBACK_BYTES 112U
#define SPF_SCAN_ACK_BYTES 96U
#define SPF_SCAN_TERMINAL_BYTES 128U
#define SPF_SCAN_TIME_QUERY_BYTES 48U
#define SPF_SCAN_TIME_BYTES 128U

/* Separate opt-in command; existing published scan records are unchanged.
 * UINT64_MAX snapshot age means unknown, never zero uncertainty. */
struct spf_scan_time_query {
	uint64_t request, session, generation;
};
struct spf_scan_time {
	struct spf_scan_time_query identity;
	uint8_t boot_id[16];
	uint64_t epoch, counter, monotonic_before_ns, monotonic_after_ns;
	uint32_t sample_rate_hz;
	uint64_t maximum_snapshot_age_ns;
};
int spf_scan_time_query_encode(void *, size_t, const struct spf_scan_time_query *);
int spf_scan_time_query_decode(struct spf_scan_time_query *, const void *, size_t);
int spf_scan_time_encode(void *, size_t, const struct spf_scan_time *);
int spf_scan_time_decode(struct spf_scan_time *, const void *, size_t);

enum spf_scan_terminal_state {
	SPF_SCAN_TERMINAL_COMPLETED = 1,
	SPF_SCAN_TERMINAL_CANCELLED = 2,
	SPF_SCAN_TERMINAL_FAILED = 3
};

struct spf_scan_caps {
	uint16_t protocol_version; /* zero initializes the legacy version */
	uint32_t rate_mode, minimum_rate_hz, maximum_rate_hz;
	uint32_t rate_mask, rx_mask, formats, maximum_targets;
	uint32_t maximum_fastlock_profiles, minimum_dwell_ms, maximum_dwell_ms;
	uint32_t maximum_duration_ms;
	uint64_t maximum_queue_bytes;
	uint32_t maximum_queue_age_ms, feedback_capacity;
	uint32_t maximum_feedback_age_ms, maximum_application_delay_ms;
	uint32_t maximum_analog_bandwidth_hz, source_counter_bits;
};

struct spf_scan_target {
	uint32_t channel, profile;
	uint64_t frequency_hz;
	uint32_t baseline_weight, profile_crc32;
};

struct spf_scan_setup {
	uint16_t protocol_version;
	uint64_t session, generation, seed;
	uint32_t source_rate_hz, analog_bandwidth_hz;
	uint32_t duration_ms, dwell_ms, transition_budget_ms;
	uint32_t maximum_revisit_ms, feedback_age_ms, application_delay_ms;
	uint32_t decay_ms, maximum_boost;
	uint64_t maximum_queue_bytes;
	uint32_t maximum_queue_age_ms, maximum_queue_visits;
	uint32_t target_count, rx_mask, format, flags;
	uint8_t analysis_digest[32];
	struct spf_scan_target targets[SPF_SCAN_TARGETS];
};

struct spf_scan_visit_record {
	uint16_t protocol_version;
	uint64_t session, generation, visit, selection_counter;
	uint64_t transition_before, transition_after, valid_start, valid_end;
	uint64_t frequency_hz, iq_bytes, missing_samples_before;
	uint32_t analog_bandwidth_hz, source_rate_hz;
	uint32_t target, profile, result, eligible_mask, effective_weight;
	uint32_t profile_crc32, flags;
	uint64_t gain_counter;
	uint32_t gain_read_duration_ns;
	uint8_t rx1_gain_index, rx2_gain_index, gain_valid;
};

struct spf_scan_terminal {
	uint64_t session, generation, final_counter;
	uint64_t restore_before, restore_after;
	uint64_t planned, delivered, skipped, invalid, cancelled, iq_bytes;
	uint32_t state, reason;
	int32_t error;
	uint32_t flags;
};

void spf_scan_caps_default(struct spf_scan_caps *caps);
int spf_scan_setup_validate(const struct spf_scan_setup *setup);
int spf_scan_setup_policy(const struct spf_scan_setup *setup,
			  struct spf_scan_policy_config *policy);

int spf_scan_caps_encode(void *wire, size_t bytes,
			 const struct spf_scan_caps *caps);
int spf_scan_caps_decode(struct spf_scan_caps *caps,
			 const void *wire, size_t bytes);
int spf_scan_setup_encode(void *wire, size_t bytes,
			  const struct spf_scan_setup *setup);
int spf_scan_setup_decode(struct spf_scan_setup *setup,
			  const void *wire, size_t bytes);
int spf_scan_visit_encode(void *wire, size_t bytes,
			  const struct spf_scan_visit_record *visit);
int spf_scan_visit_decode(struct spf_scan_visit_record *visit,
			  const void *wire, size_t bytes);
int spf_scan_feedback_encode(void *wire, size_t bytes,
			     const struct spf_scan_feedback *feedback);
int spf_scan_feedback_decode(struct spf_scan_feedback *feedback,
			     const void *wire, size_t bytes);
int spf_scan_ack_encode(void *wire, size_t bytes,
			const struct spf_scan_ack *ack);
int spf_scan_ack_decode(struct spf_scan_ack *ack,
			const void *wire, size_t bytes);
int spf_scan_terminal_encode(void *wire, size_t bytes,
			     const struct spf_scan_terminal *terminal);
int spf_scan_terminal_decode(struct spf_scan_terminal *terminal,
			     const void *wire, size_t bytes);

#endif
