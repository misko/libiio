/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-metadata-request.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static size_t valid_request(uint8_t *wire, uint16_t transport_kind)
{
	uint16_t transport_bytes = 0;

	if (transport_kind == SPF_METADATA_TRANSPORT_BURST)
		transport_bytes = 32;
	else if (transport_kind == SPF_METADATA_TRANSPORT_RING)
		transport_bytes = 48;
	memset(wire, 0, 32 + 104 + 48);
	put_le32(wire, SPF_METADATA_REQUEST_MAGIC);
	put_le16(wire + 4, SPF_METADATA_REQUEST_VERSION);
	put_le16(wire + 6, SPF_METADATA_REQUEST_HEADER_BYTES);
	put_le32(wire + 8, SPF_METADATA_REQUIRED_FEATURES);
	put_le16(wire + 12, SPF_METADATA_RECORD_VERSION);
	put_le16(wire + 14, transport_kind);
	put_le16(wire + 16, SPF_METADATA_REQUEST_TANDEM_BYTES);
	put_le16(wire + 18, transport_bytes);
	return 32U + 104U + transport_bytes;
}

static void test_valid_forms(void)
{
	uint8_t wire[32 + 104 + 48];
	struct spf_metadata_request decoded;
	const uint16_t kinds[] = {
		SPF_METADATA_TRANSPORT_ORDINARY,
		SPF_METADATA_TRANSPORT_BURST,
		SPF_METADATA_TRANSPORT_RING,
	};

	for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
		const size_t bytes = valid_request(wire, kinds[index]);

		assert(spf_metadata_request_is_envelope(wire, bytes));
		assert(spf_metadata_request_decode(&decoded, wire, bytes) == 0);
		assert(decoded.required_features == SPF_METADATA_REQUIRED_FEATURES);
		assert(decoded.record_version == SPF_METADATA_RECORD_VERSION);
		assert(decoded.transport_kind == kinds[index]);
		assert(decoded.tandem_request == wire + 32);
		assert(decoded.tandem_request_bytes == 104);
		assert(decoded.transport_request == wire + 32 + 104);
		assert(decoded.transport_request_bytes == bytes - 32 - 104);
	}
}

static void test_exact_rejections(void)
{
	uint8_t wire[32 + 104 + 49];
	struct spf_metadata_request decoded;
	size_t bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);

	assert(!spf_metadata_request_is_envelope(NULL, bytes));
	assert(!spf_metadata_request_is_envelope(wire, 3));
	assert(spf_metadata_request_decode(NULL, wire, bytes) == -EINVAL);
	assert(spf_metadata_request_decode(&decoded, NULL, bytes) == -EINVAL);
	for (size_t length = 0; length < 32; ++length)
		assert(spf_metadata_request_decode(&decoded, wire, length) == -EINVAL);
	assert(spf_metadata_request_decode(&decoded, wire, bytes - 1) == -EINVAL);
	assert(spf_metadata_request_decode(&decoded, wire, bytes + 1) == -EINVAL);

	put_le32(wire, 0);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le16(wire + 4, 2);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le16(wire + 6, 28);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);

	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le32(wire + 8, SPF_METADATA_REQUIRED_FEATURES &
		~SPF_METADATA_FEATURE_TYPED_CAPTURE_ERRORS);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le32(wire + 8, SPF_METADATA_REQUIRED_FEATURES | UINT32_C(0x10));
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le16(wire + 12, 6);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le16(wire + 14, 3);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le16(wire + 16, 103);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) ==
		-EPROTONOSUPPORT);

	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_ORDINARY);
	put_le16(wire + 18, 32);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) == -EINVAL);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_BURST);
	put_le16(wire + 18, 48);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) == -EINVAL);
	bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
	put_le16(wire + 18, 32);
	assert(spf_metadata_request_decode(&decoded, wire, bytes) == -EINVAL);

	for (size_t offset = 20; offset < 32; offset += 4) {
		bytes = valid_request(wire, SPF_METADATA_TRANSPORT_RING);
		put_le32(wire + offset, 1);
		assert(spf_metadata_request_decode(&decoded, wire, bytes) == -EINVAL);
	}
}

int main(void)
{
	test_valid_forms();
	test_exact_rejections();
	puts("SPF metadata request tests passed");
	return 0;
}
