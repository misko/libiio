/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-session.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

struct fake_device {
	struct spf_hop_device_event_v1 events[8];
	size_t event_count;
	size_t next_event;
	uint64_t dropped_events;
	struct spf_hop_restore_receipt_v1 receipt;
	int submit_error;
	int drain_error;
	int restore_error;
	unsigned int submit_calls;
	unsigned int restore_calls;
};

static struct spf_hop_request_v1 make_request(void)
{
	struct spf_hop_request_v1 request = {0};
	unsigned int i;

	request.required_features = SPF_HOP_REQUIRED_FEATURES_V1;
	request.flags = SPF_HOP_REQUEST_FLAGS_V1;
	request.session_id = UINT64_C(77);
	request.sample_rate_hz = UINT64_C(1000);
	request.rf_bandwidth_hz = UINT64_C(1000);
	request.if_offset_hz = INT64_C(10);
	request.dwell_samples = UINT64_C(100);
	request.transition_guard_samples = UINT64_C(10);
	request.dwell_count = UINT64_C(3);
	request.capture_span_samples = UINT64_C(250);
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		request.profiles[i].profile_id = (uint8_t)i;
		request.profiles[i].fastlock_slot = (uint8_t)(7 - i);
		request.profiles[i].lo_frequency_hz = UINT64_C(1000000) + i * 1000;
		request.profiles[i].center_frequency_hz =
			request.profiles[i].lo_frequency_hz + 10;
		request.profiles[i].profile_crc32 = UINT32_C(0xa0000000) + i;
	}
	return request;
}

static struct spf_hop_device_event_v1 make_event(
	const struct spf_hop_request_v1 *request, uint64_t index,
	uint64_t before, uint64_t after)
{
	struct spf_hop_device_event_v1 event = {0};
	uint8_t profile = (uint8_t)(index % SPF_HOP_PROFILE_COUNT);

	event.event_sequence = index;
	event.dwell_index = index;
	event.transition_before = before;
	event.transition_after = after;
	event.actual_lo_frequency_hz = request->profiles[profile].lo_frequency_hz;
	event.actual_if_offset_hz = request->if_offset_hz;
	event.device_event_id = UINT64_C(100) + index;
	event.from_profile = index ? (uint8_t)((index - 1) %
		SPF_HOP_PROFILE_COUNT) : SPF_HOP_PROFILE_NONE;
	event.to_profile = profile;
	event.kind = index ? SPF_HOP_EVENT_RETUNE : SPF_HOP_EVENT_STARTUP;
	event.flags = SPF_HOP_EVENT_FLAGS_V1;
	event.fastlock_slot = request->profiles[profile].fastlock_slot;
	return event;
}

static int fake_submit(void *opaque, const struct spf_hop_request_v1 *request)
{
	struct fake_device *device = opaque;

	assert(request->profiles[7].profile_id == 7);
	device->submit_calls++;
	return device->submit_error;
}

static int fake_drain(void *opaque, struct spf_hop_device_event_v1 *events,
	size_t capacity, size_t *event_count, uint64_t *dropped_events)
{
	struct fake_device *device = opaque;

	if (device->drain_error)
		return device->drain_error;
	*event_count = 0;
	if (device->next_event < device->event_count && capacity) {
		events[0] = device->events[device->next_event++];
		*event_count = 1;
	}
	*dropped_events = device->dropped_events;
	return 0;
}

static int fake_restore(void *opaque, uint16_t reason,
	struct spf_hop_restore_receipt_v1 *receipt)
{
	struct fake_device *device = opaque;

	assert(reason != SPF_HOP_REASON_NONE);
	device->restore_calls++;
	*receipt = device->receipt;
	return device->restore_error;
}

static const struct spf_hop_device_ops_v1 fake_ops = {
	.submit_plan = fake_submit,
	.drain_events = fake_drain,
	.cancel_restore = fake_restore,
};

static void init_receipt(struct fake_device *device, uint64_t before)
{
	device->receipt.transition_before = before;
	device->receipt.transition_after = before + 2;
	device->receipt.restored_lo_frequency_hz = UINT64_C(900000);
	device->receipt.restored_profile = SPF_HOP_PROFILE_NONE;
	device->receipt.flags = SPF_HOP_EVENT_FLAGS_V1;
}

static void test_complete_valid_visits(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_session_v1 session;
	struct spf_hop_sidecar_v1 sidecar;
	struct fake_device device = {0};

	device.events[0] = make_event(&request, 0, 1000, 1002); /* end 1012 */
	/* The scheduled invalid starts are 1112 and 1227.  Later before-counters
	 * truthfully account for software-scheduler latency as invalid samples. */
	device.events[1] = make_event(&request, 1, 1115, 1117); /* end 1127 */
	device.events[2] = make_event(&request, 2, 1231, 1233); /* end 1243 */
	device.event_count = 3;
	/* The terminal block is observed after the exact final dwell.  Restoration
	 * may begin later; the intervening samples are deliberately unassigned. */
	init_receipt(&device, 1375);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(device.submit_calls == 1);
	assert(spf_hop_session_v1_on_block(&session, 0, 900, 1100,
		&sidecar) == 0);
	assert(sidecar.event_count == 1 && sidecar.events[0].invalid_end == 1012);
	assert(spf_hop_session_v1_on_block(&session, 1, 1100, 1200,
		&sidecar) == 0);
	assert(sidecar.events[0].invalid_start - session.status.startup_invalid_end ==
		request.dwell_samples);
	assert(spf_hop_session_v1_on_block(&session, 2, 1200, 1300,
		&sidecar) == 0);
	assert(sidecar.events[0].invalid_start == 1227);
	assert(sidecar.events[0].device.transition_before == 1231);
	assert(spf_hop_session_v1_on_block(&session, 3, 1300, 1400,
		&sidecar) == 0);
	assert(sidecar.state == SPF_HOP_STATE_COMPLETED);
	assert(sidecar.terminal_reason == SPF_HOP_REASON_PLAN_COMPLETE);
	assert(session.status.final_counter == 1343);
	assert(session.status.restore_before == 1375);
	assert(session.status.final_counter - session.last_event.invalid_end ==
		request.dwell_samples);
	assert(session.status.visits_started == 3);
	assert(device.restore_calls == 1);
}

static void test_event_sequence_fails_closed(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_session_v1 session;
	struct spf_hop_sidecar_v1 sidecar;
	struct fake_device device = {0};

	device.events[0] = make_event(&request, 1, 1000, 1002);
	device.event_count = 1;
	init_receipt(&device, 1005);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(spf_hop_session_v1_on_block(&session, 0, 900, 1100,
		&sidecar) == -EILSEQ);
	assert(session.status.state == SPF_HOP_STATE_FAILED);
	assert(session.status.terminal_reason == SPF_HOP_REASON_EVENT_SEQUENCE);
	assert(session.status.flags & SPF_HOP_STATUS_CONTINUITY_FAULT);
	assert(device.restore_calls == 1);
}

static void test_restore_cannot_truncate_final_dwell(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_session_v1 session;
	struct spf_hop_sidecar_v1 sidecar;
	struct fake_device device = {0};

	device.events[0] = make_event(&request, 0, 1000, 1002);
	device.events[1] = make_event(&request, 1, 1115, 1117);
	device.events[2] = make_event(&request, 2, 1231, 1233);
	device.event_count = 3;
	/* The exact terminal counter is 1343.  A provider must not restore early
	 * and silently shorten the final target-assigned dwell. */
	init_receipt(&device, 1342);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(spf_hop_session_v1_on_block(&session, 0, 900, 1100,
		&sidecar) == 0);
	assert(spf_hop_session_v1_on_block(&session, 1, 1100, 1200,
		&sidecar) == 0);
	assert(spf_hop_session_v1_on_block(&session, 2, 1200, 1300,
		&sidecar) == 0);
	assert(spf_hop_session_v1_on_block(&session, 3, 1300, 1400,
		&sidecar) == -ERANGE);
	assert(session.status.state == SPF_HOP_STATE_FAILED);
	assert(session.status.terminal_reason == SPF_HOP_REASON_RESTORE_ERROR);
	assert(!(session.status.flags & SPF_HOP_STATUS_RESTORE_SUCCEEDED));
}

static void test_bad_valid_dwell_fails_closed(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_session_v1 session;
	struct spf_hop_sidecar_v1 sidecar;
	struct fake_device device = {0};

	device.events[0] = make_event(&request, 0, 1000, 1002);
	device.events[1] = make_event(&request, 1, 1111, 1114); /* before deadline */
	device.event_count = 2;
	init_receipt(&device, 1115);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(spf_hop_session_v1_on_block(&session, 0, 900, 1050,
		&sidecar) == 0);
	assert(spf_hop_session_v1_on_block(&session, 1, 1050, 1150,
		&sidecar) == -ERANGE);
	assert(session.status.terminal_reason == SPF_HOP_REASON_PROTOCOL_ERROR);
	assert(device.restore_calls == 1);
}

static void test_counter_gap_and_overflow_fail_closed(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_session_v1 session;
	struct spf_hop_sidecar_v1 sidecar;
	struct fake_device device = {0};

	device.events[0] = make_event(&request, 0, 1000, 1002);
	device.event_count = 1;
	init_receipt(&device, 1200);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(spf_hop_session_v1_on_block(&session, 0, 900, 1050,
		&sidecar) == 0);
	assert(spf_hop_session_v1_on_block(&session, 1, 1051, 1150,
		&sidecar) == -EILSEQ);
	assert(session.status.terminal_reason ==
		SPF_HOP_REASON_COUNTER_DISCONTINUITY);

	memset(&device, 0, sizeof(device));
	device.dropped_events = 1;
	init_receipt(&device, 1000);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(spf_hop_session_v1_on_block(&session, 0, 900, 1050,
		&sidecar) == -EOVERFLOW);
	assert(session.status.terminal_reason == SPF_HOP_REASON_EVENT_OVERFLOW);
	assert(session.status.flags & SPF_HOP_STATUS_DEVICE_EVENT_OVERFLOW);
}

static void test_cancel_and_restore_once(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_session_v1 session;
	struct spf_hop_status_v1 status = {0};
	struct spf_hop_status_v1 decoded = {0};
	struct fake_device device = {0};
	uint8_t wire[SPF_HOP_STATUS_BYTES];

	init_receipt(&device, 1234);
	assert(spf_hop_session_v1_init(&session, &request, &fake_ops, &device) == 0);
	assert(spf_hop_session_v1_start(&session) == 0);
	assert(spf_hop_session_v1_cancel(&session,
		SPF_HOP_REASON_CLIENT_CLOSE) == 0);
	assert(session.status.state == SPF_HOP_STATE_CANCELLED);
	assert(session.status.flags & SPF_HOP_STATUS_RESTORE_SUCCEEDED);
	assert(device.restore_calls == 1);
	spf_hop_session_v1_get_status(&session, &status);
	assert(status.state == SPF_HOP_STATE_CANCELLED);
	assert(status.terminal_reason == SPF_HOP_REASON_CLIENT_CLOSE);
	assert(status.final_counter == 1234);
	assert(status.restore_before == 1234);
	assert(status.restore_after == 1236);
	assert(spf_hop_status_v1_encode(wire, sizeof(wire), &status) == 0);
	assert(spf_hop_status_v1_decode(&decoded, wire, sizeof(wire)) == 0);
	assert(decoded.state == SPF_HOP_STATE_CANCELLED);
	assert(decoded.flags & SPF_HOP_STATUS_RESTORE_SUCCEEDED);
	assert(decoded.restore_before == 1234);
	assert(spf_hop_session_v1_cancel(&session,
		SPF_HOP_REASON_CLIENT_CLOSE) == 0);
	assert(device.restore_calls == 1);
}

int main(void)
{
	test_complete_valid_visits();
	test_restore_cannot_truncate_final_dwell();
	test_event_sequence_fails_closed();
	test_bad_valid_dwell_fails_closed();
	test_counter_gap_and_overflow_fail_closed();
	test_cancel_and_restore_once();
	return 0;
}
