/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_LEGACY_METADATA_H
#define SPF_LEGACY_METADATA_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Provider-only request: preserve AD9361 gain control and observe the IQ.
 * Never pass this request to the tandem kernel device. Wire order is LE.
 */
#define SPF_LEGACY_METADATA_REQUEST_BYTES 16U
static inline int spf_legacy_metadata_decode(const void *request, size_t bytes,
	uint32_t *interval, uint16_t *capacity)
{
	const uint8_t *p = request;
	if (!p || bytes != SPF_LEGACY_METADATA_REQUEST_BYTES ||
		p[0] != 'S' || p[1] != 'P' || p[2] != 'F' || p[3] != 'L' ||
		p[4] != 1 || p[5] || p[6] != 16 || p[7] || p[14] || p[15])
		return -EINVAL;
	*interval = (uint32_t)p[8] | (uint32_t)p[9] << 8 |
		(uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
	*capacity = (uint16_t)p[12] | (uint16_t)p[13] << 8;
	if (*interval < 1024 || !*capacity || *capacity > 64)
		return -EINVAL;
	return 0;
}
#endif
