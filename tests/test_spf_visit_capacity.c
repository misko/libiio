/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "spf-visit-queue.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct simulation {
	struct spf_visit_queue *queue;
	uint32_t rate;
	uint32_t block_samples;
	uint64_t drain_bytes_per_second;
	uint64_t now;
	uint64_t next_block_first;
	uint64_t send_end;
	uint64_t sending_visit;
	uint64_t next_token;
	uint64_t delivered_samples;
	uint64_t released_blocks;
	uint32_t dummy;
	bool sending;
};

struct result {
	uint64_t planned, admitted, delivered, skipped_capacity, skipped_age;
	uint64_t planned_samples, delivered_samples, elapsed_ticks;
	uint64_t high_water_bytes;
};

static int release_block(void *opaque, uintptr_t token)
{
	struct simulation *simulation = opaque;

	assert(token && token < simulation->next_token);
	simulation->released_blocks++;
	return 0;
}

static uint64_t divide_up(uint64_t numerator, uint64_t denominator)
{
	return numerator / denominator + !!(numerator % denominator);
}

static void start_send(struct simulation *simulation)
{
	struct spf_visit_view view;
	uint64_t bytes = 0;
	unsigned int i;
	int ret;

	if (simulation->sending)
		return;
	ret = spf_visit_queue_take(simulation->queue, &view);
	if (ret == -EAGAIN)
		return;
	assert(ret == 0);
	assert(view.result == SPF_VISIT_COMPLETE);
	for (i = 0; i < view.slice_count; i++)
		bytes += view.slices[i].bytes;
	assert(bytes == (view.end - view.start) * 4);
	simulation->sending = true;
	simulation->sending_visit = view.id;
	simulation->send_end = simulation->now +
		divide_up(bytes * simulation->rate,
			  simulation->drain_bytes_per_second);
}

static void finish_send(struct simulation *simulation)
{
	struct spf_visit_queue_stats stats;

	assert(simulation->sending && simulation->send_end == simulation->now);
	assert(spf_visit_queue_complete_send(simulation->queue,
					     simulation->sending_visit) == 0);
	simulation->sending = false;
	assert(spf_visit_queue_reap(simulation->queue) == 0);
	spf_visit_queue_stats(simulation->queue, &stats);
	assert(stats.leased_blocks <= 50 && stats.reserved_bytes <= 200000000);
}

static void feed_block(struct simulation *simulation)
{
	uintptr_t token = (uintptr_t)simulation->next_token++;

	assert(spf_visit_queue_feed(simulation->queue, token, &simulation->dummy,
				    simulation->next_block_first,
				    simulation->block_samples) == 0);
	simulation->next_block_first += simulation->block_samples;
	assert(spf_visit_queue_reap(simulation->queue) == 0);
}

static void advance_to(struct simulation *simulation, uint64_t target)
{
	while (simulation->now < target) {
		uint64_t block_ready = simulation->next_block_first +
			simulation->block_samples;
		uint64_t event = target;

		start_send(simulation);
		if (simulation->sending && simulation->send_end < event)
			event = simulation->send_end;
		if (block_ready < event)
			event = block_ready;
		simulation->now = event;
		/* Free a block before accepting another when events are simultaneous. */
		if (simulation->sending && simulation->send_end == event)
			finish_send(simulation);
		if (block_ready == event)
			feed_block(simulation);
	}
	start_send(simulation);
}

static struct result simulate(uint32_t rate, uint32_t dwell_ms,
			      uint64_t drain_bytes_per_second,
			      uint32_t duration_seconds)
{
	const uint64_t transition = divide_up((uint64_t)rate * 6960, 1000000);
	const uint32_t dwell_samples = (uint32_t)((uint64_t)rate * dwell_ms / 1000);
	const uint64_t duration = (uint64_t)rate * duration_seconds;
	struct spf_visit_queue_config config = {
		.block_count = 50,
		.headroom_blocks = 2,
		.maximum_visits = 50,
		.block_samples = 1000000,
		.source_rate_hz = rate,
		.maximum_bytes = 200000000,
		.maximum_age_ticks = (uint64_t)rate * 5,
		.drain_bytes_per_second = drain_bytes_per_second,
		.bytes_per_sample = 4,
	};
	struct simulation simulation = {
		.rate = rate,
		.block_samples = config.block_samples,
		.drain_bytes_per_second = drain_bytes_per_second,
		.next_token = 1,
	};
	struct spf_visit_queue_stats stats;
	struct result result = { 0 };
	uint64_t transition_start = 0, previous_id = 0;

	assert(spf_visit_queue_create(&simulation.queue, &config,
				      release_block, &simulation) == 0);
	while (transition_start + transition + dwell_samples <= duration) {
		enum spf_visit_result admission;
		uint64_t id = ++result.planned;
		uint64_t valid_start = transition_start + transition;
		int ret;

		advance_to(&simulation, transition_start);
		if (previous_id)
			assert(spf_visit_queue_close_window(simulation.queue,
							    previous_id,
							    transition_start) == 0);
		start_send(&simulation);
		ret = spf_visit_queue_reserve(simulation.queue, id, dwell_samples,
						     transition_start, &admission);
		assert(ret == 0);
		result.planned_samples += dwell_samples;
		if (admission == SPF_VISIT_ADMITTED) {
			assert(spf_visit_queue_bind(simulation.queue, id,
						    valid_start) == 0);
			result.admitted++;
			previous_id = id;
		} else {
			assert(admission == SPF_VISIT_SKIP_CAPACITY ||
			       admission == SPF_VISIT_SKIP_AGE);
			if (admission == SPF_VISIT_SKIP_CAPACITY)
				result.skipped_capacity++;
			else
				result.skipped_age++;
			previous_id = 0;
		}
		transition_start += transition + dwell_samples;
	}

	advance_to(&simulation, transition_start);
	if (previous_id)
		assert(spf_visit_queue_close_window(simulation.queue, previous_id,
						    transition_start) == 0);
	/* Make the DMA block containing the final sample available. */
	advance_to(&simulation, transition_start + config.block_samples);
	for (;;) {
		spf_visit_queue_stats(simulation.queue, &stats);
		if (!stats.visits)
			break;
		start_send(&simulation);
		assert(simulation.sending);
		advance_to(&simulation, simulation.send_end);
	}
	assert(spf_visit_queue_reap(simulation.queue) == 0);
	spf_visit_queue_stats(simulation.queue, &stats);
	assert(!stats.visits && !stats.leased_blocks && !stats.reserved_blocks &&
	       !stats.reserved_bytes && !stats.missing_samples);
	result.delivered = result.admitted;
	result.delivered_samples = result.delivered * dwell_samples;
	result.elapsed_ticks = transition_start;
	result.high_water_bytes = stats.high_water_bytes;
	assert(result.planned == result.delivered + result.skipped_capacity +
				 result.skipped_age);
	assert(spf_visit_queue_destroy(simulation.queue) == 0);
	return result;
}

static double ratio(uint64_t numerator, uint64_t denominator)
{
	return denominator ? (double)numerator / (double)denominator : 0.0;
}

static void print_result(uint32_t rate, uint32_t dwell_ms, uint64_t drain,
			 const struct result *result)
{
	printf("%2u MS/s %3u ms %2" PRIu64 " MB/s: duty %.3f delivery %.3f "
	       "visits %" PRIu64 "/%" PRIu64 " skips %" PRIu64
	       " high-water %.1f MB\n",
	       rate / 1000000, dwell_ms, drain / 1000000,
	       ratio(result->delivered_samples, result->elapsed_ticks),
	       ratio(result->delivered_samples, result->planned_samples),
	       result->delivered, result->planned,
	       result->skipped_capacity + result->skipped_age,
	       result->high_water_bytes / 1000000.0);
}

int main(void)
{
	struct result ten_240 = simulate(10000000, 240, 55000000, 300);
	struct result ten_120 = simulate(10000000, 120, 55000000, 300);
	struct result fifteen_240 = simulate(15000000, 240, 55000000, 300);
	struct result near_capacity = simulate(15000000, 240, 60000000, 300);
	struct result twenty = simulate(20000000, 240, 60000000, 120);
	struct result thirty = simulate(30000000, 240, 60000000, 120);

	print_result(10000000, 240, 55000000, &ten_240);
	print_result(10000000, 120, 55000000, &ten_120);
	print_result(15000000, 240, 55000000, &fifteen_240);
	print_result(15000000, 240, 60000000, &near_capacity);
	print_result(20000000, 240, 60000000, &twenty);
	print_result(30000000, 240, 60000000, &thirty);

	assert(ratio(ten_240.delivered_samples, ten_240.elapsed_ticks) > 0.95);
	assert(ratio(ten_120.delivered_samples, ten_120.planned_samples) > 0.99);
	assert(ratio(fifteen_240.delivered_samples,
		     fifteen_240.elapsed_ticks) >= 0.90);
	assert(ratio(near_capacity.delivered_samples,
		     near_capacity.planned_samples) == 1.0);
	assert(ratio(near_capacity.delivered_samples * 4,
		     divide_up(near_capacity.elapsed_ticks * UINT64_C(60000000),
			       UINT64_C(15000000))) >= 0.95);
	assert(twenty.skipped_capacity + twenty.skipped_age > 0);
	assert(thirty.skipped_capacity + thirty.skipped_age > 0);
	puts("PASS: real visit queue sustains duty gates and sheds only whole visits");
	return 0;
}
