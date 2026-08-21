/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "spf-temperature-cache.h"

#include <spf_thread_join.h>

#include <iio.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SPF_TEMPERATURE_ATTR "input"
#define SPF_TEMPERATURE_CHANNEL "temp0"
#define SPF_TEMPERATURE_DEVICE "ad9361-phy"
#define SPF_TEMPERATURE_STOP_TIMEOUT_MS UINT32_C(500)
#define SPF_TEMPERATURE_STUCK_EXIT_STATUS 70

static bool monotonic_now(uint64_t *now_ns)
{
	struct timespec now;

	if (!now_ns || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return false;
	*now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
	return true;
}

bool spf_temperature_cache_init(struct spf_temperature_cache *cache)
{
	if (!cache)
		return false;
	memset(cache, 0, sizeof(*cache));
	cache->temperature_mdeg_c = SPF_TEMPERATURE_INVALID;
	if (pthread_mutex_init(&cache->mutex, NULL) != 0)
		return false;
	cache->initialized = true;
	return true;
}

void spf_temperature_cache_destroy(struct spf_temperature_cache *cache)
{
	if (!cache || !cache->initialized)
		return;
	pthread_mutex_destroy(&cache->mutex);
	memset(cache, 0, sizeof(*cache));
}

bool spf_temperature_cache_store(struct spf_temperature_cache *cache,
	int32_t temperature_mdeg_c, uint64_t successful_read_ns)
{
	if (!cache || !cache->initialized ||
		temperature_mdeg_c == SPF_TEMPERATURE_INVALID ||
		!successful_read_ns)
		return false;
	pthread_mutex_lock(&cache->mutex);
	cache->temperature_mdeg_c = temperature_mdeg_c;
	cache->successful_read_ns = successful_read_ns;
	cache->valid = true;
	pthread_mutex_unlock(&cache->mutex);
	return true;
}

int32_t spf_temperature_cache_get(struct spf_temperature_cache *cache,
	uint64_t now_ns)
{
	int32_t value = SPF_TEMPERATURE_INVALID;

	if (!cache || !cache->initialized || !now_ns)
		return value;
	pthread_mutex_lock(&cache->mutex);
	if (cache->valid && now_ns >= cache->successful_read_ns &&
		now_ns - cache->successful_read_ns <= SPF_TEMPERATURE_MAX_AGE_NS)
		value = cache->temperature_mdeg_c;
	pthread_mutex_unlock(&cache->mutex);
	return value;
}

static struct iio_channel *find_temperature_channel(struct iio_context *context)
{
	struct iio_device *phy;

	if (!context)
		return NULL;
	phy = iio_context_find_device(context, SPF_TEMPERATURE_DEVICE);
	return phy ? iio_device_find_channel(
		phy, SPF_TEMPERATURE_CHANNEL, false) : NULL;
}

static bool wait_for_next_poll(struct spf_temperature_sampler *sampler)
{
	struct timespec deadline;
	bool stop;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		return false;
	deadline.tv_sec += 1;
	pthread_mutex_lock(&sampler->state_mutex);
	if (!sampler->stop_requested)
		(void)pthread_cond_timedwait(
			&sampler->wake, &sampler->state_mutex, &deadline);
	stop = sampler->stop_requested;
	pthread_mutex_unlock(&sampler->state_mutex);
	return !stop;
}

static void *temperature_thread(void *opaque)
{
	struct spf_temperature_sampler *sampler = opaque;
	struct iio_context *context;
	struct iio_channel *temperature;
	struct sched_param normal_priority = {.sched_priority = 0};

	(void)pthread_setname_np(pthread_self(), "SPF_TEMP_SAMPLE");
	(void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority);
	context = iio_create_local_context();
	temperature = find_temperature_channel(context);
	do {
		long long reading;
		uint64_t now_ns;

		if (temperature &&
			iio_channel_attr_read_longlong(
				temperature, SPF_TEMPERATURE_ATTR, &reading) == 0 &&
			reading >= INT32_MIN + 1LL && reading <= INT32_MAX &&
			monotonic_now(&now_ns))
			(void)spf_temperature_cache_store(
				&sampler->cache, (int32_t)reading, now_ns);
	} while (wait_for_next_poll(sampler));
	if (context)
		iio_context_destroy(context);
	return NULL;
}

bool spf_temperature_sampler_start(struct spf_temperature_sampler *sampler)
{
	pthread_condattr_t attributes;
	bool attributes_initialized = false;

	if (!sampler)
		return false;
	memset(sampler, 0, sizeof(*sampler));
	if (!spf_temperature_cache_init(&sampler->cache))
		return false;
	if (pthread_mutex_init(&sampler->state_mutex, NULL) != 0)
		goto fail;
	sampler->state_mutex_initialized = true;
	if (pthread_condattr_init(&attributes) != 0)
		goto fail;
	attributes_initialized = true;
	if (pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) != 0 ||
		pthread_cond_init(&sampler->wake, &attributes) != 0)
		goto fail;
	sampler->wake_initialized = true;
	pthread_condattr_destroy(&attributes);
	attributes_initialized = false;
	if (pthread_create(
		&sampler->thread, NULL, temperature_thread, sampler) != 0)
		goto fail;
	sampler->thread_started = true;
	return true;

fail:
	if (attributes_initialized)
		pthread_condattr_destroy(&attributes);
	spf_temperature_sampler_stop(sampler);
	return false;
}

void spf_temperature_sampler_stop(struct spf_temperature_sampler *sampler)
{
	if (!sampler)
		return;
	if (sampler->thread_started) {
		int join_error = 0;
		spf_thread_join_result_t join_result;

		pthread_mutex_lock(&sampler->state_mutex);
		sampler->stop_requested = true;
		pthread_cond_broadcast(&sampler->wake);
		pthread_mutex_unlock(&sampler->state_mutex);
		join_result = spf_thread_join_bounded(sampler->thread,
			SPF_TEMPERATURE_STOP_TIMEOUT_MS, &join_error);
		if (join_result != SPF_THREAD_JOIN_OK) {
			fprintf(stderr,
				"temperature sampler teardown did not complete within %u ms "
				"(result=%d error=%d); terminating for supervised recovery\n",
				SPF_TEMPERATURE_STOP_TIMEOUT_MS, (int)join_result, join_error);
			fflush(NULL);
			_exit(SPF_TEMPERATURE_STUCK_EXIT_STATUS);
		}
		sampler->thread_started = false;
	}
	if (sampler->wake_initialized) {
		pthread_cond_destroy(&sampler->wake);
		sampler->wake_initialized = false;
	}
	if (sampler->state_mutex_initialized) {
		pthread_mutex_destroy(&sampler->state_mutex);
		sampler->state_mutex_initialized = false;
	}
	spf_temperature_cache_destroy(&sampler->cache);
}

int32_t spf_temperature_sampler_get(struct spf_temperature_sampler *sampler)
{
	uint64_t now_ns;

	if (!sampler || !sampler->thread_started || !monotonic_now(&now_ns))
		return SPF_TEMPERATURE_INVALID;
	return spf_temperature_cache_get(&sampler->cache, now_ns);
}
