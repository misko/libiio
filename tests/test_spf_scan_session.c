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
static unsigned recall_count;

static int mock_ioctl(int fd, unsigned long request, void *argument)
{
	assert(fd == 29);
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
	if (request == ADI_RX_COUNTER_IOC_ACQUIRE)
		return 0;
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
		recall->counter_before = (uint32_t)(mock_now + 100);
		recall->counter_after = (uint32_t)(mock_now + 1000);
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_RELEASE_SCAN) {
		struct adi_rx_counter_scan_release *release = argument;

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

int main(void)
{
	test_three_visits_feedback_and_early_restore();
	test_recall_failure_cancels_and_restores();
	test_dual_rx_uses_one_shared_recall_and_eight_byte_frames();
	test_transport_failure_cancels_current_and_remainder();
	test_graceful_cancel_restores_and_accounts();
	test_rebase_uses_full_dma_epoch_before_first_visit();
	test_reservation_failure_never_serializes_admitted_placeholder();
	puts("PASS: scan session joins scheduler, owner recall, DMA visits, feedback and restore");
	return 0;
}
