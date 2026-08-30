/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "spf-gain-telemetry.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct fake_sampler {
	bool start_result;
	bool wait_result;
	bool limit_wait_result;
	bool finish_result;
	bool rssi_result;
	unsigned int start_calls;
	unsigned int stop_calls;
	unsigned int limit_calls;
	unsigned int unlimit_calls;
	unsigned int wait_calls;
	unsigned int limit_wait_calls;
	unsigned int finish_calls;
	unsigned int collect_calls;
	unsigned int rssi_calls;
};

static struct fake_sampler fake;

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.start_result = true;
	fake.wait_result = true;
	fake.limit_wait_result = true;
	fake.finish_result = true;
	fake.rssi_result = true;
}

bool spf_gain_sampler_start(spf_gain_sampler_t *sampler,
	uint32_t interval_samples)
{
	assert(sampler);
	assert(interval_samples);
	fake.start_calls++;
	return fake.start_result;
}

void spf_gain_sampler_stop(spf_gain_sampler_t *sampler)
{
	assert(sampler);
	fake.stop_calls++;
}

void spf_gain_sampler_limit(spf_gain_sampler_t *sampler, uint64_t samples)
{
	assert(sampler);
	assert(samples);
	fake.limit_calls++;
}

void spf_gain_sampler_unlimit(spf_gain_sampler_t *sampler)
{
	assert(sampler);
	fake.unlimit_calls++;
}

bool spf_gain_sampler_wait_started(spf_gain_sampler_t *sampler,
	uint32_t timeout_ms)
{
	assert(sampler);
	assert(timeout_ms);
	fake.wait_calls++;
	return fake.wait_result;
}

bool spf_gain_sampler_limit_and_wait_started(spf_gain_sampler_t *sampler,
	uint64_t samples, uint32_t timeout_ms)
{
	assert(sampler);
	assert(samples);
	assert(timeout_ms);
	fake.limit_wait_calls++;
	return fake.limit_wait_result;
}

bool spf_gain_sampler_finish_capture(spf_gain_sampler_t *sampler,
	uint32_t timeout_ms)
{
	assert(sampler);
	assert(timeout_ms);
	fake.finish_calls++;
	return fake.finish_result;
}

uint16_t spf_gain_sampler_collect(spf_gain_sampler_t *sampler,
	uint64_t frame_start, uint32_t samples,
	spf_gain_observation_v3_t *destination, uint16_t capacity,
	uint32_t *overflow_count)
{
	assert(sampler);
	assert(frame_start);
	assert(samples);
	assert(destination);
	assert(capacity >= 2);
	assert(overflow_count);
	fake.collect_calls++;
	memset(destination, 0, 2 * sizeof(*destination));
	*overflow_count = 3;
	return 2;
}

bool spf_gain_sampler_collect_rssi(spf_gain_sampler_t *sampler,
	uint64_t frame_start, uint32_t samples, spf_rssi_pair_t *rssi_start,
	spf_rssi_pair_t *rssi_end, uint32_t *overflow_count)
{
	assert(sampler);
	assert(frame_start);
	assert(samples);
	assert(rssi_start);
	assert(rssi_end);
	assert(overflow_count);
	fake.rssi_calls++;
	memset(rssi_start, 0, sizeof(*rssi_start));
	memset(rssi_end, 0, sizeof(*rssi_end));
	rssi_start->valid = fake.rssi_result;
	rssi_end->valid = fake.rssi_result;
	*overflow_count = 4;
	return fake.rssi_result;
}

static void test_optional_start_failure_is_safe(void)
{
	struct spf_gain_telemetry telemetry;
	spf_gain_observation_v3_t observations[2];
	spf_rssi_pair_t rssi_start;
	spf_rssi_pair_t rssi_end;
	enum spf_gain_telemetry_failure failure;
	uint32_t overflow = 99;
	int error = 0;

	reset_fake();
	fake.start_result = false;
	spf_gain_telemetry_init(&telemetry, true);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(!spf_gain_telemetry_is_usable(&telemetry));
	assert(!telemetry.thread_started);
	assert(spf_gain_telemetry_take_diagnostic(
		&telemetry, &failure, &error));
	assert(failure == SPF_GAIN_TELEMETRY_START_FAILED);
	assert(error == -EIO);
	assert(!spf_gain_telemetry_take_diagnostic(
		&telemetry, &failure, &error));

	memset(&rssi_start, 0x55, sizeof(rssi_start));
	memset(&rssi_end, 0x55, sizeof(rssi_end));
	assert(spf_gain_telemetry_collect(&telemetry, 1, 128,
		observations, 2, &overflow) == 0);
	assert(overflow == 0);
	overflow = 99;
	assert(!spf_gain_telemetry_collect_rssi(&telemetry, 1, 128,
		&rssi_start, &rssi_end, &overflow));
	assert(overflow == 0);
	assert(!rssi_start.valid && !rssi_end.valid);
	assert(spf_gain_telemetry_wait_started(&telemetry, 100) == 0);
	assert(spf_gain_telemetry_limit_and_wait_started(
		&telemetry, 0, 100) == 0);
	assert(spf_gain_telemetry_finish_capture(&telemetry, 100) == 0);
	spf_gain_telemetry_limit(&telemetry, 1000);
	spf_gain_telemetry_unlimit(&telemetry);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 0);
	assert(fake.collect_calls == 0);
	assert(fake.rssi_calls == 0);
	assert(fake.limit_calls == 0);
	assert(fake.unlimit_calls == 0);
}

static void test_legacy_start_failure_is_fatal(void)
{
	struct spf_gain_telemetry telemetry;
	enum spf_gain_telemetry_failure failure;
	int error;

	reset_fake();
	fake.start_result = false;
	spf_gain_telemetry_init(&telemetry, false);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == -EIO);
	assert(!spf_gain_telemetry_is_usable(&telemetry));
	assert(!spf_gain_telemetry_take_diagnostic(
		&telemetry, &failure, &error));
}

static void test_optional_start_fence_timeout_defers_stop(void)
{
	struct spf_gain_telemetry telemetry;
	enum spf_gain_telemetry_failure failure;
	int error;

	reset_fake();
	fake.wait_result = false;
	spf_gain_telemetry_init(&telemetry, true);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(telemetry.thread_started);
	assert(spf_gain_telemetry_wait_started(&telemetry, 100) == 0);
	assert(!spf_gain_telemetry_is_usable(&telemetry));
	assert(telemetry.thread_started);
	assert(fake.stop_calls == 0);
	assert(spf_gain_telemetry_take_diagnostic(
		&telemetry, &failure, &error));
	assert(failure == SPF_GAIN_TELEMETRY_START_FENCE_TIMEOUT);
	assert(error == -ETIMEDOUT);
	assert(spf_gain_telemetry_finish_capture(&telemetry, 100) == 0);
	assert(fake.finish_calls == 0);
	spf_gain_telemetry_stop(&telemetry);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 1);
}

static void test_optional_limit_fence_timeout_defers_stop(void)
{
	struct spf_gain_telemetry telemetry;

	reset_fake();
	fake.limit_wait_result = false;
	spf_gain_telemetry_init(&telemetry, true);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(spf_gain_telemetry_limit_and_wait_started(
		&telemetry, 8192, 100) == 0);
	assert(!spf_gain_telemetry_is_usable(&telemetry));
	assert(telemetry.thread_started);
	assert(fake.stop_calls == 0);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 1);
}

static void test_optional_finish_timeout_defers_stop(void)
{
	struct spf_gain_telemetry telemetry;
	enum spf_gain_telemetry_failure failure;
	int error;

	reset_fake();
	fake.finish_result = false;
	spf_gain_telemetry_init(&telemetry, true);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(spf_gain_telemetry_finish_capture(&telemetry, 100) == 0);
	assert(!spf_gain_telemetry_is_usable(&telemetry));
	assert(fake.stop_calls == 0);
	assert(spf_gain_telemetry_take_diagnostic(
		&telemetry, &failure, &error));
	assert(failure == SPF_GAIN_TELEMETRY_FINISH_FENCE_TIMEOUT);
	assert(error == -ETIMEDOUT);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 1);
}

static void test_legacy_fence_timeout_stays_fatal(void)
{
	struct spf_gain_telemetry telemetry;

	reset_fake();
	fake.wait_result = false;
	spf_gain_telemetry_init(&telemetry, false);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(spf_gain_telemetry_wait_started(&telemetry, 100) == -ETIMEDOUT);
	assert(spf_gain_telemetry_is_usable(&telemetry));
	assert(telemetry.thread_started);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 1);
}

static void test_optional_coverage_failure(void)
{
	struct spf_gain_telemetry telemetry;
	enum spf_gain_telemetry_failure failure;
	int error;

	reset_fake();
	spf_gain_telemetry_init(&telemetry, true);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(spf_gain_telemetry_degrade(&telemetry,
		SPF_GAIN_TELEMETRY_COVERAGE_UNAVAILABLE, -ENOSPC) == 0);
	assert(!spf_gain_telemetry_is_usable(&telemetry));
	assert(spf_gain_telemetry_take_diagnostic(
		&telemetry, &failure, &error));
	assert(failure == SPF_GAIN_TELEMETRY_COVERAGE_UNAVAILABLE);
	assert(error == -ENOSPC);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 1);

	reset_fake();
	spf_gain_telemetry_init(&telemetry, false);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	assert(spf_gain_telemetry_degrade(&telemetry,
		SPF_GAIN_TELEMETRY_COVERAGE_UNAVAILABLE, -ENOSPC) == -ENOSPC);
	assert(spf_gain_telemetry_is_usable(&telemetry));
	spf_gain_telemetry_stop(&telemetry);
}

static void test_success_delegates_every_operation(void)
{
	struct spf_gain_telemetry telemetry;
	spf_gain_observation_v3_t observations[2];
	spf_rssi_pair_t rssi_start;
	spf_rssi_pair_t rssi_end;
	uint32_t overflow = 0;

	reset_fake();
	spf_gain_telemetry_init(&telemetry, true);
	assert(spf_gain_telemetry_start(&telemetry, 1024) == 0);
	spf_gain_telemetry_limit(&telemetry, 8192);
	spf_gain_telemetry_unlimit(&telemetry);
	assert(spf_gain_telemetry_wait_started(&telemetry, 100) == 0);
	assert(spf_gain_telemetry_limit_and_wait_started(
		&telemetry, 8192, 100) == 0);
	assert(spf_gain_telemetry_finish_capture(&telemetry, 100) == 0);
	assert(spf_gain_telemetry_collect(&telemetry, 1, 128,
		observations, 2, &overflow) == 2);
	assert(overflow == 3);
	assert(spf_gain_telemetry_collect_rssi(&telemetry, 1, 128,
		&rssi_start, &rssi_end, &overflow));
	assert(overflow == 4);
	assert(fake.start_calls == 1);
	assert(fake.limit_calls == 1);
	assert(fake.unlimit_calls == 1);
	assert(fake.wait_calls == 1);
	assert(fake.limit_wait_calls == 1);
	assert(fake.finish_calls == 1);
	assert(fake.collect_calls == 1);
	assert(fake.rssi_calls == 1);
	spf_gain_telemetry_stop(&telemetry);
	assert(fake.stop_calls == 1);
}

int main(void)
{
	test_optional_start_failure_is_safe();
	test_legacy_start_failure_is_fatal();
	test_optional_start_fence_timeout_defers_stop();
	test_optional_limit_fence_timeout_defers_stop();
	test_optional_finish_timeout_defers_stop();
	test_legacy_fence_timeout_stays_fatal();
	test_optional_coverage_failure();
	test_success_delegates_every_operation();
	return 0;
}
