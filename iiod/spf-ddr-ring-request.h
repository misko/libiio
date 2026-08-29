/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_SPF_DDR_RING_REQUEST_H__
#define __IIOD_SPF_DDR_RING_REQUEST_H__

#include <stddef.h>
#include <stdint.h>

#define SPF_DDR_RING_REQUEST_MAGIC UINT32_C(0x52524653) /* SFRR */
#define SPF_DDR_RING_REQUEST_VERSION UINT16_C(1)
#define SPF_DDR_RING_REQUEST_BYTES 48U
#define SPF_DDR_RING_FEATURE_QUEUE_IQ UINT32_C(1)
#define SPF_DDR_RING_FLAG_FINITE UINT32_C(1)
#define SPF_DDR_RING_FLAG_CONTINUOUS UINT32_C(2)

struct spf_ddr_ring_request {
	uint32_t required_features;
	uint32_t flags;
	uint64_t capacity_iq_bytes;
	uint64_t capture_frames;
};

int spf_ddr_ring_request_decode(struct spf_ddr_ring_request *destination,
	const void *wire_request, size_t wire_bytes);

#endif
