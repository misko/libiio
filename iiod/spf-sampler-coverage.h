/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_SPF_SAMPLER_COVERAGE_H__
#define __IIOD_SPF_SAMPLER_COVERAGE_H__

#include <stdint.h>

struct spf_sampler_coverage_plan {
	uint64_t window_samples;
	uint64_t maximum_observations;
};

/* A dequeue fence brackets the refill call, not the acquisition of an older
 * queued DMA block. Keep at least two periodic observations per block for hop
 * capture so normal polling jitter cannot consume the entire sampling margin.
 * This does not fabricate coverage or permit accepting an uncovered frame. */
int spf_sampler_queued_observation_interval(uint32_t samples_per_frame,
	uint32_t requested_interval, uint32_t *interval);

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
