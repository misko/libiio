/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_SCAN_SESSION_H
#define SPF_SCAN_SESSION_H

#include "spf-scan-protocol.h"
#include "spf-scan-radio.h"

#include <stdbool.h>
#include <stdint.h>

struct spf_scan_session;

struct spf_scan_session_runtime {
	unsigned block_count, headroom_blocks;
	uint32_t block_samples, bytes_per_sample;
	uint64_t drain_bytes_per_second;
	int (*release_block)(void *context, uintptr_t token);
	void *release_context;
};

struct spf_scan_session_output {
	struct spf_scan_visit_record record;
	unsigned slice_count;
	struct spf_visit_slice slices[SPF_VISIT_QUEUE_MAX_BLOCKS];
};

struct spf_scan_gain_observation {
	uint64_t counter;
	uint32_t read_duration_ns;
	uint8_t rx1_gain_index, rx2_gain_index;
	bool valid;
};

int spf_scan_session_create(struct spf_scan_session **out,
	const struct spf_scan_setup *setup,
	const struct spf_scan_session_runtime *runtime,
	struct spf_scan_radio *radio, uint64_t start_counter);
/* Before the first visit, align the owner's low-word counter with a full-width
 * timestamp observed in DMA.  No scan decision is exposed until both clocks
 * share one epoch. */
int spf_scan_session_rebase(struct spf_scan_session *session,
	uint64_t full_counter_anchor);
int spf_scan_session_schedule(struct spf_scan_session *session,
	uint64_t now, uint64_t counter_anchor, struct spf_scan_choice *choice);
int spf_scan_session_observe_gain(struct spf_scan_session *session,
	uint64_t visit, const struct spf_scan_gain_observation *observation);
int spf_scan_session_next_boundary(const struct spf_scan_session *session,
	uint64_t *counter);
int spf_scan_session_counter(const struct spf_scan_session *session,
	uint64_t *counter);
bool spf_scan_session_capture_complete(const struct spf_scan_session *session);
int spf_scan_session_feed(struct spf_scan_session *session, uintptr_t token,
	const void *data, uint64_t first, uint32_t samples);
enum spf_scan_feedback_result spf_scan_session_feedback(
	struct spf_scan_session *session,
	const struct spf_scan_feedback *feedback, uint64_t received_counter);
int spf_scan_session_take_ack(struct spf_scan_session *session,
	struct spf_scan_ack *ack);
int spf_scan_session_take_output(struct spf_scan_session *session,
	struct spf_scan_session_output *output);
int spf_scan_session_complete_output(struct spf_scan_session *session,
	uint64_t visit);
int spf_scan_session_abort_output(struct spf_scan_session *session,
	uint64_t visit, int transport_error);
int spf_scan_session_stop(struct spf_scan_session *session,
	uint64_t final_counter);
int spf_scan_session_cancel(struct spf_scan_session *session,
	uint64_t final_counter);
int spf_scan_session_fail(struct spf_scan_session *session,
	uint64_t final_counter, int error);
int spf_scan_session_terminal(struct spf_scan_session *session,
	struct spf_scan_terminal *terminal);
int spf_scan_session_destroy(struct spf_scan_session *session);

#endif
