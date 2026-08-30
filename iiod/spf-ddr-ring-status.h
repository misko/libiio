/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_SPF_DDR_RING_STATUS_H__
#define __IIOD_SPF_DDR_RING_STATUS_H__

#include <stddef.h>
#include <stdint.h>

#define SPF_DDR_RING_STATUS_MAGIC UINT32_C(0x53524653) /* SFRS */
#define SPF_DDR_RING_STATUS_VERSION_V1 UINT16_C(1)
#define SPF_DDR_RING_STATUS_VERSION_V2 UINT16_C(2)
#define SPF_DDR_RING_STATUS_VERSION SPF_DDR_RING_STATUS_VERSION_V1
#define SPF_DDR_RING_STATUS_BYTES 128U
#define SPF_DDR_RING_STATUS_VALID_LAST_CONTIGUOUS UINT32_C(1)
#define SPF_DDR_RING_STATUS_VALID_FIRST_UNAVAILABLE UINT32_C(2)
#define SPF_DDR_RING_STATUS_VALID_FAILURE_FRAME UINT32_C(4)
#define SPF_DDR_RING_STATUS_VALID_FAILURE_SAMPLE UINT32_C(8)

enum spf_ddr_ring_state {
	SPF_DDR_RING_STATE_OFF = 0,
	SPF_DDR_RING_STATE_RESERVED = 1,
	SPF_DDR_RING_STATE_RUNNING = 2,
	SPF_DDR_RING_STATE_DRAINING = 3,
	SPF_DDR_RING_STATE_COMPLETE = 4,
	SPF_DDR_RING_STATE_FAILED = 5,
	SPF_DDR_RING_STATE_CANCELLED = 6,
};

enum spf_ddr_ring_terminal_reason {
	SPF_DDR_RING_REASON_NONE = 0,
	SPF_DDR_RING_REASON_TARGET_COMPLETE = 1,
	SPF_DDR_RING_REASON_CLIENT_CANCELLED = 2,
	SPF_DDR_RING_REASON_CLIENT_DISCONNECTED = 3,
	SPF_DDR_RING_REASON_CONSUMER_STALL = 4,
	SPF_DDR_RING_REASON_DMA_ERROR = 5,
	SPF_DDR_RING_REASON_COUNTER_GAP = 6,
	SPF_DDR_RING_REASON_TRANSPORT_ERROR = 7,
	SPF_DDR_RING_REASON_INTERNAL_ERROR = 8,
	SPF_DDR_RING_REASON_GAIN_EVENT_GAP = 9,
	SPF_DDR_RING_REASON_GAIN_EVENT_OVERFLOW = 10,
	SPF_DDR_RING_REASON_METADATA_PROTOCOL = 11,
};

struct spf_ddr_ring_status {
	uint16_t version;
	uint32_t state;
	uint32_t terminal_reason;
	uint32_t valid_fields;
	int32_t error_code;
	uint64_t requested_capacity_iq_bytes;
	uint64_t admitted_capacity_iq_bytes;
	uint64_t target_frames;
	uint64_t produced_frames;
	uint64_t consumed_frames;
	uint64_t high_water_frames;
	uint64_t wrap_count;
	uint64_t producer_position;
	uint64_t consumer_position;
	uint64_t last_contiguous_sample_sequence;
	uint64_t first_unavailable_sample_sequence;
	uint64_t failure_frame_index;
	uint64_t failure_sample_sequence;
};

int spf_ddr_ring_exclusive_boundary(uint64_t first_sample_sequence,
	uint64_t samples_per_frame, uint64_t *exclusive_boundary);

int spf_ddr_ring_status_encode(void *wire_status, size_t wire_bytes,
	const struct spf_ddr_ring_status *source);
int spf_ddr_ring_status_decode(struct spf_ddr_ring_status *destination,
	const void *wire_status, size_t wire_bytes);

#endif
