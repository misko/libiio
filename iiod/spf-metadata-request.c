/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-metadata-request.h"

#include "spf-ddr-burst-request.h"
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

bool spf_metadata_request_is_envelope(const void *wire_request,
	size_t wire_bytes)
{
	return wire_request && wire_bytes >= sizeof(uint32_t) &&
		get_le32(wire_request) == SPF_METADATA_REQUEST_MAGIC;
}

int spf_metadata_request_decode(struct spf_metadata_request *destination,
	const void *wire_request, size_t wire_bytes)
{
	const uint8_t *wire = wire_request;
	size_t expected_bytes;
	uint32_t required_features;
	uint16_t transport_kind;
	uint16_t transport_bytes;

	if (!destination || !wire || wire_bytes < SPF_METADATA_REQUEST_HEADER_BYTES)
		return -EINVAL;
	if (get_le32(wire) != SPF_METADATA_REQUEST_MAGIC ||
		get_le16(wire + 4) != SPF_METADATA_REQUEST_VERSION ||
		get_le16(wire + 6) != SPF_METADATA_REQUEST_HEADER_BYTES)
		return -EPROTONOSUPPORT;
	required_features = get_le32(wire + 8);
	if (required_features != SPF_METADATA_REQUIRED_FEATURES)
		return -EPROTONOSUPPORT;
	if (get_le16(wire + 12) != SPF_METADATA_RECORD_VERSION)
		return -EPROTONOSUPPORT;
	transport_kind = get_le16(wire + 14);
	if (get_le16(wire + 16) != SPF_METADATA_REQUEST_TANDEM_BYTES)
		return -EPROTONOSUPPORT;
	transport_bytes = get_le16(wire + 18);
	switch (transport_kind) {
	case SPF_METADATA_TRANSPORT_ORDINARY:
		if (transport_bytes != 0)
			return -EINVAL;
		break;
	case SPF_METADATA_TRANSPORT_BURST:
		if (transport_bytes != SPF_DDR_BURST_REQUEST_BYTES)
			return -EINVAL;
		break;
	case SPF_METADATA_TRANSPORT_RING:
		if (transport_bytes != SPF_DDR_RING_REQUEST_BYTES)
			return -EINVAL;
		break;
	default:
		return -EPROTONOSUPPORT;
	}
	if (get_le32(wire + 20) || get_le32(wire + 24) || get_le32(wire + 28))
		return -EINVAL;
	expected_bytes = (size_t)SPF_METADATA_REQUEST_HEADER_BYTES +
		SPF_METADATA_REQUEST_TANDEM_BYTES + transport_bytes;
	if (wire_bytes != expected_bytes)
		return -EINVAL;

	memset(destination, 0, sizeof(*destination));
	destination->required_features = required_features;
	destination->record_version = SPF_METADATA_RECORD_VERSION;
	destination->transport_kind = transport_kind;
	destination->tandem_request = wire + SPF_METADATA_REQUEST_HEADER_BYTES;
	destination->tandem_request_bytes = SPF_METADATA_REQUEST_TANDEM_BYTES;
	destination->transport_request = destination->tandem_request +
		SPF_METADATA_REQUEST_TANDEM_BYTES;
	destination->transport_request_bytes = transport_bytes;
	return 0;
}
