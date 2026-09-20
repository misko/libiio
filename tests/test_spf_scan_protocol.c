/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "spf-scan-protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct spf_scan_setup setup(void)
{
	struct spf_scan_setup value = {
		.session = UINT64_C(0x1122334455667788),
		.generation = 9,
		.seed = 10,
		.source_rate_hz = 15000000,
		.analog_bandwidth_hz = 12000000,
		.duration_ms = 300000,
		.dwell_ms = 240,
		.transition_budget_ms = 10,
		.maximum_revisit_ms = 3000,
		.feedback_age_ms = 1000,
		.application_delay_ms = 1000,
		.decay_ms = 5000,
		.maximum_boost = 3,
		.maximum_queue_bytes = UINT64_C(200000000),
		.maximum_queue_age_ms = 5000,
		.maximum_queue_visits = 50,
		.target_count = 3,
		.rx_mask = 1,
		.format = SPF_SCAN_FORMAT_CI16,
		.flags = SPF_SCAN_SETUP_FLAGS,
	};
	unsigned i;

	for (i = 0; i < 32; i++)
		value.analysis_digest[i] = (uint8_t)(i + 1);
	for (i = 0; i < value.target_count; i++) {
		value.targets[i].channel = 100 + i;
		value.targets[i].profile = i * 2;
		value.targets[i].frequency_hz = UINT64_C(2400000000) +
			UINT64_C(25000000) * i;
		value.targets[i].baseline_weight = i + 1;
		value.targets[i].profile_crc32 = UINT32_C(0x10203040) + i;
	}
	return value;
}

static void time_protocol(void)
{
	uint8_t wire[SPF_SCAN_TIME_BYTES];
	struct spf_scan_time_query q = {1, 2, 3}, decoded_q;
	struct spf_scan_time t = {
		.identity = {1, 2, 3}, .boot_id = {1}, .epoch = 7,
		.counter = UINT64_C(0x100000010), .monotonic_before_ns = 100,
		.monotonic_after_ns = 120, .sample_rate_hz = 20000000,
		.maximum_snapshot_age_ns = UINT64_MAX,
	}, decoded;
	unsigned i;
	assert(!spf_scan_time_query_encode(wire, sizeof(wire), &q));
	assert(!spf_scan_time_query_decode(&decoded_q, wire, SPF_SCAN_TIME_QUERY_BYTES));
	assert(!memcmp(&q, &decoded_q, sizeof(q)));
	q.request = 0;
	assert(spf_scan_time_query_encode(wire, sizeof(wire), &q) == -EINVAL);
	assert(!spf_scan_time_encode(wire, sizeof(wire), &t));
	assert(!spf_scan_time_decode(&decoded, wire, sizeof(wire)));
	assert(decoded.counter == t.counter && decoded.maximum_snapshot_age_ns == UINT64_MAX);
	for (i = 0; i < sizeof(wire); ++i) {
		wire[i] ^= 1;
		assert(spf_scan_time_decode(&decoded, wire, sizeof(wire)) < 0);
		wire[i] ^= 1;
	}
	t.monotonic_after_ns = 99;
	assert(spf_scan_time_encode(wire, sizeof(wire), &t) == -EINVAL);
}

static void corruption(void *wire, size_t bytes,
			int (*decode)(void *, const void *, size_t), size_t output_bytes)
{
	uint8_t original[SPF_SCAN_SETUP_BYTES];
	uint8_t output[sizeof(struct spf_scan_setup)];
	size_t i;

	assert(bytes <= sizeof(original) && output_bytes <= sizeof(output));
	memcpy(original, wire, bytes);
	for (i = 0; i < bytes; i++) {
		((uint8_t *)wire)[i] ^= UINT8_C(0x80);
		memset(output, 0xa5, output_bytes);
		assert(decode(output, wire, bytes) < 0);
		((uint8_t *)wire)[i] ^= UINT8_C(0x80);
	}
	assert(!memcmp(original, wire, bytes));
}

static int decode_caps(void *out, const void *wire, size_t bytes)
{ return spf_scan_caps_decode(out, wire, bytes); }
static int decode_setup(void *out, const void *wire, size_t bytes)
{ return spf_scan_setup_decode(out, wire, bytes); }
static int decode_visit(void *out, const void *wire, size_t bytes)
{ return spf_scan_visit_decode(out, wire, bytes); }
static int decode_feedback(void *out, const void *wire, size_t bytes)
{ return spf_scan_feedback_decode(out, wire, bytes); }
static int decode_ack(void *out, const void *wire, size_t bytes)
{ return spf_scan_ack_decode(out, wire, bytes); }
static int decode_terminal(void *out, const void *wire, size_t bytes)
{ return spf_scan_terminal_decode(out, wire, bytes); }

int main(int argc, char **argv)
{
	uint8_t wire[SPF_SCAN_SETUP_BYTES];
	struct spf_scan_caps caps, caps_out;
	struct spf_scan_setup request = setup(), request_out;
	struct spf_scan_visit_record visit = {
		.session = request.session, .generation = request.generation, .visit = 7,
		.selection_counter = UINT64_C(0x200000000),
		.transition_before = UINT64_C(0x200000001),
		.transition_after = UINT64_C(0x200000101),
		.valid_start = UINT64_C(0x200000201),
		.valid_end = UINT64_C(0x200003201),
		.frequency_hz = request.targets[1].frequency_hz,
		.iq_bytes = UINT64_C(0x3000) * 4,
		.analog_bandwidth_hz = request.analog_bandwidth_hz,
		.source_rate_hz = request.source_rate_hz,
		.target = 1, .profile = request.targets[1].profile,
		.result = SPF_VISIT_COMPLETE, .eligible_mask = 7,
		.effective_weight = SPF_SCAN_WEIGHT_ONE * 2,
		.profile_crc32 = request.targets[1].profile_crc32,
		.flags = SPF_SCAN_VISIT_FLAGS,
	}, visit_out;
	struct spf_scan_feedback feedback = {
		.session = request.session, .generation = request.generation,
		.sequence = 11, .visit = visit.visit,
		.valid_start = visit.valid_start, .valid_end = visit.valid_end,
		.target = visit.target, .outcome = SPF_SCAN_ACTIVE,
	}, feedback_out;
	struct spf_scan_ack ack = {
		.sequence = feedback.sequence, .source_visit = visit.visit,
		.first_visit = 8, .received_counter = visit.valid_end + 1,
		.application_counter = visit.valid_end + 2, .target = visit.target,
		.result = SPF_SCAN_APPLIED, .old_boost = 1, .new_boost = 3,
	}, ack_out;
	struct spf_scan_terminal terminal = {
		.session = request.session, .generation = request.generation,
		.final_counter = UINT64_C(0x300000000),
		.restore_before = UINT64_C(0x300000001),
		.restore_after = UINT64_C(0x300000101),
		.planned = 10, .delivered = 8, .skipped = 1, .invalid = 1,
		.iq_bytes = 123456, .state = SPF_SCAN_TERMINAL_COMPLETED,
		.reason = 1, .flags = SPF_SCAN_TERMINAL_FLAGS,
	}, terminal_out;
	FILE *golden;

	memcpy(feedback.analysis_digest, request.analysis_digest, 32);
	spf_scan_caps_default(&caps);
	assert(spf_scan_caps_encode(wire, sizeof(wire), &caps) == 0);
	assert(spf_scan_caps_decode(&caps_out, wire, SPF_SCAN_CAPS_BYTES) == 0);
	assert(!memcmp(&caps, &caps_out, sizeof(caps)));
	corruption(wire, SPF_SCAN_CAPS_BYTES, decode_caps, sizeof(caps_out));

	assert(spf_scan_setup_encode(wire, sizeof(wire), &request) == 0);
	assert(spf_scan_setup_decode(&request_out, wire, SPF_SCAN_SETUP_BYTES) == 0);
	assert(!memcmp(&request, &request_out, sizeof(request)));
	corruption(wire, SPF_SCAN_SETUP_BYTES, decode_setup, sizeof(request_out));
	request.targets[1].profile = request.targets[0].profile;
	assert(spf_scan_setup_encode(wire, sizeof(wire), &request) == -EINVAL);
	request = setup();

	assert(spf_scan_visit_encode(wire, sizeof(wire), &visit) == 0);
	assert(spf_scan_visit_decode(&visit_out, wire, SPF_SCAN_VISIT_BYTES) == 0);
	assert(!memcmp(&visit, &visit_out, sizeof(visit)));
	corruption(wire, SPF_SCAN_VISIT_BYTES, decode_visit, sizeof(visit_out));
	visit.iq_bytes = (visit.valid_end - visit.valid_start) * 8;
	assert(spf_scan_visit_encode(wire, sizeof(wire), &visit) == 0);
	assert(spf_scan_visit_decode(&visit_out, wire, SPF_SCAN_VISIT_BYTES) == 0);
	assert(!memcmp(&visit, &visit_out, sizeof(visit)));
	visit.iq_bytes = (visit.valid_end - visit.valid_start) * 6;
	assert(spf_scan_visit_encode(wire, sizeof(wire), &visit) == -EINVAL);
	visit.iq_bytes = (visit.valid_end - visit.valid_start) * 4;

	assert(spf_scan_feedback_encode(wire, sizeof(wire), &feedback) == 0);
	assert(spf_scan_feedback_decode(&feedback_out, wire,
					 SPF_SCAN_FEEDBACK_BYTES) == 0);
	assert(!memcmp(&feedback, &feedback_out, sizeof(feedback)));
	corruption(wire, SPF_SCAN_FEEDBACK_BYTES, decode_feedback,
		   sizeof(feedback_out));

	assert(spf_scan_ack_encode(wire, sizeof(wire), &ack) == 0);
	assert(spf_scan_ack_decode(&ack_out, wire, SPF_SCAN_ACK_BYTES) == 0);
	assert(!memcmp(&ack, &ack_out, sizeof(ack)));
	corruption(wire, SPF_SCAN_ACK_BYTES, decode_ack, sizeof(ack_out));

	assert(spf_scan_terminal_encode(wire, sizeof(wire), &terminal) == 0);
	assert(spf_scan_terminal_decode(&terminal_out, wire,
					 SPF_SCAN_TERMINAL_BYTES) == 0);
	assert(!memcmp(&terminal, &terminal_out, sizeof(terminal)));
	corruption(wire, SPF_SCAN_TERMINAL_BYTES, decode_terminal,
		   sizeof(terminal_out));

	if (argc == 2) {
		golden = fopen(argv[1], "wb");
		assert(golden);
		assert(spf_scan_setup_encode(wire, sizeof(wire), &request) == 0);
		assert(fwrite(wire, 1, SPF_SCAN_SETUP_BYTES, golden) ==
		       SPF_SCAN_SETUP_BYTES);
		assert(fclose(golden) == 0);
	}
	time_protocol();
	if (argc == 3 && !strcmp(argv[1], "--time-golden")) {
		struct spf_scan_time t = {
			.identity = {1, 2, 3}, .boot_id = {1}, .epoch = 7,
			.counter = UINT64_C(0x100000010), .monotonic_before_ns = 100,
			.monotonic_after_ns = 120, .sample_rate_hz = 20000000,
			.maximum_snapshot_age_ns = UINT64_MAX,
		};
		golden = fopen(argv[2], "wb");
		assert(golden);
		assert(!spf_scan_time_encode(wire, sizeof(wire), &t));
		assert(fwrite(wire, 1, SPF_SCAN_TIME_BYTES, golden) == SPF_SCAN_TIME_BYTES);
		assert(!fclose(golden));
	}
	puts("PASS: scan protocol round trips, strict reserved fields and per-byte CRC rejection");
	return 0;
}
