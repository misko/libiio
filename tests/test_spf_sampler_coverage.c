#include "spf-sampler-coverage.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

int main(void)
{
	struct spf_sampler_coverage_plan plan;
	assert(!spf_sampler_requires_continuous_coverage(false, false));
	assert(spf_sampler_requires_continuous_coverage(true, false));
	assert(spf_sampler_requires_continuous_coverage(false, true));
	assert(spf_sampler_requires_continuous_coverage(true, true));

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
