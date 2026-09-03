/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-session.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static bool center_matches_actual(uint64_t center, uint64_t lo,
	int64_t offset)
{
	uint64_t magnitude;

	if (offset >= 0) {
		magnitude = (uint64_t)offset;
		return lo <= UINT64_MAX - magnitude && center == lo + magnitude;
	}
	magnitude = (uint64_t)(-(offset + 1)) + UINT64_C(1);
	return lo >= magnitude && center == lo - magnitude;
}

static uint64_t extend_counter_near(uint64_t reference, uint64_t observed)
{
	uint64_t candidate = (reference & UINT64_C(0xffffffff00000000)) |
		(uint32_t)observed;

	if (candidate < reference &&
			reference - candidate > UINT64_C(0x80000000))
		candidate += UINT64_C(0x100000000);
	else if (candidate > reference &&
			candidate - reference > UINT64_C(0x80000000) &&
			candidate >= UINT64_C(0x100000000))
		candidate -= UINT64_C(0x100000000);
	return candidate;
}

static bool terminal(const struct spf_hop_session_v1 *session)
{
	return session->status.state >= SPF_HOP_STATE_COMPLETED;
}

static int normalize_error(int error)
{
	return error > 0 ? -EIO : error;
}

static int restore(struct spf_hop_session_v1 *session, uint16_t reason,
	uint16_t final_state, uint64_t required_final_counter)
{
	struct spf_hop_restore_receipt_v1 receipt;
	int ret;

	if (session->restore_called)
		return session->status.restore_error;
	session->restore_called = 1;
	session->status.flags |= SPF_HOP_STATUS_RESTORE_ATTEMPTED;
	memset(&receipt, 0, sizeof(receipt));
	receipt.restored_profile = SPF_HOP_PROFILE_NONE;
	ret = session->ops->cancel_restore(session->device_context, reason,
		&receipt);
	ret = normalize_error(ret);
	/* Software-only devices observe the FPGA's low counter register while
	 * metadata IQ carries the full inserted 64-bit counter.  Re-anchor either
	 * representation at the last delivered block before publishing HOPT. */
	if (session->have_last_block) {
		uint64_t reference = required_final_counter ?
			required_final_counter : session->status.last_block_end;

		receipt.transition_before = extend_counter_near(reference,
			receipt.transition_before);
		receipt.transition_after = extend_counter_near(
			receipt.transition_before, receipt.transition_after);
	}
	session->status.restore_before = receipt.transition_before;
	session->status.restore_after = receipt.transition_after;
	session->status.restored_lo_frequency_hz =
		receipt.restored_lo_frequency_hz;
	session->status.restored_profile = receipt.restored_profile;
	session->status.restore_error = ret ? ret :
		normalize_error(receipt.error_code);
	if (!ret && !receipt.error_code &&
		receipt.flags == SPF_HOP_EVENT_FLAGS_V1 &&
		receipt.transition_before <= receipt.transition_after &&
		receipt.restored_lo_frequency_hz) {
		if (reason == SPF_HOP_REASON_PLAN_COMPLETE &&
			receipt.transition_before < required_final_counter) {
			session->status.restore_error = -ERANGE;
		} else {
			session->status.flags |= SPF_HOP_STATUS_RESTORE_SUCCEEDED;
		}
	} else if (!session->status.restore_error) {
		session->status.restore_error = -EBADMSG;
	}
	session->status.state = final_state;
	session->status.terminal_reason = reason;
	session->status.flags |= SPF_HOP_STATUS_TERMINAL;
	if (reason == SPF_HOP_REASON_PLAN_COMPLETE)
		session->status.final_counter = required_final_counter;
	else
		session->status.final_counter = receipt.transition_before;
	if (!(session->status.flags & SPF_HOP_STATUS_RESTORE_SUCCEEDED)) {
		session->status.state = SPF_HOP_STATE_FAILED;
		session->status.terminal_reason = SPF_HOP_REASON_RESTORE_ERROR;
		session->status.error_code = session->status.restore_error;
		return session->status.restore_error;
	}
	return 0;
}

static int fail(struct spf_hop_session_v1 *session, uint16_t reason, int error)
{
	int restore_ret;

	if (terminal(session))
		return session->terminal_error ? session->terminal_error : -EPIPE;
	session->status.error_code = error ? error : -EIO;
	if (reason == SPF_HOP_REASON_EVENT_OVERFLOW)
		session->status.flags |= SPF_HOP_STATUS_DEVICE_EVENT_OVERFLOW;
	if (reason == SPF_HOP_REASON_EVENT_SEQUENCE ||
		reason == SPF_HOP_REASON_COUNTER_DISCONTINUITY)
		session->status.flags |= SPF_HOP_STATUS_CONTINUITY_FAULT;
	restore_ret = restore(session, reason, SPF_HOP_STATE_FAILED, 0);
	if (!restore_ret) {
		/* restore() deliberately keeps the caller's failure reason/state when
		 * restoration succeeds. */
		session->status.state = SPF_HOP_STATE_FAILED;
		session->status.terminal_reason = reason;
		session->status.error_code = error ? error : -EIO;
	}
	session->terminal_error = error ? error : (restore_ret ? restore_ret : -EIO);
	return session->terminal_error;
}

int spf_hop_session_v1_init(struct spf_hop_session_v1 *session,
	const struct spf_hop_request_v1 *request,
	const struct spf_hop_device_ops_v1 *ops, void *device_context)
{
	uint8_t wire[SPF_HOP_REQUEST_BYTES];
	int ret;

	if (!session || !request || !ops || !ops->submit_plan ||
		!ops->drain_events || !ops->cancel_restore)
		return -EINVAL;
	/* The protocol encoder is the single canonical semantic validator. */
	ret = spf_hop_request_v1_encode(wire, sizeof(wire), request);
	if (ret)
		return ret;
	memset(session, 0, sizeof(*session));
	session->request = *request;
	session->ops = ops;
	session->device_context = device_context;
	session->status.state = SPF_HOP_STATE_IDLE;
	session->status.flags = SPF_HOP_STATUS_RESTORE_REQUIRED;
	session->status.session_id = request->session_id;
	session->status.planned_dwells = request->dwell_count;
	session->status.active_profile = SPF_HOP_PROFILE_NONE;
	session->status.restored_profile = SPF_HOP_PROFILE_NONE;
	return 0;
}

int spf_hop_session_v1_start(struct spf_hop_session_v1 *session)
{
	int ret;

	if (!session || session->status.state != SPF_HOP_STATE_IDLE)
		return -EINVAL;
	session->status.state = SPF_HOP_STATE_ARMED;
	ret = session->ops->submit_plan(session->device_context,
		&session->request);
	ret = normalize_error(ret);
	if (ret)
		return fail(session, SPF_HOP_REASON_DEVICE_ERROR, ret);
	session->status.state = SPF_HOP_STATE_RUNNING;
	return 0;
}

static int validate_device_event(struct spf_hop_session_v1 *session,
	const struct spf_hop_device_event_v1 *device,
	struct spf_hop_event_v1 *event)
{
	const struct spf_hop_profile_v1 *profile;
	uint8_t expected_from;

	if (device->event_sequence != session->status.next_event_sequence ||
		device->dwell_index != session->status.visits_started ||
		device->dwell_index >= session->request.dwell_count)
		return -EILSEQ;
	if (device->dwell_index == 0) {
		expected_from = SPF_HOP_PROFILE_NONE;
		if (device->kind != SPF_HOP_EVENT_STARTUP)
			return -EILSEQ;
	} else {
		expected_from = (uint8_t)((device->dwell_index - 1) %
			SPF_HOP_PROFILE_COUNT);
		if (device->kind != SPF_HOP_EVENT_RETUNE)
			return -EILSEQ;
	}
	if (device->from_profile != expected_from ||
		device->to_profile != device->dwell_index % SPF_HOP_PROFILE_COUNT ||
		device->flags != SPF_HOP_EVENT_FLAGS_V1 ||
		!device->device_event_id ||
		device->transition_before > device->transition_after ||
		device->transition_after > UINT64_MAX -
			session->request.transition_guard_samples)
		return -EBADMSG;
	if (session->have_last_event &&
		session->last_event.invalid_end > UINT64_MAX -
			session->request.dwell_samples)
		return -ERANGE;
	profile = &session->request.profiles[device->to_profile];
	if (device->fastlock_slot != profile->fastlock_slot ||
		device->actual_lo_frequency_hz != profile->lo_frequency_hz ||
		device->actual_if_offset_hz != session->request.if_offset_hz ||
		!center_matches_actual(profile->center_frequency_hz,
			device->actual_lo_frequency_hz,
			device->actual_if_offset_hz))
		return -EBADMSG;
	memset(event, 0, sizeof(*event));
	event->device = *device;
	/* A device-local software scheduler cannot begin an SPI fastlock recall on
	 * an exact sample edge.  Preserve the scheduled edge as invalid_start and
	 * conservatively include scheduler lateness in the invalid span. */
	event->invalid_start = session->have_last_event ?
		session->last_event.invalid_end + session->request.dwell_samples :
		device->transition_before;
	event->invalid_end = device->transition_after +
		session->request.transition_guard_samples;
	if (session->have_last_event) {
		if (event->invalid_start < session->last_event.invalid_end ||
			device->transition_before < event->invalid_start ||
			device->device_event_id <= session->last_device_event_id)
			return -ERANGE;
		/* The previous visit reached the requested capture envelope.  The
		 * provider was required to stop rather than activate another visit. */
		if (event->invalid_start >=
				session->status.startup_invalid_start &&
			event->invalid_start -
				session->status.startup_invalid_start >=
				session->request.capture_span_samples)
			return -ERANGE;
	} else if (device->event_sequence != 0 || device->dwell_index != 0) {
		return -EILSEQ;
	}
	return 0;
}

static void accept_event(struct spf_hop_session_v1 *session,
	const struct spf_hop_event_v1 *event)
{
	session->last_event = *event;
	session->last_device_event_id = event->device.device_event_id;
	session->have_last_event = 1;
	session->status.next_event_sequence++;
	session->status.visits_started++;
	session->status.active_profile = event->device.to_profile;
	if (event->device.event_sequence == 0) {
		session->status.startup_invalid_start = event->invalid_start;
		session->status.startup_invalid_end = event->invalid_end;
		session->status.first_counter = event->invalid_start;
	}
}

static int maybe_complete(struct spf_hop_session_v1 *session,
	uint64_t block_end)
{
	uint64_t final_counter;

	if (!session->have_last_event ||
		session->last_event.invalid_end > UINT64_MAX -
			session->request.dwell_samples)
		return session->have_last_event ?
			fail(session, SPF_HOP_REASON_PROTOCOL_ERROR, -EOVERFLOW) : 0;
	final_counter = session->last_event.invalid_end +
		session->request.dwell_samples;
	if (final_counter < session->status.startup_invalid_start ||
		final_counter - session->status.startup_invalid_start <
			session->request.capture_span_samples)
		return 0;
	if (block_end < final_counter)
		return 0;
	return restore(session, SPF_HOP_REASON_PLAN_COMPLETE,
		SPF_HOP_STATE_COMPLETED, final_counter);
}

int spf_hop_session_v1_on_block(struct spf_hop_session_v1 *session,
	uint64_t buffer_sequence, uint64_t first_sample, uint64_t block_end,
	struct spf_hop_sidecar_v1 *sidecar)
{
	struct spf_hop_device_event_v1 device_events[SPF_HOP_EVENT_CAPACITY];
	uint64_t dropped_events = 0;
	size_t event_count = 0;
	size_t i;
	int ret;

	if (!session || !sidecar || session->status.state != SPF_HOP_STATE_RUNNING ||
		first_sample >= block_end)
		return -EINVAL;
	if (session->have_last_block &&
		(buffer_sequence != session->status.last_block_sequence + 1 ||
		 first_sample != session->status.last_block_end))
		return fail(session, SPF_HOP_REASON_COUNTER_DISCONTINUITY, -EILSEQ);
	ret = session->ops->drain_events(session->device_context, device_events,
		SPF_HOP_EVENT_CAPACITY, &event_count, &dropped_events);
	ret = normalize_error(ret);
	if (ret)
		return fail(session, SPF_HOP_REASON_DEVICE_ERROR, ret);
	if (event_count > SPF_HOP_EVENT_CAPACITY)
		return fail(session, SPF_HOP_REASON_PROTOCOL_ERROR, -EOVERFLOW);
	session->status.device_dropped_events = dropped_events;
	if (dropped_events)
		return fail(session, SPF_HOP_REASON_EVENT_OVERFLOW, -EOVERFLOW);
	memset(sidecar, 0, sizeof(*sidecar));
	for (i = 0; i < event_count; i++) {
		/* The userspace scheduler reads only the public low-32 register.  The
		 * block timestamp is authoritative for its 64-bit epoch.  Kernel
		 * provider counters pass through the same mapping unchanged. */
		device_events[i].transition_before = extend_counter_near(first_sample,
			device_events[i].transition_before);
		device_events[i].transition_after = extend_counter_near(
			device_events[i].transition_before,
			device_events[i].transition_after);
		ret = validate_device_event(session, &device_events[i],
			&sidecar->events[i]);
		if (ret)
			return fail(session,
				ret == -EILSEQ ? SPF_HOP_REASON_EVENT_SEQUENCE :
				SPF_HOP_REASON_PROTOCOL_ERROR, ret);
		accept_event(session, &sidecar->events[i]);
	}
	session->status.last_block_sequence = buffer_sequence;
	session->status.last_block_end = block_end;
	session->have_last_block = 1;
	ret = maybe_complete(session, block_end);
	if (ret)
		return ret;
	sidecar->flags = session->status.flags;
	sidecar->session_id = session->request.session_id;
	sidecar->buffer_sequence = buffer_sequence;
	sidecar->block_first_sample = first_sample;
	sidecar->block_end_sample = block_end;
	sidecar->state = session->status.state;
	sidecar->terminal_reason = session->status.terminal_reason;
	sidecar->error_code = session->status.error_code;
	sidecar->event_count = (uint16_t)event_count;
	session->status.events_emitted += event_count;
	return 0;
}

int spf_hop_session_v1_cancel(struct spf_hop_session_v1 *session,
	uint16_t reason)
{
	if (!session ||
		(reason != SPF_HOP_REASON_CLIENT_CLOSE &&
		 reason != SPF_HOP_REASON_CLIENT_DISCONNECT))
		return -EINVAL;
	if (terminal(session))
		return session->status.restore_error;
	return restore(session, reason, SPF_HOP_STATE_CANCELLED, 0);
}

void spf_hop_session_v1_get_status(const struct spf_hop_session_v1 *session,
	struct spf_hop_status_v1 *status)
{
	if (session && status)
		*status = session->status;
}
