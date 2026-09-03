/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-ddr-ring-request.h"

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

int spf_ddr_ring_request_decode(struct spf_ddr_ring_request *destination,
	const void *wire_request, size_t wire_bytes)
{
	const uint8_t *wire = wire_request;
	uint32_t features, flags;
	uint64_t capacity, frames;

	if (!destination || !wire || wire_bytes != SPF_DDR_RING_REQUEST_BYTES)
		return -EINVAL;
	if (get_le32(wire) != SPF_DDR_RING_REQUEST_MAGIC ||
		get_le16(wire + 4) != SPF_DDR_RING_REQUEST_VERSION ||
		get_le16(wire + 6) != SPF_DDR_RING_REQUEST_BYTES)
		return -EPROTONOSUPPORT;
	features = get_le32(wire + 8);
	flags = get_le32(wire + 12);
	capacity = get_le64(wire + 16);
	frames = get_le64(wire + 24);
	if (features != SPF_DDR_RING_FEATURE_QUEUE_IQ)
		return -EPROTONOSUPPORT;
	if (flags != SPF_DDR_RING_FLAG_FINITE &&
		flags != SPF_DDR_RING_FLAG_CONTINUOUS &&
		flags != SPF_DDR_RING_FLAG_DIRECT_EXTENSION)
		return -EINVAL;
	if (!capacity || get_le64(wire + 32) || get_le64(wire + 40))
		return -EINVAL;
	if ((flags == SPF_DDR_RING_FLAG_FINITE) != (frames != 0U))
		return -EINVAL;

	memset(destination, 0, sizeof(*destination));
	destination->required_features = features;
	destination->flags = flags;
	destination->capacity_iq_bytes = capacity;
	destination->capture_frames = frames;
	return 0;
}
