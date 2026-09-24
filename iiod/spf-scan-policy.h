/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_SCAN_POLICY_H
#define SPF_SCAN_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define SPF_SCAN_TARGETS 8U
#define SPF_SCAN_MAX_VISITS 16384U
#define SPF_SCAN_ACK_CAPACITY 64U
#define SPF_SCAN_WEIGHT_ONE 65536U

/* Policy is transport-independent. One session mutex must serialize its API.
 * All times are uint64 source-clock ticks, never host timestamps or doubles.
 * Allocate once at setup; no allocation or IO occurs at a dwell boundary. */
struct spf_scan_policy;

struct spf_scan_policy_config {
	uint64_t session, generation, seed;
	uint16_t protocol_version;
	uint32_t source_rate_hz, targets, duration_ms, dwell_ms;
	uint32_t transition_budget_ms, maximum_revisit_ms;
	uint32_t feedback_age_ms, application_delay_ms, decay_ms;
	uint32_t maximum_boost, baseline[SPF_SCAN_TARGETS];
	uint8_t analysis_digest[32];
};

enum spf_scan_outcome { SPF_SCAN_UNKNOWN, SPF_SCAN_ACTIVE, SPF_SCAN_QUIET };
enum spf_scan_feedback_result {
	SPF_SCAN_ACCEPTED, SPF_SCAN_DUPLICATE, SPF_SCAN_EXPIRED,
	SPF_SCAN_WRONG_SESSION, SPF_SCAN_REJECTED, SPF_SCAN_SUPERSEDED,
	SPF_SCAN_MAILBOX_FULL, SPF_SCAN_APPLIED, SPF_SCAN_CANCELLED
};

struct spf_scan_feedback {
	uint64_t session, generation, sequence, visit, valid_start, valid_end;
	uint32_t target, outcome;
	uint8_t analysis_digest[32];
};

struct spf_scan_ack {
	uint64_t sequence, source_visit, first_visit;
	uint64_t received_counter, application_counter;
	uint32_t target, result, old_boost, new_boost;
};

struct spf_scan_choice {
	uint64_t visit, selection_counter;
	uint32_t target, eligible_mask, effective_weight, dwell_ms;
	bool deadline_forced;
};

int spf_scan_policy_validate(const struct spf_scan_policy_config *config);
int spf_scan_policy_create(struct spf_scan_policy **out,
	const struct spf_scan_policy_config *config, uint64_t start_counter);
void spf_scan_policy_destroy(struct spf_scan_policy *policy);
/* A successful select must be followed by commit or stop, never another select.
 * commit records actual post-settling start; hardware lateness fails closed. */
int spf_scan_policy_select(struct spf_scan_policy *policy, uint64_t now,
	struct spf_scan_choice *choice);
int spf_scan_policy_commit(struct spf_scan_policy *policy, uint64_t valid_start);
int spf_scan_policy_finish_visit(struct spf_scan_policy *policy, uint64_t visit,
	bool intact);
enum spf_scan_feedback_result spf_scan_policy_feedback(
	struct spf_scan_policy *policy, const struct spf_scan_feedback *feedback,
	uint64_t received_counter);
/* One terminal acknowledgement for every accepted feedback, including
 * supersession/cancellation. Admission reserves space so none can be lost. */
int spf_scan_policy_take_ack(struct spf_scan_policy *policy,
	struct spf_scan_ack *ack);
void spf_scan_policy_stop(struct spf_scan_policy *policy, uint64_t now);

#endif
