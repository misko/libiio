/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-scheduler.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

struct fake_io {
	struct spf_hop_request_v1 request;
	uint64_t counter;
	uint64_t event_id;
	unsigned int recalls;
	unsigned int restores;
	unsigned int starts;
};

static struct spf_hop_request_v1 make_request(void)
{
	struct spf_hop_request_v1 request = {0};
	unsigned int i;

	request.required_features = SPF_HOP_REQUIRED_FEATURES_V1;
	request.flags = SPF_HOP_REQUEST_FLAGS_V1;
	request.session_id = 71;
	request.sample_rate_hz = 1000;
	request.rf_bandwidth_hz = 1000;
	request.if_offset_hz = 10;
	request.dwell_samples = 100;
	request.transition_guard_samples = 10;
	request.dwell_count = 4;
	request.capture_span_samples = 250;
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		request.profiles[i].profile_id = (uint8_t)i;
		request.profiles[i].fastlock_slot = (uint8_t)(7U - i);
		request.profiles[i].lo_frequency_hz = 1000000U + i * 1000U;
		request.profiles[i].center_frequency_hz =
			request.profiles[i].lo_frequency_hz + 10U;
		request.profiles[i].profile_crc32 = 0xa0000000U + i;
	}
	return request;
}

static int fake_start(void *opaque, uint64_t expected_original_lo_hz)
{
	struct fake_io *io = opaque;

	assert(expected_original_lo_hz == io->request.profiles[0].lo_frequency_hz);
	io->starts++;
	return 0;
}

static int fake_counter(void *opaque, uint64_t *counter)
{
	struct fake_io *io = opaque;

	*counter = io->counter;
	return 0;
}

static int fake_recall(void *opaque, uint32_t profile,
	uint64_t expected_lo_hz,
	struct spf_hop_scheduler_transition_v1 *transition)
{
	struct fake_io *io = opaque;

	/* Include three samples of honest scheduler latency after startup. */
	if (io->recalls)
		io->counter += 3;
	transition->transition_before = io->counter;
	io->counter += 2;
	transition->transition_after = io->counter;
	transition->actual_lo_frequency_hz = expected_lo_hz;
	transition->active_profile = profile;
	transition->device_event_id = ++io->event_id;
	io->recalls++;
	return 0;
}

static int fake_restore(void *opaque, uint64_t expected_original_lo_hz,
	struct spf_hop_scheduler_restore_v1 *restore)
{
	struct fake_io *io = opaque;

	io->restores++;
	restore->transition_before = io->counter;
	restore->transition_after = ++io->counter;
	restore->actual_lo_frequency_hz = expected_original_lo_hz;
	restore->active_profile = UINT32_MAX;
	return 0;
}

static int fake_sleep(void *opaque, uint64_t nanoseconds)
{
	struct fake_io *io = opaque;
	uint64_t samples = nanoseconds * io->request.sample_rate_hz /
		UINT64_C(1000000000);

	io->counter += samples ? samples : 1;
	return 0;
}

static const struct spf_hop_scheduler_io_v1 fake_ops = {
	.start = fake_start,
	.get_counter = fake_counter,
	.recall = fake_recall,
	.restore = fake_restore,
	.sleep_ns = fake_sleep,
};

static void test_finite_schedule_and_conservative_lateness(void)
{
	struct spf_hop_device_event_v1 events[8];
	struct spf_hop_restore_receipt_v1 receipt;
	const struct spf_hop_device_ops_v1 *ops;
	struct fake_io io = {0};
	void *context;
	uint64_t dropped;
	size_t total = 0;
	unsigned int attempt;
	int ret;

	io.request = make_request();
	io.counter = 1000;
	assert(spf_hop_scheduler_v1_create(&io.request, &fake_ops, &io,
		&context, &ops) == 0);
	assert(ops->submit_plan(context, &io.request) == 0);
	for (attempt = 0; attempt < 100000 && total < 3; attempt++) {
		size_t count = 0;

		ret = ops->drain_events(context, events + total,
			8U - total, &count, &dropped);
		assert(ret == 0);
		assert(dropped == 0);
		total += count;
		if (total < 3)
			sched_yield();
	}
	assert(total == 3);
	assert(io.starts == 1 && io.recalls == 3);
	assert(events[0].transition_before == 1000);
	assert(events[0].transition_after == 1002);
	assert(events[1].transition_before == 1115);
	assert(events[1].transition_after == 1117);
	assert(events[2].transition_before == 1230);
	assert(events[2].transition_after == 1232);
	assert(events[0].device_event_id == 1 &&
		events[2].device_event_id == 3);
	io.counter = 1332;
	memset(&receipt, 0, sizeof(receipt));
	assert(ops->cancel_restore(context, SPF_HOP_REASON_PLAN_COMPLETE,
		&receipt) == 0);
	assert(io.restores == 1);
	assert(receipt.transition_before == 1332);
	assert(receipt.transition_after == 1333);
	assert(receipt.restored_lo_frequency_hz ==
		io.request.profiles[0].lo_frequency_hz);
	assert(receipt.restored_profile == SPF_HOP_PROFILE_NONE);
	spf_hop_scheduler_v1_destroy(context);
}

int main(void)
{
	test_finite_schedule_and_conservative_lateness();
	return 0;
}
