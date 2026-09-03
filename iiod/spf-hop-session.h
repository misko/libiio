/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_HOP_SESSION_H__
#define __SPF_HOP_SESSION_H__

#include "spf-hop-protocol.h"

#include <stddef.h>
#include <stdint.h>

struct spf_hop_restore_receipt_v1 {
	uint64_t transition_before;
	uint64_t transition_after;
	uint64_t restored_lo_frequency_hz;
	int32_t error_code;
	uint8_t restored_profile;
	uint8_t flags;
};

struct spf_hop_device_ops_v1 {
	/* Atomically validates/settings-attests all eight profiles and submits the
	 * complete finite plan.  It must not return success after a partial plan. */
	int (*submit_plan)(void *device_context,
		const struct spf_hop_request_v1 *request);
	/* Drains device-originated events. dropped_events is cumulative and must
	 * be zero for a valid session. */
	int (*drain_events)(void *device_context,
		struct spf_hop_device_event_v1 *events, size_t capacity,
		size_t *event_count, uint64_t *dropped_events);
	/* Stops all pending hops and restores the pre-session settings atomically.
	 * The receipt's counter bounds and LO are device-attested. */
	int (*cancel_restore)(void *device_context, uint16_t reason,
		struct spf_hop_restore_receipt_v1 *receipt);
};

struct spf_hop_session_v1 {
	struct spf_hop_request_v1 request;
	struct spf_hop_status_v1 status;
	const struct spf_hop_device_ops_v1 *ops;
	void *device_context;
	struct spf_hop_event_v1 last_event;
	uint64_t last_device_event_id;
	int terminal_error;
	uint8_t have_last_event;
	uint8_t have_last_block;
	uint8_t restore_called;
};

int spf_hop_session_v1_init(struct spf_hop_session_v1 *session,
	const struct spf_hop_request_v1 *request,
	const struct spf_hop_device_ops_v1 *ops, void *device_context);
int spf_hop_session_v1_start(struct spf_hop_session_v1 *session);
int spf_hop_session_v1_on_block(struct spf_hop_session_v1 *session,
	uint64_t buffer_sequence, uint64_t first_sample, uint64_t block_end,
	struct spf_hop_sidecar_v1 *sidecar);
int spf_hop_session_v1_cancel(struct spf_hop_session_v1 *session,
	uint16_t reason);
void spf_hop_session_v1_get_status(const struct spf_hop_session_v1 *session,
	struct spf_hop_status_v1 *status);

#endif
