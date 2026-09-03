/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-protocol.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static struct spf_hop_request_v1 make_request(void)
{
	struct spf_hop_request_v1 request = {0};
	unsigned int i;

	request.required_features = SPF_HOP_REQUIRED_FEATURES_V1;
	request.flags = SPF_HOP_REQUEST_FLAGS_V1;
	request.session_id = UINT64_C(0x0102030405060708);
	request.sample_rate_hz = UINT64_C(5000000);
	request.rf_bandwidth_hz = UINT64_C(5000000);
	request.if_offset_hz = INT64_C(1250000);
	request.dwell_samples = UINT64_C(600000);
	request.transition_guard_samples = UINT64_C(5000);
	request.dwell_count = UINT64_C(2500);
	request.capture_span_samples = UINT64_C(1500000000);
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		request.profiles[i].profile_id = (uint8_t)i;
		request.profiles[i].fastlock_slot = (uint8_t)i;
		request.profiles[i].lo_frequency_hz =
			UINT64_C(10700000000) + i * UINT64_C(250000000);
		request.profiles[i].center_frequency_hz =
			request.profiles[i].lo_frequency_hz + UINT64_C(1250000);
		request.profiles[i].profile_crc32 = UINT32_C(0x10203040) + i;
	}
	return request;
}

static struct spf_hop_event_v1 make_event(void)
{
	struct spf_hop_event_v1 event = {0};

	event.device.event_sequence = 0;
	event.device.dwell_index = 0;
	event.device.transition_before = UINT64_C(1000);
	event.device.transition_after = UINT64_C(1004);
	event.invalid_start = UINT64_C(998);
	event.invalid_end = UINT64_C(1014);
	event.device.from_profile = SPF_HOP_PROFILE_NONE;
	event.device.to_profile = 0;
	event.device.kind = SPF_HOP_EVENT_STARTUP;
	event.device.flags = SPF_HOP_EVENT_FLAGS_V1;
	event.device.fastlock_slot = 0;
	event.device.actual_lo_frequency_hz = UINT64_C(10700000000);
	event.device.actual_if_offset_hz = INT64_C(1250000);
	event.device.device_event_id = UINT64_C(44);
	return event;
}

static void test_request_round_trip_and_offsets(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_request_v1 decoded;
	uint8_t wire[SPF_HOP_REQUEST_BYTES];
	unsigned int i;

	assert(spf_hop_request_v1_encode(wire, sizeof(wire), &request) == 0);
	assert(!memcmp(wire, "HOPR\001\000 \001", 8));
	assert(wire[72] == 8 && wire[73] == 0);
	assert(wire[76] == 80 && wire[77] == 0);
	assert(wire[78] == 160 && wire[79] == 0);
	assert(wire[88] == 0x00 && wire[89] == 0x2f);
	assert(wire[96] == 0 && wire[97] == 0);
	assert(wire[120] == 1 && wire[121] == 1);
	assert(spf_hop_request_v1_decode(&decoded, wire, sizeof(wire)) == 0);
	assert(decoded.session_id == request.session_id);
	assert(decoded.capture_span_samples == request.capture_span_samples);
	assert(decoded.if_offset_hz == request.if_offset_hz);
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		assert(decoded.profiles[i].profile_id == i);
		assert(decoded.profiles[i].fastlock_slot == i);
		assert(decoded.profiles[i].center_frequency_hz ==
			request.profiles[i].center_frequency_hz);
		assert(decoded.profiles[i].profile_crc32 ==
			request.profiles[i].profile_crc32);
	}
}

static void test_request_rejections(void)
{
	struct spf_hop_request_v1 request = make_request();
	struct spf_hop_request_v1 decoded;
	uint8_t wire[SPF_HOP_REQUEST_BYTES];

	assert(spf_hop_request_v1_encode(wire, sizeof(wire), &request) == 0);
	wire[4] = 2;
	assert(spf_hop_request_v1_decode(&decoded, wire, sizeof(wire)) ==
		-EPROTONOSUPPORT);
	wire[4] = 1;
	wire[80] = 1;
	assert(spf_hop_request_v1_decode(&decoded, wire, sizeof(wire)) == -EBADMSG);
	wire[80] = 0;
	wire[121] = 0;
	assert(spf_hop_request_v1_decode(&decoded, wire, sizeof(wire)) == -EINVAL);
	assert(spf_hop_request_v1_decode(&decoded, wire, sizeof(wire) - 1) ==
		-EMSGSIZE);
	request.capture_span_samples = request.dwell_samples - 1;
	assert(spf_hop_request_v1_encode(wire, sizeof(wire), &request) == -EINVAL);
	request = make_request();
	request.profiles[7].fastlock_slot = 0;
	assert(spf_hop_request_v1_encode(wire, sizeof(wire), &request) == -EINVAL);
	request = make_request();
	request.required_features |= UINT32_C(0x80);
	assert(spf_hop_request_v1_encode(wire, sizeof(wire), &request) == -EINVAL);
}

static void test_sidecar_round_trip(void)
{
	struct spf_hop_sidecar_v1 sidecar = {0};
	struct spf_hop_sidecar_v1 decoded;
	uint8_t wire[SPF_HOP_SIDECAR_MAX_BYTES];
	int bytes;

	sidecar.flags = SPF_HOP_STATUS_RESTORE_REQUIRED;
	sidecar.session_id = UINT64_C(1234);
	sidecar.buffer_sequence = UINT64_C(7);
	sidecar.block_first_sample = UINT64_C(900);
	sidecar.block_end_sample = UINT64_C(1100);
	sidecar.state = SPF_HOP_STATE_RUNNING;
	sidecar.event_count = 1;
	sidecar.events[0] = make_event();
	bytes = spf_hop_sidecar_v1_encode(wire, sizeof(wire), &sidecar);
	assert(bytes == 144);
	assert(!memcmp(wire, "HOPS\001\000@\000", 8));
	assert(wire[8] == 144 && wire[20] == 1 && wire[22] == 8);
	assert(wire[64 + 48] == SPF_HOP_PROFILE_NONE);
	assert(wire[64 + 56] == 0x00);
	assert(spf_hop_sidecar_v1_decode(&decoded, wire, (size_t)bytes) == 0);
	assert(decoded.event_count == 1);
	assert(decoded.events[0].device.actual_if_offset_hz == INT64_C(1250000));
	assert(decoded.events[0].invalid_end == UINT64_C(1014));
	assert(decoded.events[0].invalid_start == UINT64_C(998));
	wire[64 + 54] = 1;
	assert(spf_hop_sidecar_v1_decode(&decoded, wire, (size_t)bytes) == -EBADMSG);
}

static void test_status_round_trip(void)
{
	struct spf_hop_status_v1 status = {0};
	struct spf_hop_status_v1 decoded;
	uint8_t wire[SPF_HOP_STATUS_BYTES];

	status.state = SPF_HOP_STATE_COMPLETED;
	status.terminal_reason = SPF_HOP_REASON_PLAN_COMPLETE;
	status.flags = SPF_HOP_STATUS_RESTORE_REQUIRED |
		SPF_HOP_STATUS_RESTORE_ATTEMPTED |
		SPF_HOP_STATUS_RESTORE_SUCCEEDED |
		SPF_HOP_STATUS_TERMINAL;
	status.session_id = UINT64_C(9);
	status.planned_dwells = UINT64_C(2500);
	status.visits_started = UINT64_C(2498);
	status.events_emitted = UINT64_C(2498);
	status.next_event_sequence = UINT64_C(2498);
	status.final_counter = UINT64_C(1500010100);
	status.restore_before = status.final_counter;
	status.restore_after = status.final_counter + 4;
	status.restored_lo_frequency_hz = UINT64_C(10700000000);
	status.active_profile = 1;
	status.restored_profile = SPF_HOP_PROFILE_NONE;
	assert(spf_hop_status_v1_encode(wire, sizeof(wire), &status) == 0);
	assert(!memcmp(wire, "HOPT\001\000\240\000", 8));
	assert(spf_hop_status_v1_decode(&decoded, wire, sizeof(wire)) == 0);
	assert(decoded.visits_started == status.visits_started);
	assert(decoded.final_counter == status.final_counter);
	wire[152] = 1;
	assert(spf_hop_status_v1_decode(&decoded, wire, sizeof(wire)) == -EBADMSG);
}

int main(void)
{
	test_request_round_trip_and_offsets();
	test_request_rejections();
	test_sidecar_round_trip();
	test_status_round_trip();
	return 0;
}
