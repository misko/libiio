/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-temperature-cache.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_freshness_and_invalid_sentinel(void)
{
	struct spf_temperature_cache cache;
	const uint64_t observed = UINT64_C(50000000000);

	assert(spf_temperature_cache_init(&cache));
	assert(spf_temperature_cache_get(&cache, observed) ==
		SPF_TEMPERATURE_INVALID);
	assert(spf_temperature_cache_store(&cache, 43860, observed));
	assert(spf_temperature_cache_get(&cache, observed) == 43860);
	assert(spf_temperature_cache_get(&cache,
		observed + SPF_TEMPERATURE_MAX_AGE_NS) == 43860);
	assert(spf_temperature_cache_get(&cache,
		observed + SPF_TEMPERATURE_MAX_AGE_NS + 1) ==
		SPF_TEMPERATURE_INVALID);
	assert(spf_temperature_cache_get(&cache, observed - 1) ==
		SPF_TEMPERATURE_INVALID);
	assert(!spf_temperature_cache_store(
		&cache, SPF_TEMPERATURE_INVALID, observed + 1));
	assert(spf_temperature_cache_store(&cache, -1250, observed + 1));
	assert(spf_temperature_cache_get(&cache, observed + 1) == -1250);
	spf_temperature_cache_destroy(&cache);
}

int main(void)
{
	test_freshness_and_invalid_sentinel();
	puts("SPF temperature cache tests passed");
	return 0;
}
