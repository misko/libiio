/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-ddr-ring-status.h"

#include <stdbool.h>
#include <errno.h>
#include <string.h>

static uint16_t get_le16(const uint8_t *source)
{
	return (uint16_t)source[0] | (uint16_t)source[1] << 8;
}

static uint32_t get_le32(const uint8_t *source)
{
	return (uint32_t)source[0] | (uint32_t)source[1] << 8 |
		(uint32_t)source[2] << 16 | (uint32_t)source[3] << 24;
}

static uint64_t get_le64(const uint8_t *source)
{
	return (uint64_t)get_le32(source) | (uint64_t)get_le32(source + 4) << 32;
}

static void put_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
	destination[2] = (uint8_t)(value >> 16);
	destination[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *destination, uint64_t value)
{
	put_le32(destination, (uint32_t)value);
	put_le32(destination + 4, (uint32_t)(value >> 32));
}

static bool status_values_valid(const struct spf_ddr_ring_status *status)
{
	if (status->state > SPF_DDR_RING_STATE_CANCELLED ||
		status->terminal_reason > SPF_DDR_RING_REASON_INTERNAL_ERROR)
		return false;
	if (status->valid_fields & ~(SPF_DDR_RING_STATUS_VALID_LAST_CONTIGUOUS |
			SPF_DDR_RING_STATUS_VALID_FIRST_UNAVAILABLE))
		return false;
	if (status->consumed_frames > status->produced_frames ||
		status->high_water_frames > status->produced_frames)
		return false;
	return true;
}

int spf_ddr_ring_status_encode(void *wire_status, size_t wire_bytes,
	const struct spf_ddr_ring_status *source)
{
	uint8_t *wire = wire_status;

	if (!wire || !source || wire_bytes != SPF_DDR_RING_STATUS_BYTES)
		return -EINVAL;
	if (!status_values_valid(source))
		return -EINVAL;
	memset(wire, 0, wire_bytes);
	put_le32(wire, SPF_DDR_RING_STATUS_MAGIC);
	put_le16(wire + 4, SPF_DDR_RING_STATUS_VERSION);
	put_le16(wire + 6, SPF_DDR_RING_STATUS_BYTES);
	put_le32(wire + 8, source->state);
	put_le32(wire + 12, source->terminal_reason);
	put_le32(wire + 16, source->valid_fields);
	put_le32(wire + 20, (uint32_t)source->error_code);
	put_le64(wire + 24, source->requested_capacity_iq_bytes);
	put_le64(wire + 32, source->admitted_capacity_iq_bytes);
	put_le64(wire + 40, source->target_frames);
	put_le64(wire + 48, source->produced_frames);
	put_le64(wire + 56, source->consumed_frames);
	put_le64(wire + 64, source->high_water_frames);
	put_le64(wire + 72, source->wrap_count);
	put_le64(wire + 80, source->producer_position);
	put_le64(wire + 88, source->consumer_position);
	put_le64(wire + 96, source->last_contiguous_sample_sequence);
	put_le64(wire + 104, source->first_unavailable_sample_sequence);
	return 0;
}

int spf_ddr_ring_status_decode(struct spf_ddr_ring_status *destination,
	const void *wire_status, size_t wire_bytes)
{
	const uint8_t *wire = wire_status;
	struct spf_ddr_ring_status status;

	if (!destination || !wire || wire_bytes != SPF_DDR_RING_STATUS_BYTES)
		return -EINVAL;
	if (get_le32(wire) != SPF_DDR_RING_STATUS_MAGIC ||
		get_le16(wire + 4) != SPF_DDR_RING_STATUS_VERSION ||
		get_le16(wire + 6) != SPF_DDR_RING_STATUS_BYTES)
		return -EPROTONOSUPPORT;
	if (get_le64(wire + 112) || get_le64(wire + 120))
		return -EINVAL;
	memset(&status, 0, sizeof(status));
	status.state = get_le32(wire + 8);
	status.terminal_reason = get_le32(wire + 12);
	status.valid_fields = get_le32(wire + 16);
	status.error_code = (int32_t)get_le32(wire + 20);
	status.requested_capacity_iq_bytes = get_le64(wire + 24);
	status.admitted_capacity_iq_bytes = get_le64(wire + 32);
	status.target_frames = get_le64(wire + 40);
	status.produced_frames = get_le64(wire + 48);
	status.consumed_frames = get_le64(wire + 56);
	status.high_water_frames = get_le64(wire + 64);
	status.wrap_count = get_le64(wire + 72);
	status.producer_position = get_le64(wire + 80);
	status.consumer_position = get_le64(wire + 88);
	status.last_contiguous_sample_sequence = get_le64(wire + 96);
	status.first_unavailable_sample_sequence = get_le64(wire + 104);
	if (!status_values_valid(&status))
		return -EINVAL;
	*destination = status;
	return 0;
}
