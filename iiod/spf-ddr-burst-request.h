/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_SPF_DDR_BURST_REQUEST_H__
#define __IIOD_SPF_DDR_BURST_REQUEST_H__

#include <stddef.h>
#include <stdint.h>

#define SPF_DDR_BURST_REQUEST_MAGIC UINT32_C(0x42524653) /* SFRB */
#define SPF_DDR_BURST_REQUEST_VERSION UINT16_C(1)
#define SPF_DDR_BURST_REQUEST_BYTES 32U
#define SPF_DDR_BURST_FEATURE_CACHE_IQ UINT32_C(1)
/*
 * Hardware qualification found intermittent whole-frame loss at 8 ms while
 * 10 ms was the first passing boundary.  Keep 50% headroom over the observed
 * failure boundary so admission does not depend on best-case iiOD scheduling.
 */
#define SPF_DDR_BURST_MIN_FRAME_DURATION_US UINT32_C(12000)

struct spf_ddr_burst_request {
	uint32_t required_features;
	uint64_t requested_iq_bytes;
};

int spf_ddr_burst_request_decode(struct spf_ddr_burst_request *destination,
	const void *wire_request, size_t wire_bytes);
int spf_ddr_burst_validate_frame_period(uint32_t samples_per_channel,
	uint32_t sample_rate_hz);

#endif
