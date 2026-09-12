/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_COUNTER_METADATA_H
#define SPF_COUNTER_METADATA_H
#include <stddef.h>
#include <stdint.h>
#include "buffer-metadata.h"
#define SPF_COUNTER_REQUEST_MAGIC UINT32_C(0x43465053)
#define SPF_COUNTER_FRAME_MAGIC UINT32_C(0x31435053)
#define SPF_COUNTER_REQUEST_BYTES 32U
#define SPF_COUNTER_FRAME_BYTES 80U
#define SPF_COUNTER_FEATURES 7U
struct spf_counter_request {
	uint32_t sample_rate_hz;
	uint32_t samples_per_channel;
};
uint32_t spf_counter_read32(const void *wire);
uint64_t spf_counter_read64(const void *wire);
int spf_counter_request_decode(const void *wire, size_t bytes, size_t samples, uint32_t scan_mask,
			       struct spf_counter_request *request);
int spf_counter_frame_build(void *wire, size_t capacity, uint64_t stream, uint64_t sequence,
			    uint64_t first, uint64_t missing, uint32_t samples, uint32_t rate);
int spf_counter_frame_describe(const void *wire, size_t bytes,
			       struct iiod_buffer_metadata_frame_info *info);
int spf_counter_frame_rebase(void *wire, size_t bytes, uint64_t previous_end);
#endif
