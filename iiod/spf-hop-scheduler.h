/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_SCHEDULER_H__
#define __SPF_HOP_SCHEDULER_H__

#include "spf-hop-session.h"

#include <stdint.h>

struct spf_hop_scheduler_transition_v1 {
	uint64_t transition_before;
	uint64_t transition_after;
	uint64_t actual_lo_frequency_hz;
	uint64_t device_event_id;
	uint32_t active_profile;
};

struct spf_hop_scheduler_restore_v1 {
	uint64_t transition_before;
	uint64_t transition_after;
	uint64_t actual_lo_frequency_hz;
	uint32_t active_profile;
};

struct spf_hop_scheduler_io_v1 {
	int (*start)(void *opaque, uint64_t expected_original_lo_hz);
	int (*get_counter)(void *opaque, uint64_t *sample_counter);
	int (*recall)(void *opaque, uint32_t profile, uint64_t expected_lo_hz,
		struct spf_hop_scheduler_transition_v1 *transition);
	int (*restore)(void *opaque, uint64_t expected_original_lo_hz,
		struct spf_hop_scheduler_restore_v1 *restore);
	int (*sleep_ns)(void *opaque, uint64_t nanoseconds);
	void (*destroy)(void *opaque);
};

int spf_hop_scheduler_v1_create(const struct spf_hop_request_v1 *request,
	const struct spf_hop_scheduler_io_v1 *io, void *io_context,
	void **device_context, const struct spf_hop_device_ops_v1 **ops);
void spf_hop_scheduler_v1_destroy(void *device_context);

#endif
