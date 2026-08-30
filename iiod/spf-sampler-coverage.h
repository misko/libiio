/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_SPF_SAMPLER_COVERAGE_H__
#define __IIOD_SPF_SAMPLER_COVERAGE_H__

#include <stdbool.h>
#include <stdint.h>

struct spf_sampler_coverage_plan {
	uint64_t window_samples;
	uint64_t maximum_observations;
};

/*
 * Host-buffered capture must remain covered continuously: unlike ordinary
 * request/response IIO, a burst or ring producer can be delayed by DDR copies
 * and consumer backpressure while the kernel DMA queue remains armed.
 */
bool spf_sampler_requires_continuous_coverage(bool burst_enabled,
	bool ring_enabled);

/*
 * Bound sampler work to every block which may still be captured by the
 * kernel DMA queue, plus one arm-safety window.  The returned observation
 * bound proves the sampler ledger can retain that complete window.
 */
int spf_sampler_coverage_plan_compute(uint32_t samples_per_frame,
	uint32_t observation_interval_samples,
	unsigned int kernel_buffers_count,
	uint32_t observation_capacity,
	struct spf_sampler_coverage_plan *plan);

#endif
