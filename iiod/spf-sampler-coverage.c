/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-sampler-coverage.h"

#include <errno.h>
#include <stddef.h>

int spf_sampler_queued_observation_interval(uint32_t samples_per_frame,
	uint32_t requested_interval, uint32_t *interval)
{
	uint32_t maximum;
	if (!samples_per_frame || !requested_interval || !interval)
		return -EINVAL;
	maximum = samples_per_frame / 2U;
	if (!maximum)
		maximum = 1U;
	*interval = requested_interval < maximum ? requested_interval : maximum;
	return 0;
}

int spf_sampler_coverage_plan_compute(uint32_t samples_per_frame,
	uint32_t observation_interval_samples,
	unsigned int kernel_buffers_count,
	uint32_t observation_capacity,
	struct spf_sampler_coverage_plan *plan)
{
	uint64_t observations_per_frame;
	uint64_t queue_windows;

	if (!samples_per_frame || !observation_interval_samples ||
		!kernel_buffers_count || !observation_capacity || !plan)
		return -EINVAL;

	queue_windows = (uint64_t)kernel_buffers_count + 1U;
	observations_per_frame =
		((uint64_t)samples_per_frame + observation_interval_samples - 1U) /
		observation_interval_samples;
	plan->window_samples = (uint64_t)samples_per_frame * queue_windows;
	plan->maximum_observations = observations_per_frame * queue_windows;
	if (plan->maximum_observations > observation_capacity)
		return -E2BIG;
	return 0;
}
