/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "adi-rx-counter.h"
#include "spf-scan-session.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint64_t mock_now;
static uint64_t configured_frequency[8];
static unsigned released_blocks;
static int fail_recall;
static int fail_release;
static unsigned recall_count;
static uint32_t reject_rate, acquired_rate;
static uint64_t recall_before_offset = 100;
static uint64_t recall_after_offset = 1000;

static int mock_ioctl(int fd, unsigned long request, void *argument)
{
	assert(fd == 29);
	if (request == ADI_RX_COUNTER_IOC_DIAG_CONTEXT ||
	    request == ADI_RX_COUNTER_IOC_GET_DIAGNOSTICS) {
		errno = ENOTTY;
		return -1;
	}
	if (request == ADI_RX_COUNTER_IOC_GET_SCAN_CAPS) {
		struct adi_rx_counter_scan_caps *caps = argument;

		*caps = (struct adi_rx_counter_scan_caps) {
			.magic = ADI_RX_COUNTER_MAGIC,
			.version = ADI_RX_COUNTER_SCAN_VERSION,
			.size = sizeof(*caps),
			.features = ADI_RX_COUNTER_SCAN_FEATURES,
			.maximum_profiles = 8,
			.source_counter_bits = 32,
			.frequency_resolution_hz = 2,
		};
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_CONFIGURE_SCAN) {
		const struct adi_rx_counter_scan_config *config = argument;
		unsigned i;

		for (i = 0; i < 8; i++)
			configured_frequency[i] = config->profiles[i].frequency_hz;
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_ACQUIRE) {
		const struct adi_rx_counter_request *acquire = argument;
		acquired_rate = acquire->sample_rate_hz;
		if (acquired_rate == reject_rate) {
			errno = ERANGE;
			return -1;
		}
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_RECALL) {
		struct adi_rx_counter_scan_recall *recall = argument;

		recall_count++;
		if (fail_recall) {
			errno = EIO;
			return -1;
		}
		assert(recall->profile < 8 && configured_frequency[recall->profile]);
		recall->frequency_hz = configured_frequency[recall->profile];
		recall->profile_crc32 = UINT32_C(0x90000000) + recall->profile;
		recall->counter_before = (uint32_t)(mock_now + recall_before_offset);
		recall->counter_after = (uint32_t)(mock_now + recall_after_offset);
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_RELEASE_SCAN) {
		struct adi_rx_counter_scan_release *release = argument;
		if (fail_release) {
			errno = EREMOTEIO;
			return -1;
		}

		release->frequency_hz = UINT64_C(915000000);
		release->counter_before = (uint32_t)(mock_now + 10);
		release->counter_after = (uint32_t)(mock_now + 20);
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_SCAN_SNAPSHOT) {
		struct adi_rx_counter_scan_snapshot *snapshot = argument;

		snapshot->counter = (uint32_t)mock_now;
		return 0;
	}
	assert(!"unexpected ioctl");
	return -1;
}

static int release_block(void *context, uintptr_t token)
{
	(void)context;
	assert(token);
	released_blocks++;
	return 0;
}

static struct spf_scan_setup setup(void)
{
	struct spf_scan_setup value = {
		.session = 1, .generation = 2, .seed = 3,
		.source_rate_hz = 10000000, .analog_bandwidth_hz = 8000000,
		.duration_ms = 2000, .dwell_ms = 20, .transition_budget_ms = 10,
		.maximum_revisit_ms = 100, .feedback_age_ms = 1000,
		.application_delay_ms = 1000, .decay_ms = 5000,
		.maximum_boost = 3, .maximum_queue_bytes = 200000000,
		.maximum_queue_age_ms = 5000, .maximum_queue_visits = 8,
		.target_count = 2, .rx_mask = 1, .format = SPF_SCAN_FORMAT_CI16,
		.flags = SPF_SCAN_SETUP_FLAGS,
		.targets = {
			{ 10, 1, UINT64_C(2400000000), 1, UINT32_C(0x11111111) },
			{ 11, 3, UINT64_C(2450000000), 1, UINT32_C(0x33333333) },
		},
	};
	unsigned i;

	for (i = 0; i < 32; i++)
		value.analysis_digest[i] = (uint8_t)(i + 1);
	return value;
}

static void feed_three(struct spf_scan_session *session, uint64_t first,
			       uintptr_t *token)
{
	static uint32_t data;
	unsigned i;

	for (i = 0; i < 3; i++) {
		assert(spf_scan_session_feed(session, (*token)++, &data,
					     first + i * 100000, 100000) == 0);
	}
}

static struct spf_scan_session_output take_complete(
	struct spf_scan_session *session, uint64_t visit)
{
	struct spf_scan_session_output output;
	uint64_t bytes = 0;
	unsigned i;

	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == visit);
	assert(output.record.result == SPF_VISIT_COMPLETE);
	for (i = 0; i < output.slice_count; i++)
		bytes += output.slices[i].bytes;
	assert(bytes == 800000 && output.record.iq_bytes == bytes);
	assert(spf_scan_session_complete_output(session, visit) == 0);
	return output;
}

static struct spf_scan_session_output take_complete_bytes(
	struct spf_scan_session *session, uint64_t visit, uint64_t expected_bytes)
{
	struct spf_scan_session_output output;
	uint64_t bytes = 0;
	unsigned i;

	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == visit);
	assert(output.record.result == SPF_VISIT_COMPLETE);
	for (i = 0; i < output.slice_count; i++)
		bytes += output.slices[i].bytes;
	assert(bytes == expected_bytes && output.record.iq_bytes == bytes);
	assert(spf_scan_session_complete_output(session, visit) == 0);
	return output;
}

static void feed_blocks(struct spf_scan_session *session, uint64_t first,
	uintptr_t *token, unsigned count, uint32_t samples)
{
	static uint32_t data;
	unsigned i;

	for (i = 0; i < count; i++)
		assert(spf_scan_session_feed(session, (*token)++, &data,
					     first + (uint64_t)i * samples,
					     samples) == 0);
}

static void test_v4_fixed_360_ms_visit_is_captured(void)
{
	const uint64_t base = UINT64_C(0x1afff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 64, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 8, .drain_bytes_per_second = 1000000000,
		.release_block = release_block,
	};
	struct spf_scan_setup request = setup();
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_choice choice;
	struct spf_scan_session_output output;
	uint64_t boundary;
	uintptr_t token = 40;

	request.protocol_version = SPF_SCAN_FIXED_DWELL_VERSION;
	request.source_rate_hz = 2500000;
	request.analog_bandwidth_hz = 2000000;
	request.rx_mask = SPF_SCAN_RX1_RX2;
	request.dwell_ms = 360;
	request.duration_ms = 1000;
	request.transition_budget_ms = 10;
	request.maximum_revisit_ms = 1000;
	memset(configured_frequency, 0, sizeof(configured_frequency));
	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	assert(spf_scan_session_schedule(session, base, base, &choice) == 0);
	assert(choice.dwell_ms == 360);
	feed_blocks(session, base, &token, 10, 100000);
	assert(spf_scan_session_next_boundary(session, &boundary) == 0);
	mock_now = boundary;
	assert(spf_scan_session_stop(session, mock_now) == 0);
	output = take_complete_bytes(session, 0, UINT64_C(900000) * 8);
	assert(output.record.protocol_version == SPF_SCAN_FIXED_DWELL_VERSION);
	assert(output.record.valid_end - output.record.valid_start == 900000);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_three_visits_feedback_and_early_restore(void)
{
	const uint64_t base = UINT64_C(0x1ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4,
		.drain_bytes_per_second = 60000000, .release_block = release_block,
	};
	struct spf_scan_feedback feedback;
	struct spf_scan_terminal terminal;
	struct spf_scan_session_output first;
	struct spf_scan_choice choice[3];
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_ack ack;
	struct spf_scan_setup request = setup();
	uintptr_t token = 1;

	memset(configured_frequency, 0, sizeof(configured_frequency));
	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	assert(spf_scan_session_schedule(session, base, base, &choice[0]) == 0);
	{
		uint64_t boundary;

		assert(spf_scan_session_next_boundary(session, &boundary) == 0);
		assert(boundary == choice[0].selection_counter + 300000);
	}
	feed_three(session, base, &token);
	mock_now = base + 300000;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice[1]) == 0);
	first = take_complete(session, 0);
	feedback = (struct spf_scan_feedback) {
		.session = request.session, .generation = request.generation,
		.sequence = 1, .visit = 0, .valid_start = first.record.valid_start,
		.valid_end = first.record.valid_end, .target = first.record.target,
		.outcome = SPF_SCAN_ACTIVE,
	};
	memcpy(feedback.analysis_digest, request.analysis_digest, 32);
	assert(spf_scan_session_feedback(session, &feedback,
					 base + 400000) == SPF_SCAN_ACCEPTED);
	feed_three(session, base + 300000, &token);
	mock_now = base + 600000;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice[2]) == 0);
	assert(spf_scan_session_take_ack(session, &ack) == 0);
	assert(ack.sequence == 1 && ack.result == SPF_SCAN_APPLIED && ack.first_visit == 2);
	(void)take_complete(session, 1);
	feed_three(session, base + 600000, &token);
	mock_now = base + 900000;
	assert(spf_scan_session_stop(session, mock_now) == 0);
	/* Restoration is independent of draining the final visit. */
	assert(radio.released);
	(void)take_complete(session, 2);
	assert(spf_scan_session_terminal(session, &terminal) == 0);
	assert(terminal.state == SPF_SCAN_TERMINAL_COMPLETED);
	assert(terminal.planned == 3 && terminal.delivered == 3 && !terminal.skipped &&
	       !terminal.invalid && !terminal.cancelled && terminal.iq_bytes == 2400000);
	assert(terminal.restore_before >= terminal.final_counter);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_recall_failure_cancels_and_restores(void)
{
	const uint64_t base = UINT64_C(0x2ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4,
		.drain_bytes_per_second = 60000000, .release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();

	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	fail_recall = 1;
	assert(spf_scan_session_schedule(session, base, base, &choice) == -EIO);
	fail_recall = 0;
	assert(radio.released);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == 0 && output.record.result == SPF_VISIT_CANCELLED);
	assert(output.record.profile_crc32 == request.targets[output.record.target].profile_crc32);
	assert(output.record.transition_before == base && output.record.transition_after == base);
	assert(output.record.valid_start == base && output.record.valid_end == base);
	assert(spf_scan_session_complete_output(session, 0) == 0);
	assert(spf_scan_session_terminal(session, &terminal) == 0);
	assert(terminal.state == SPF_SCAN_TERMINAL_FAILED && terminal.error == -EIO);
	assert(terminal.cancelled == 1 && terminal.planned == 1);
	{
		char diagnostics[SPF_SCAN_DIAG_MAX_BYTES];
		assert(spf_scan_session_diagnostics(session, 1, 2, diagnostics, sizeof(diagnostics)) > 0);
		assert(strstr(diagnostics, "\"first_error\":-5"));
		assert(strstr(diagnostics, "\"failure_stage\":1"));
		assert(strstr(diagnostics, "\"restoration_error\":0"));
		assert(spf_scan_session_diagnostics(session, 9, 2, diagnostics, sizeof(diagnostics)) == -ESTALE);
		assert(spf_scan_session_diagnostics(session, 1, 2, diagnostics, 10) == -ENOSPC);
	}
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_first_failure_survives_restoration_and_producer_errors(void)
{
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4, .drain_bytes_per_second = 60000000,
		.release_block = release_block,
	};
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();
	struct spf_scan_choice choice;
	char diagnostics[SPF_SCAN_DIAG_MAX_BYTES];
	mock_now = 0;
	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, 0) == 0);
	fail_recall = fail_release = 1;
	assert(spf_scan_session_schedule(session, 0, 0, &choice) == -EIO);
	assert(spf_scan_session_fail_stage(session, 100, -ETIMEDOUT, SPF_SCAN_STAGE_PRODUCER) == -EIO);
	assert(spf_scan_session_diagnostics(session, 1, 2, diagnostics, sizeof(diagnostics)) > 0);
	assert(strstr(diagnostics, "\"first_error\":-5"));
	assert(strstr(diagnostics, "\"restoration_error\":-121"));
	assert(strstr(diagnostics, "\"failure_stage\":1"));
	fail_recall = fail_release = 0;
	{
		struct spf_scan_session_output output;
		assert(spf_scan_session_take_output(session, &output) == 0);
		assert(spf_scan_session_complete_output(session, output.record.visit) == 0);
	}
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_dual_rx_uses_one_shared_recall_and_eight_byte_frames(void)
{
	const uint64_t base = UINT64_C(0x7ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 8, .drain_bytes_per_second = 60000000,
		.release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();
	static uint64_t data;
	uintptr_t token = 900;
	uint64_t boundary;

	request.source_rate_hz = 2500000;
	request.analog_bandwidth_hz = 2500000;
	request.rx_mask = SPF_SCAN_RX1_RX2;
	request.target_count = 1;
	memset(&request.targets[1], 0, sizeof(request.targets[1]));
	assert(spf_scan_setup_validate(&request) == 0);
	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	recall_count = 0;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	assert(spf_scan_session_schedule(session, base, base, &choice) == 0);
	assert(recall_count == 1);
	assert(spf_scan_session_next_boundary(session, &boundary) == 0);
	assert(boundary == choice.selection_counter + 75000);
	assert(spf_scan_session_feed(session, token++, &data, base, 100000) == 0);
	/* The mocked DMA block already extends past the nominal boundary; schedule
	 * at its coherent source watermark as the live scheduler does. */
	mock_now = base + 100000;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice) == 0);
	assert(recall_count == 1);
	assert(spf_scan_session_next_boundary(session, &boundary) == 0);
	assert(boundary == mock_now + 50000);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == 0);
	assert(output.record.result == SPF_VISIT_COMPLETE);
	assert(output.record.iq_bytes == (output.record.valid_end -
		output.record.valid_start) * 8);
	assert(spf_scan_session_complete_output(session, output.record.visit) == 0);
	assert(spf_scan_session_cancel(session, boundary) == 0);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(spf_scan_session_complete_output(session, output.record.visit) == 0);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_delayed_recall_moves_valid_start_without_failing_session(void)
{
	const uint64_t base = UINT64_C(0x8ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4, .drain_bytes_per_second = 60000000,
		.release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();
	static uint32_t data;
	uintptr_t token = 950;
	uint64_t boundary;
	unsigned i;

	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	/* The recall ends 60k samples after the nominal selection+transition
	 * boundary.  Preserve a full 100k-sample settling interval after the
	 * measured completion instead of terminating with -ETIME. */
	recall_before_offset = 150000;
	recall_after_offset = 160000;
	assert(spf_scan_session_schedule(session, base, base, &choice) == 0);
	assert(spf_scan_session_next_boundary(session, &boundary) == 0);
	assert(boundary == base + 460000);
	recall_before_offset = 100;
	recall_after_offset = 1000;
	for (i = 0; i < 5; i++)
		assert(spf_scan_session_feed(session, token++, &data,
			base + (uint64_t)i * 100000, 100000) == 0);
	mock_now = base + 500000;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice) == 0);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.result == SPF_VISIT_COMPLETE);
	assert(output.record.transition_before == base + 150000);
	assert(output.record.transition_after == base + 160000);
	assert(output.record.valid_start == base + 260000);
	assert(output.record.valid_end == base + 460000);
	assert(spf_scan_session_complete_output(session, output.record.visit) == 0);
	assert(spf_scan_session_cancel(session, boundary) == 0);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(spf_scan_session_complete_output(session, output.record.visit) == 0);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_transport_failure_cancels_current_and_remainder(void)
{
	const uint64_t base = UINT64_C(0x3ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4,
		.drain_bytes_per_second = 60000000, .release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();
	uintptr_t token = 100;

	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	assert(spf_scan_session_schedule(session, base, base, &choice) == 0);
	feed_three(session, base, &token);
	mock_now = base + 300000;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice) == 0);
	feed_three(session, base + 300000, &token);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == 0 && output.record.result == SPF_VISIT_COMPLETE);
	assert(spf_scan_session_abort_output(session, 0, -EPIPE) == -EPIPE);
	assert(radio.released);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == 1 && output.record.result == SPF_VISIT_CANCELLED);
	assert(spf_scan_session_complete_output(session, 1) == 0);
	assert(spf_scan_session_terminal(session, &terminal) == 0);
	assert(terminal.state == SPF_SCAN_TERMINAL_FAILED && terminal.error == -EPIPE);
	assert(terminal.delivered == 0 && terminal.cancelled == 2 && terminal.iq_bytes == 0);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_graceful_cancel_restores_and_accounts(void)
{
	const uint64_t base = UINT64_C(0x4ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4,
		.drain_bytes_per_second = 60000000, .release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();

	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	assert(spf_scan_session_schedule(session, base, base, &choice) == 0);
	mock_now = base + 1000;
	assert(spf_scan_session_cancel(session, mock_now) == 0);
	assert(radio.released);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.result == SPF_VISIT_CANCELLED);
	assert(spf_scan_session_complete_output(session, 0) == 0);
	assert(spf_scan_session_terminal(session, &terminal) == 0);
	assert(terminal.state == SPF_SCAN_TERMINAL_CANCELLED);
	assert(terminal.error == -ECANCELED && terminal.cancelled == 1);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_rebase_uses_full_dma_epoch_before_first_visit(void)
{
	const uint64_t epoch = UINT64_C(5) << 32;
	const uint64_t anchor = epoch + UINT64_C(0x1000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4,
		.drain_bytes_per_second = 60000000, .release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();
	uint64_t counter;

	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = anchor + 100;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, 0) == 0);
	assert(spf_scan_session_counter(session, &counter) == 0);
	assert(counter == (uint32_t)mock_now);
	assert(spf_scan_session_rebase(session, anchor) == 0);
	assert(spf_scan_session_counter(session, &counter) == 0);
	assert(counter == mock_now);
	assert(spf_scan_session_schedule(session, counter, counter, &choice) == 0);
	assert(choice.selection_counter == mock_now);
	assert(spf_scan_session_cancel(session, counter + 1000) == 0);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.result == SPF_VISIT_CANCELLED);
	assert(spf_scan_session_complete_output(session, 0) == 0);
	assert(spf_scan_session_terminal(session, &terminal) == 0);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_reservation_failure_never_serializes_admitted_placeholder(void)
{
	const uint64_t base = UINT64_C(0x6ffff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4,
		.drain_bytes_per_second = 60000000, .release_block = release_block,
	};
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_choice choice;
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_setup request = setup();
	static uint32_t data;
	uintptr_t token = 500;
	unsigned i;

	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == 0);
	assert(spf_scan_session_schedule(session, base, base, &choice) == 0);
	for (i = 0; i < 4; i++)
		assert(spf_scan_session_feed(session, token++, &data,
			base + (uint64_t)i * 100000, 100000) == 0);
	mock_now = base + 300000;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice) == -ERANGE);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == 0 && output.record.result == SPF_VISIT_CANCELLED);
	assert(spf_scan_session_complete_output(session, 0) == 0);
	assert(spf_scan_session_take_output(session, &output) == 0);
	assert(output.record.visit == 1 && output.record.result == SPF_VISIT_CANCELLED);
	assert(output.record.profile_crc32 == request.targets[output.record.target].profile_crc32);
	assert(output.record.transition_before == mock_now);
	assert(output.record.transition_after == mock_now);
	assert(output.record.valid_start == mock_now && output.record.valid_end == mock_now);
	assert(spf_scan_session_complete_output(session, 1) == 0);
	assert(spf_scan_session_terminal(session, &terminal) == 0);
	assert(terminal.state == SPF_SCAN_TERMINAL_FAILED && terminal.error == -ERANGE);
	assert(terminal.cancelled == 2 && terminal.planned == 2);
	assert(spf_scan_session_destroy(session) == 0);
}

static void test_runtime_rates_and_rejected_clock(void)
{
	const uint32_t rates[] = {1250000, 2500000, 5000000, 7500000, 8000000, 12345679, 61440000};
	const uint64_t base = UINT64_C(0x7ffff0000);
	unsigned i, bytes;
	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		for (bytes = 4; bytes <= 8; bytes += 4) {
			struct spf_scan_session_runtime runtime = {
				.block_count = 24, .headroom_blocks = 2, .block_samples = 1000000,
				.bytes_per_sample = bytes, .drain_bytes_per_second = 60000000,
				.release_block = release_block,
			};
			struct spf_scan_setup request = setup();
			struct spf_scan_session *session;
			struct spf_scan_radio radio;
			struct spf_scan_choice choice;
			struct spf_scan_session_output output;
			struct spf_scan_visit_record decoded;
			struct spf_scan_terminal terminal;
			uint8_t wire[SPF_SCAN_VISIT_BYTES];
			static uint8_t data[8000000];
			uint64_t boundary, at, total = 0;
			uintptr_t token = 1;
			unsigned slice;
			request.protocol_version = 2;
			request.source_rate_hz = rates[i];
			request.analog_bandwidth_hz = 200000;
			request.rx_mask = bytes == 8 ? 3 : 1;
			request.dwell_ms = 120;
			request.maximum_revisit_ms = 1000;
			assert(!spf_scan_radio_init(&radio, 29, mock_ioctl));
			mock_now = base;
			assert(!spf_scan_session_create(&session, &request, &runtime, &radio, base));
			assert(acquired_rate == rates[i]);
			assert(!spf_scan_session_schedule(session, base, base, &choice));
			assert(!spf_scan_session_next_boundary(session, &boundary));
			for (at = base; at < boundary; at += 1000000)
				assert(!spf_scan_session_feed(session, token++, data, at, 1000000));
			mock_now = boundary;
			/* The provider observes gain at every dwell boundary. Legacy
			 * protocols must still produce encodable records with reserved
			 * gain bytes zero, even when an observation is supplied. */
			struct spf_scan_gain_observation gain = {
				.counter = boundary, .read_duration_ns = 100,
				.rx1_gain_index = 40, .rx2_gain_index = 40, .valid = true,
			};
			assert(!spf_scan_session_observe_gain(session, choice.visit, &gain));
			assert(!spf_scan_session_stop(session, boundary));
			assert(!spf_scan_session_take_output(session, &output));
			assert(output.record.result == SPF_VISIT_COMPLETE);
			assert(output.record.valid_end - output.record.valid_start ==
			       (uint64_t)rates[i] * 120 / 1000);
			assert(output.record.iq_bytes == (uint64_t)rates[i] * 120 / 1000 * bytes);
			for (slice = 0; slice < output.slice_count; slice++)
				total += output.slices[slice].bytes;
			assert(total == output.record.iq_bytes);
			assert(!spf_scan_visit_encode(wire, sizeof(wire), &output.record));
			assert(!spf_scan_visit_decode(&decoded, wire, sizeof(wire)));
			assert(decoded.protocol_version == 2 && decoded.source_rate_hz == rates[i]);
			assert(!spf_scan_session_complete_output(session, output.record.visit));
			assert(!spf_scan_session_terminal(session, &terminal));
			assert(radio.released && terminal.delivered == 1 && terminal.iq_bytes == total);
			assert(!spf_scan_session_destroy(session));
			assert(!spf_scan_radio_init(&radio, 29, mock_ioctl));
			reject_rate = rates[i];
			assert(spf_scan_session_create(&session, &request, &runtime, &radio, base) == -ERANGE);
			assert(!radio.acquired);
			reject_rate = 0;
		}
	}
}

static void test_native_low_rate_and_stalled_final_dma(void)
{
	const uint64_t base = UINT64_C(0x800000000);
	for (unsigned stall = 0; stall < 2; stall++) {
		struct spf_scan_setup request = setup();
		struct spf_scan_session_runtime runtime = {
			.block_count = 16, .headroom_blocks = 2, .block_samples = 1000000,
			.bytes_per_sample = 8, .drain_bytes_per_second = 60000000,
			.release_block = release_block,
		};
		struct spf_scan_session *session;
		struct spf_scan_radio radio;
		struct spf_scan_choice choice;
		struct spf_scan_session_output output;
		struct spf_scan_terminal terminal;
		uint8_t wire[SPF_SCAN_VISIT_BYTES];
		static uint8_t data[8000000];
		uint64_t boundary;
		request.protocol_version = 3;
		request.source_rate_hz = request.analog_bandwidth_hz = 1250000;
		request.rx_mask = 3;
		request.dwell_ms = 120;
		request.maximum_revisit_ms = 1000;
		mock_now = base;
		assert(!spf_scan_radio_init(&radio, 29, mock_ioctl));
		assert(!spf_scan_session_create(&session, &request, &runtime, &radio, base));
		assert(!spf_scan_session_schedule(session, base, base, &choice));
		assert(!spf_scan_session_next_boundary(session, &boundary));
		struct spf_scan_gain_observation gain = {
			.counter = boundary, .read_duration_ns = 100,
			.rx1_gain_index = 40, .rx2_gain_index = 41, .valid = true,
		};
		assert(!spf_scan_session_observe_gain(session, choice.visit, &gain));
		if (!stall)
			assert(!spf_scan_session_feed(session, 99, data, base, 1000000));
		mock_now = boundary;
		assert(!spf_scan_session_stop(session, boundary));
		if (stall) {
			assert(spf_scan_session_terminal(session, &terminal) == -EAGAIN);
			assert(spf_scan_session_fail(session, boundary, -ETIMEDOUT) == -ETIMEDOUT);
		}
		assert(!spf_scan_session_take_output(session, &output));
		assert(!spf_scan_visit_encode(wire, sizeof(wire), &output.record));
		assert(output.record.gain_valid && output.record.rx2_gain_index == 41);
		assert(output.record.valid_end - output.record.valid_start == 150000);
		assert(output.record.iq_bytes == (stall ? 0 : 1200000));
		assert(!spf_scan_session_complete_output(session, choice.visit));
		assert(!spf_scan_session_terminal(session, &terminal));
		assert(terminal.flags & 1);
		assert(terminal.error == (stall ? -ETIMEDOUT : 0));
		assert(terminal.state == (stall ? SPF_SCAN_TERMINAL_FAILED : SPF_SCAN_TERMINAL_COMPLETED));
		assert(!spf_scan_session_destroy(session));
	}
}

static void test_last_recall_deadline_preserves_completed_visit(void)
{
	const uint64_t base = UINT64_C(0x1afff0000);
	struct spf_scan_session_runtime runtime = {
		.block_count = 8, .headroom_blocks = 2, .block_samples = 100000,
		.bytes_per_sample = 4, .drain_bytes_per_second = 60000000,
		.release_block = release_block,
	};
	struct spf_scan_setup request = setup();
	struct spf_scan_session *session;
	struct spf_scan_radio radio;
	struct spf_scan_choice choice;
	struct spf_scan_terminal terminal;
	uintptr_t token = 1200;
	unsigned before;

	request.duration_ms = 60;
	request.maximum_revisit_ms = 60;
	assert(spf_scan_radio_init(&radio, 29, mock_ioctl) == 0);
	mock_now = base;
	assert(!spf_scan_session_create(&session, &request, &runtime, &radio, base));
	assert(!spf_scan_session_schedule(session, base, base, &choice));
	feed_three(session, base, &token);
	/* The first visit is complete but intentionally not drained. The next
	 * selection fits nominally; successful attestation pushes it past end. */
	mock_now = base + 300000;
	recall_before_offset = 100;
	recall_after_offset = 150000;
	before = recall_count;
	assert(spf_scan_session_schedule(session, mock_now, mock_now, &choice) == -ENODATA);
	assert(recall_count == before + 1);
	mock_now += recall_after_offset;
	recall_after_offset = 1000;
	/* The scheduler supplies its pre-recall snapshot to stop. */
	assert(!spf_scan_session_stop(session, base + 300000));
	assert(radio.released);
	(void)take_complete(session, 0);
	assert(!spf_scan_session_terminal(session, &terminal));
	assert(terminal.state == SPF_SCAN_TERMINAL_COMPLETED && !terminal.error);
	assert(terminal.planned == 1 && terminal.delivered == 1);
	assert(!terminal.invalid && !terminal.skipped && !terminal.cancelled);
	assert(terminal.final_counter == base + 450000);
	assert(terminal.restore_before >= terminal.final_counter);
	assert(!spf_scan_session_destroy(session));
}

static void test_continuous_post_recall_guard_preserves_finite_modes(void)
{
	const uint64_t base = UINT64_C(0x1ffff0000);
	const uint64_t after_offsets[] = { 0, 49999, 50000, 50001, 100000, 0 };
	const struct spf_scan_session_runtime runtime = {
		.block_count = 16, .headroom_blocks = 2, .block_samples = 1000000,
		.bytes_per_sample = 8, .drain_bytes_per_second = 1000000000,
		.release_block = release_block,
	};
	uintptr_t token = 5000;

	for (unsigned version = 1; version <= 5; version++) {
		for (unsigned boundary = 0; boundary < (version == 5 ? 6U : 1U); boundary++) {
			struct spf_scan_setup request = setup();
			struct spf_scan_session *session;
			struct spf_scan_radio radio;
			struct spf_scan_choice choice;
			struct spf_scan_session_output output;
			struct spf_scan_terminal terminal;
			uint64_t selection = base + (boundary == 5 ? 1000 : 0);
			uint64_t transition, expected_start, samples, end;

			request.protocol_version = version;
			request.rx_mask = SPF_SCAN_RX1_RX2;
			request.maximum_revisit_ms = 1000;
			if (version != 1) {
				request.source_rate_hz = 2500000;
				request.analog_bandwidth_hz = 2000000;
			}
			if (version == 3 || version == 4)
				request.dwell_ms = 120;
			if (version == 5) {
				request.duration_ms = 0;
				request.transition_budget_ms = 20;
				request.maximum_boost = 1;
				request.target_count = 8;
				for (unsigned i = 0; i < 8; i++)
					request.targets[i] = (struct spf_scan_target) {
						i, i, UINT64_C(2400000000) + i * 1000000, 1, i + 1,
					};
			}
			transition = (uint64_t)request.source_rate_hz * request.transition_budget_ms / 1000;
			samples = (uint64_t)request.source_rate_hz * request.dwell_ms / 1000;
			recall_before_offset = 0;
			recall_after_offset = version == 5 ? after_offsets[boundary] : transition - 1;
			mock_now = base;
			assert(!spf_scan_radio_init(&radio, 29, mock_ioctl));
			assert(!spf_scan_session_create(&session, &request, &runtime, &radio, base));
			assert(!spf_scan_session_schedule(session, selection, base, &choice));
			expected_start = selection + transition;
			if (version == 5 && base + recall_after_offset + transition > expected_start)
				expected_start = base + recall_after_offset + transition;
			assert(!spf_scan_session_next_boundary(session, &end));
			assert(end == expected_start + samples);
			feed_blocks(session, base, &token, 1, 1000000);
			mock_now = base + 1000000;
			assert(!spf_scan_session_stop(session, mock_now));
			output = take_complete_bytes(session, 0, samples * 8);
			assert(output.record.valid_start == expected_start);
			assert(output.record.transition_after == base + recall_after_offset);
			if (version == 5)
				assert(output.record.valid_start - output.record.transition_after >= transition);
			else
				assert(output.record.valid_start - output.record.transition_after == 1);
			assert(!spf_scan_session_terminal(session, &terminal));
			assert(terminal.delivered == 1 && !terminal.error && terminal.flags == 1);
			assert(!spf_scan_session_destroy(session));
		}
	}
	recall_before_offset = 100;
	recall_after_offset = 1000;
}

static void test_continuous_ordered_bounded_history_and_counter_wrap(void)
{
	const uint64_t base = UINT64_C(0xffff0000);
	struct spf_scan_setup request = setup();
	struct spf_scan_session_runtime runtime = {
		.block_count=16, .headroom_blocks=2, .block_samples=150000,
		.bytes_per_sample=8, .drain_bytes_per_second=1000000000,
		.release_block=release_block,
	};
	struct spf_scan_radio radio;
	struct spf_scan_session *session;
	struct spf_scan_choice choice;
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_control query={7,1,2,1};
	struct spf_scan_status status;
	uintptr_t token=1;
	uint64_t now=base;
	request.protocol_version=SPF_SCAN_CONTINUOUS_ORDERED_VERSION;
	request.source_rate_hz=2500000; request.analog_bandwidth_hz=2000000;
	request.duration_ms=0; request.dwell_ms=20; request.transition_budget_ms=20;
	request.maximum_revisit_ms=500; request.maximum_boost=1; request.target_count=8;
	request.rx_mask=SPF_SCAN_RX1_RX2;
	for (unsigned i=0;i<8;i++) request.targets[i]=(struct spf_scan_target){i,i,UINT64_C(2400000000)+i*1000000,1,UINT32_C(0x90000000)+i};
	mock_now=now;
	assert(!spf_scan_radio_init(&radio,29,mock_ioctl));
	assert(!spf_scan_session_create(&session,&request,&runtime,&radio,now));
	assert(!spf_scan_session_schedule(session,now,now,&choice));
	for (uint64_t visit=0;visit<20000;visit++) {
		assert(choice.visit==visit && choice.target==visit%8 && choice.dwell_ms==20);
		/* Recall completion plus the full guard precedes exact20ms support. */
		feed_blocks(session,now,&token,1,150000);
		now+=150000; mock_now=now;
		if (visit==19999) assert(!spf_scan_session_stop(session,now));
		else assert(!spf_scan_session_schedule(session,now,now,&choice));
		assert(!spf_scan_session_take_output(session,&output));
		assert(output.record.visit==visit && output.record.target==visit%8);
		assert(output.record.protocol_version==5 && output.record.iq_bytes==400000);
		assert(output.record.valid_end-output.record.valid_start==50000);
		assert(output.record.valid_start-output.record.transition_after>=50000);
		assert(!spf_scan_session_complete_output(session,visit));
		assert(!spf_scan_session_status(session,&query,&status));
		assert(status.delivered==visit+1 && status.queued_visits<=1);
	}
	assert(!spf_scan_session_stop(session,now)); /* idempotent v5 */
	assert(!spf_scan_session_terminal(session,&terminal));
	assert(terminal.planned==20000 && terminal.delivered==20000 && !terminal.skipped && !terminal.invalid);
	assert(terminal.state==SPF_SCAN_TERMINAL_COMPLETED && terminal.reason==2 && terminal.flags==1);
	assert(status.state==3 && status.restored_flags==1);
	assert(!spf_scan_session_destroy(session));
}

static void test_continuous_diagnostics_survive_history_reuse(void)
{
	const uint64_t base = UINT64_C(0xffff0000);
	struct spf_scan_setup request = setup();
	struct spf_scan_session_runtime runtime = {
		.block_count = 16, .headroom_blocks = 2, .block_samples = 150000,
		.bytes_per_sample = 8, .drain_bytes_per_second = 1000000000,
		.release_block = release_block,
	};
	struct spf_scan_radio radio;
	struct spf_scan_session *session;
	struct spf_scan_choice choice;
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	struct spf_scan_control query = {7, 1, 2, 1};
	struct spf_scan_status status;
	char diagnostics[SPF_SCAN_DIAG_MAX_BYTES];
	uint64_t now = base;
	uintptr_t token = 1;

	request.protocol_version = SPF_SCAN_CONTINUOUS_ORDERED_VERSION;
	request.source_rate_hz = 2500000; request.analog_bandwidth_hz = 2000000;
	request.duration_ms = 0; request.dwell_ms = 20; request.transition_budget_ms = 20;
	request.maximum_revisit_ms = 500; request.maximum_boost = 1;
	request.target_count = 8; request.rx_mask = SPF_SCAN_RX1_RX2;
	for (unsigned i = 0; i < 8; i++)
		request.targets[i] = (struct spf_scan_target){i, i,
			UINT64_C(2400000000) + i * 1000000, 1, UINT32_C(0x90000000) + i};
	mock_now = now;
	assert(!spf_scan_radio_init(&radio, 29, mock_ioctl));
	assert(!spf_scan_session_create(&session, &request, &runtime, &radio, now));
	assert(!spf_scan_session_schedule(session, now, now, &choice));
	for (uint64_t visit = 0; visit < 200; visit++) {
		feed_blocks(session, now, &token, 1, 150000);
		now += 150000; mock_now = now;
		if (visit == 199) {
			fail_recall = fail_release = 1;
			assert(spf_scan_session_schedule(session, now, now, &choice) == -EIO);
			assert(spf_scan_session_fail_stage(session, now, -ETIMEDOUT,
				SPF_SCAN_STAGE_PRODUCER) == -EIO);
			fail_recall = fail_release = 0;
		} else {
			assert(!spf_scan_session_schedule(session, now, now, &choice));
		}
		assert(!spf_scan_session_take_output(session, &output));
		assert(output.record.visit == visit && output.record.iq_bytes == 400000);
		assert(!spf_scan_session_complete_output(session, visit));
	}
	/* The v0.61 first-failure evidence must retain absolute visit identity
	 * after the continuous ledger has reused every physical history slot. */
	assert(spf_scan_session_diagnostics(session, 1, 2, diagnostics, sizeof(diagnostics)) > 0);
	assert(strstr(diagnostics, "\"first_error\":-5"));
	assert(strstr(diagnostics, "\"restoration_error\":-121"));
	assert(strstr(diagnostics, "\"failure_stage\":1"));
	assert(strstr(diagnostics, "\"failure_visit\":200"));
	assert(!spf_scan_session_status(session, &query, &status));
	assert(status.state == 4 && status.error == -EIO && status.planned == 201);
	assert(!spf_scan_session_take_output(session, &output));
	assert(output.record.visit == 200 && !output.record.iq_bytes);
	assert(!spf_scan_session_complete_output(session, 200));
	assert(!spf_scan_session_terminal(session, &terminal));
	assert(terminal.delivered == 200 && terminal.cancelled == 1 && terminal.error == -EIO);
	assert(!spf_scan_session_destroy(session));
}

static void test_continuous_partial_cancel_and_gap(void)
{
	for (unsigned gap=0;gap<3;gap++) {
		const uint64_t base=UINT64_C(0x1ffff0000);
		struct spf_scan_setup request=setup();
		struct spf_scan_session_runtime runtime={.block_count=16,.headroom_blocks=2,
			.block_samples=25000,.bytes_per_sample=8,.drain_bytes_per_second=1000000000,.release_block=release_block};
		struct spf_scan_radio radio;
		struct spf_scan_session *session;
		struct spf_scan_choice choice;
		struct spf_scan_session_output output;
		struct spf_scan_terminal terminal;
		struct spf_scan_control query={1,1,2,1};
		struct spf_scan_status status;
		uint8_t wire[SPF_SCAN_VISIT_BYTES];
		struct spf_scan_visit_record decoded;
		uintptr_t token=1;
		request.protocol_version=5; request.source_rate_hz=2500000; request.analog_bandwidth_hz=2000000;
		request.duration_ms=0; request.target_count=8; request.rx_mask=3; request.maximum_boost=1;
		request.transition_budget_ms=20; request.maximum_revisit_ms=500;
		for (unsigned i=0;i<8;i++) request.targets[i]=(struct spf_scan_target){i,i,UINT64_C(2400000000)+i*1000000,1,i+1};
		mock_now=base;
		assert(!spf_scan_radio_init(&radio,29,mock_ioctl));
		assert(!spf_scan_session_create(&session,&request,&runtime,&radio,base));
		assert(!spf_scan_session_schedule(session,base,base,&choice));
		feed_blocks(session,base+1000,&token,3,25000);
		mock_now=base+101000;
		if (gap==1) feed_blocks(session,base+101000,&token,1,25000);
		else if (gap==2) {
			assert(!spf_scan_session_stop(session,base+101000));
			assert(!radio.released);
			assert(!spf_scan_session_cancel(session,base+101000));
			assert(!spf_scan_session_cancel(session,base+101000));
		}
		else assert(!spf_scan_session_cancel(session,base+76000));
		assert(!spf_scan_session_status(session,&query,&status));
		assert(status.state==(gap==1?4:3));
		assert(!spf_scan_session_take_output(session,&output));
		assert(output.record.result==(gap==2?SPF_VISIT_CANCELLED:SPF_VISIT_INVALID_GAP) && output.record.iq_bytes==200000);
		assert(output.record.valid_end-output.record.valid_start==25000 && output.slice_count==1);
		assert(!spf_scan_visit_encode(wire,sizeof(wire),&output.record));
		assert(!spf_scan_visit_decode(&decoded,wire,sizeof(wire)) && decoded.iq_bytes==200000);
		assert(!spf_scan_session_complete_output(session,0));
		assert(!spf_scan_session_terminal(session,&terminal));
		assert(terminal.iq_bytes==200000 && terminal.invalid==(gap==2?0:1) && terminal.flags==1);
		assert(terminal.state==(gap==1?SPF_SCAN_TERMINAL_FAILED:SPF_SCAN_TERMINAL_CANCELLED));
		assert(!spf_scan_session_destroy(session));
	}
}

static void test_continuous_pressure_stops_without_silent_skip(void)
{
	const uint64_t base=UINT64_C(0x1ffff0000);
	struct spf_scan_setup request=setup();
	struct spf_scan_session_runtime runtime={.block_count=16,.headroom_blocks=2,
		.block_samples=150000,.bytes_per_sample=8,.drain_bytes_per_second=1000000000,.release_block=release_block};
	struct spf_scan_radio radio;
	struct spf_scan_session *session;
	struct spf_scan_choice choice;
	struct spf_scan_session_output output;
	struct spf_scan_terminal terminal;
	uint8_t wire[SPF_SCAN_VISIT_BYTES];
	uintptr_t token=1;
	request.protocol_version=5; request.source_rate_hz=2500000; request.analog_bandwidth_hz=2000000;
	request.duration_ms=0; request.target_count=8; request.rx_mask=3; request.maximum_boost=1;
	request.transition_budget_ms=20; request.maximum_revisit_ms=500; request.maximum_queue_visits=1;
	for (unsigned i=0;i<8;i++) request.targets[i]=(struct spf_scan_target){i,i,UINT64_C(2400000000)+i*1000000,1,i+1};
	mock_now=base;
	assert(!spf_scan_radio_init(&radio,29,mock_ioctl));
	assert(!spf_scan_session_create(&session,&request,&runtime,&radio,base));
	assert(!spf_scan_session_schedule(session,base,base,&choice));
	feed_blocks(session,base,&token,1,150000);
	mock_now=base+150000;
	assert(spf_scan_session_schedule(session,mock_now,mock_now,&choice)==-ENOSPC);
	(void)take_complete_bytes(session,0,400000);
	assert(!spf_scan_session_take_output(session,&output));
	assert(output.record.visit==1 && output.record.target==1 && output.record.result==SPF_VISIT_SKIP_CAPACITY);
	assert(!output.record.iq_bytes && output.record.valid_end==output.record.valid_start);
	assert(!spf_scan_visit_encode(wire,sizeof(wire),&output.record));
	assert(!spf_scan_session_complete_output(session,1));
	assert(!spf_scan_session_terminal(session,&terminal));
	assert(terminal.state==SPF_SCAN_TERMINAL_FAILED && terminal.error==-ENOSPC && terminal.planned==2);
	assert(terminal.delivered==1 && terminal.skipped==1 && terminal.iq_bytes==400000 && terminal.flags==1);
	assert(!spf_scan_session_destroy(session));
}

int main(void)
{
	test_continuous_diagnostics_survive_history_reuse();
	test_continuous_post_recall_guard_preserves_finite_modes();
	test_continuous_partial_cancel_and_gap();
	test_continuous_pressure_stops_without_silent_skip();
	test_continuous_ordered_bounded_history_and_counter_wrap();
	test_native_low_rate_and_stalled_final_dma();
	test_last_recall_deadline_preserves_completed_visit();
	test_v4_fixed_360_ms_visit_is_captured();
	test_three_visits_feedback_and_early_restore();
	test_recall_failure_cancels_and_restores();
	test_first_failure_survives_restoration_and_producer_errors();
	test_dual_rx_uses_one_shared_recall_and_eight_byte_frames();
	test_delayed_recall_moves_valid_start_without_failing_session();
	test_transport_failure_cancels_current_and_remainder();
	test_graceful_cancel_restores_and_accounts();
	test_rebase_uses_full_dma_epoch_before_first_visit();
	test_reservation_failure_never_serializes_admitted_placeholder();
	test_runtime_rates_and_rejected_clock();
	puts("PASS: scan session joins scheduler, owner recall, DMA visits, feedback and restore");
	return 0;
}
