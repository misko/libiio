/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-ddr-burst-request.h"

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

int spf_ddr_burst_request_decode(struct spf_ddr_burst_request *destination,
	const void *wire_request, size_t wire_bytes)
{
	const uint8_t *wire = wire_request;
	uint32_t required_features;

	if (!destination || !wire || wire_bytes != SPF_DDR_BURST_REQUEST_BYTES)
		return -EINVAL;
	if (get_le32(wire) != SPF_DDR_BURST_REQUEST_MAGIC ||
		get_le16(wire + 4) != SPF_DDR_BURST_REQUEST_VERSION ||
		get_le16(wire + 6) != SPF_DDR_BURST_REQUEST_BYTES)
		return -EPROTONOSUPPORT;
	required_features = get_le32(wire + 8);
	if (required_features != SPF_DDR_BURST_FEATURE_CACHE_IQ)
		return -EPROTONOSUPPORT;
	if (get_le32(wire + 12) || get_le64(wire + 24))
		return -EINVAL;
	memset(destination, 0, sizeof(*destination));
	destination->required_features = required_features;
	destination->requested_iq_bytes = get_le64(wire + 16);
	if (!destination->requested_iq_bytes)
		return -EINVAL;
	return 0;
}

int spf_ddr_burst_validate_frame_period(uint32_t samples_per_channel,
	uint32_t sample_rate_hz)
{
	uint64_t frame_sample_us;
	uint64_t minimum_sample_us;

	if (!samples_per_channel || !sample_rate_hz)
		return -EINVAL;
	frame_sample_us = (uint64_t)samples_per_channel * UINT64_C(1000000);
	minimum_sample_us = (uint64_t)sample_rate_hz *
		SPF_DDR_BURST_MIN_FRAME_DURATION_US;
	return frame_sample_us >= minimum_sample_us ? 0 : -EOPNOTSUPP;
}
