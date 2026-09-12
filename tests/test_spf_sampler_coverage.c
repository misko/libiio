#include "spf-sampler-coverage.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static unsigned overlapping_reads(uint32_t interval)
{
	/* Reproduce the observed geometry: a 131072-sample DMA block lies
	 * between read intervals when a once-per-block poll is delayed. The
	 * preceding read ended before the block; the next starts 3812 samples
	 * beyond its end. Denser real reads restore overlap without extending
	 * a recorded observation's validity interval. */
	int64_t before = -15116;
	unsigned count = 0;
	while (before < 300000) {
		if (before + 10000 >= 0 && before < 131072)
			++count;
		before += interval + 18928;
	}
	return count;
}

int main(void)
{
	struct spf_sampler_coverage_plan plan;
	uint32_t interval = 0;

	assert(overlapping_reads(131072) == 0);
	assert(spf_sampler_queued_observation_interval(131072, 131072,
		&interval) == 0);
	assert(overlapping_reads(interval) != 0);
	assert(spf_sampler_coverage_plan_compute(131072, interval, 32, 1024,
		&plan) == 0);
	assert(plan.window_samples == UINT64_C(4325376));
	assert(plan.maximum_observations == 66);
	assert(spf_sampler_coverage_plan_compute(131072, interval, 32, 65,
		&plan) == -E2BIG);
	assert(spf_sampler_queued_observation_interval(131072, 8192,
		&interval) == 0 && interval == 8192);
	assert(spf_sampler_queued_observation_interval(1, 1, &interval) == 0 &&
		interval == 1);
	assert(spf_sampler_queued_observation_interval(0, 1, &interval) == -EINVAL);
	assert(spf_sampler_queued_observation_interval(1, 0, &interval) == -EINVAL);
	assert(spf_sampler_queued_observation_interval(1, 1, NULL) == -EINVAL);

	assert(spf_sampler_coverage_plan_compute(UINT32_C(1048576),
		UINT32_C(1048576), 4U, 1024U, &plan) == 0);
	assert(plan.window_samples == UINT64_C(5242880));
	assert(plan.maximum_observations == UINT64_C(5));

	assert(spf_sampler_coverage_plan_compute(UINT32_C(1048576),
		UINT32_C(262144), 4U, 1024U, &plan) == 0);
	assert(plan.window_samples == UINT64_C(5242880));
	assert(plan.maximum_observations == UINT64_C(20));

	assert(spf_sampler_coverage_plan_compute(UINT32_MAX, UINT32_C(1),
		UINT32_MAX, UINT32_MAX, &plan) == -E2BIG);
	assert(spf_sampler_coverage_plan_compute(0, UINT32_C(1), 4U, 1024U,
		&plan) == -EINVAL);
	assert(spf_sampler_coverage_plan_compute(UINT32_C(1), 0, 4U, 1024U,
		&plan) == -EINVAL);
	assert(spf_sampler_coverage_plan_compute(UINT32_C(1), UINT32_C(1), 0,
		1024U, &plan) == -EINVAL);
	assert(spf_sampler_coverage_plan_compute(UINT32_C(1), UINT32_C(1), 4U,
		0, &plan) == -EINVAL);
	assert(spf_sampler_coverage_plan_compute(UINT32_C(1), UINT32_C(1), 4U,
		1024U, NULL) == -EINVAL);
	return 0;
}
