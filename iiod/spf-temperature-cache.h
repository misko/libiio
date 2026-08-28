/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_TEMPERATURE_CACHE_H__
#define __SPF_TEMPERATURE_CACHE_H__

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define SPF_TEMPERATURE_INVALID INT32_MIN
#define SPF_TEMPERATURE_MIN_MDEG_C (-INT32_C(40000))
#define SPF_TEMPERATURE_MAX_MDEG_C INT32_C(125000)
#define SPF_TEMPERATURE_MAX_AGE_NS UINT64_C(10000000000)

struct spf_temperature_cache {
	pthread_mutex_t mutex;
	int32_t temperature_mdeg_c;
	uint64_t successful_read_ns;
	bool initialized;
	bool valid;
};

struct spf_temperature_sampler {
	struct spf_temperature_cache cache;
	pthread_t thread;
	pthread_mutex_t state_mutex;
	pthread_cond_t wake;
	bool state_mutex_initialized;
	bool wake_initialized;
	bool thread_started;
	bool stop_requested;
};

bool spf_temperature_cache_init(struct spf_temperature_cache *cache);
void spf_temperature_cache_destroy(struct spf_temperature_cache *cache);
bool spf_temperature_cache_store(struct spf_temperature_cache *cache,
	int32_t temperature_mdeg_c, uint64_t successful_read_ns);
int32_t spf_temperature_cache_get(struct spf_temperature_cache *cache,
	uint64_t now_ns);

bool spf_temperature_sampler_start(struct spf_temperature_sampler *sampler);
void spf_temperature_sampler_stop(struct spf_temperature_sampler *sampler);
int32_t spf_temperature_sampler_get(struct spf_temperature_sampler *sampler);

#endif
