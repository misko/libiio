/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_SCAN_RATE_H
#define SPF_SCAN_RATE_H

#include <stdbool.h>
#include <stdint.h>

/* Request bounds, not a promise that the active AD9361 clock/FIR topology
 * can synthesize every integer. Acquisition still requires exact readback. */
#define SPF_SCAN_RATE_MIN UINT32_C(520833)
#define SPF_SCAN_RATE_MAX UINT32_C(61440000)

static inline bool spf_scan_rate_valid(uint32_t rate)
{
	return rate >= SPF_SCAN_RATE_MIN && rate <= SPF_SCAN_RATE_MAX;
}

/* Durations and maximum ages never exceed their requested millisecond
 * bounds. Quantization is <1 sample per interval. Counter endpoints, not
 * rate*wall-time estimates, remain authoritative. uint32 inputs fit uint64. */
static inline uint64_t spf_scan_ticks(uint32_t rate, uint32_t ms)
{
	return (uint64_t)rate * ms / 1000;
}

#endif
