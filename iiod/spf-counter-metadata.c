/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-counter-metadata.h"
#include <errno.h>
#include <string.h>
#define VALID_FLAGS ((UINT32_C(1) << 4) | (UINT32_C(1) << 21))
#define GAP_FLAGS ((UINT32_C(1) << 11) | (UINT32_C(1) << 23))
uint32_t spf_counter_read32(const void *wire)
{
	const uint8_t *p = wire;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t spf_counter_read64(const void *wire)
{
	const uint8_t *p = wire;
	return spf_counter_read32(p) | (uint64_t)spf_counter_read32(p + 4) << 32;
}
static void put32(void *wire, uint32_t value)
{
	uint8_t *p = wire;
	unsigned i;
	for (i = 0; i < 4; ++i)
		p[i] = (uint8_t)(value >> (8 * i));
}
static void put64(void *wire, uint64_t value)
{
	uint8_t *p = wire;
	put32(p, (uint32_t)value);
	put32(p + 4, (uint32_t)(value >> 32));
}
static uint32_t crc32(const uint8_t *p, size_t bytes)
{
	uint32_t crc = UINT32_MAX;
	size_t i;
	unsigned bit;
	for (i = 0; i < bytes; ++i) {
		crc ^= p[i];
		for (bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1)));
	}
	return ~crc;
}
int spf_counter_request_decode(const void *wire, size_t bytes, size_t samples, uint32_t scan_mask,
			       struct spf_counter_request *request)
{
	const uint8_t *p = wire;
	if (!p || !request || bytes != SPF_COUNTER_REQUEST_BYTES)
		return -EINVAL;
	if (spf_counter_read32(p) != SPF_COUNTER_REQUEST_MAGIC ||
	    spf_counter_read32(p + 4) != ((SPF_COUNTER_REQUEST_BYTES << 16) | 1))
		return -EPROTONOSUPPORT;
	if (spf_counter_read32(p + 8) != SPF_COUNTER_FEATURES || spf_counter_read32(p + 12) != 3 ||
	    scan_mask != 3 || spf_counter_read32(p + 24) || spf_counter_read32(p + 28))
		return -EINVAL;
	request->sample_rate_hz = spf_counter_read32(p + 16);
	request->samples_per_channel = spf_counter_read32(p + 20);
	if (!request->sample_rate_hz || request->sample_rate_hz > 61440000U || !samples ||
	    samples > UINT32_MAX / 4 || (samples & 1) || request->samples_per_channel != samples)
		return -EINVAL;
	return 0;
}
int spf_counter_frame_build(void *wire, size_t capacity, uint64_t stream, uint64_t sequence,
			    uint64_t first, uint64_t missing, uint32_t samples, uint32_t rate)
{
	uint8_t *p = wire;
	if (!p || capacity < SPF_COUNTER_FRAME_BYTES)
		return -ENOSPC;
	if (!stream || !samples || (samples & 1) || samples > UINT32_MAX / 4 || !rate ||
	    rate > 61440000U || first > UINT64_MAX - samples || missing > first)
		return -EINVAL;
	memset(p, 0, SPF_COUNTER_FRAME_BYTES);
	put32(p, SPF_COUNTER_FRAME_MAGIC);
	put32(p + 4, (SPF_COUNTER_FRAME_BYTES << 16) | 1);
	put32(p + 8, SPF_COUNTER_FEATURES);
	put32(p + 12, VALID_FLAGS | (missing ? GAP_FLAGS : 0));
	put64(p + 16, stream);
	put64(p + 24, sequence);
	put64(p + 32, first);
	put64(p + 40, missing);
	put32(p + 48, samples);
	put32(p + 52, samples * 4);
	put32(p + 56, 3);
	put32(p + 60, rate);
	put32(p + 64, 2);
	put32(p + 76, crc32(p, 76));
	return SPF_COUNTER_FRAME_BYTES;
}
int spf_counter_frame_describe(const void *wire, size_t bytes,
			       struct iiod_buffer_metadata_frame_info *info)
{
	const uint8_t *p = wire;
	uint32_t samples, rate;
	uint64_t first, missing;
	if (!p || !info || bytes != SPF_COUNTER_FRAME_BYTES)
		return -EBADMSG;
	samples = spf_counter_read32(p + 48);
	rate = spf_counter_read32(p + 60);
	first = spf_counter_read64(p + 32);
	missing = spf_counter_read64(p + 40);
	if (spf_counter_read32(p) != SPF_COUNTER_FRAME_MAGIC ||
	    spf_counter_read32(p + 4) != ((SPF_COUNTER_FRAME_BYTES << 16) | 1) ||
	    spf_counter_read32(p + 8) != SPF_COUNTER_FEATURES ||
	    spf_counter_read32(p + 12) != (VALID_FLAGS | (missing ? GAP_FLAGS : 0)) ||
	    !spf_counter_read64(p + 16) || !samples || (samples & 1) || samples > UINT32_MAX / 4 ||
	    first > UINT64_MAX - samples || missing > first ||
	    spf_counter_read32(p + 52) != samples * 4 || spf_counter_read32(p + 56) != 3 || !rate ||
	    rate > 61440000U || spf_counter_read32(p + 64) != 2 || spf_counter_read32(p + 68) ||
	    spf_counter_read32(p + 72) || spf_counter_read32(p + 76) != crc32(p, 76))
		return -EBADMSG;
	info->first_sample_sequence = first;
	info->frame_end = first + samples;
	info->missing_samples_before = missing;
	return 0;
}
int spf_counter_frame_rebase(void *wire, size_t bytes, uint64_t previous_end)
{
	uint8_t *p = wire;
	struct iiod_buffer_metadata_frame_info info;
	int ret = spf_counter_frame_describe(wire, bytes, &info);
	if (ret)
		return ret;
	if (info.first_sample_sequence < previous_end)
		return -ERANGE;
	put64(p + 40, info.first_sample_sequence - previous_end);
	put32(p + 12, VALID_FLAGS | (info.first_sample_sequence != previous_end ? GAP_FLAGS : 0));
	put32(p + 76, crc32(p, 76));
	return 0;
}
