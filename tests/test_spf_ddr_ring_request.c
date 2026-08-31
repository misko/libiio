#include "spf-ddr-ring-request.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
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

static void put_le64(uint8_t *destination, uint64_t value)
{
	put_le32(destination, (uint32_t)value);
	put_le32(destination + 4, (uint32_t)(value >> 32));
}

static void build_request(uint8_t wire[SPF_DDR_RING_REQUEST_BYTES],
	uint32_t flags, uint64_t capacity, uint64_t frames)
{
	memset(wire, 0, SPF_DDR_RING_REQUEST_BYTES);
	put_le32(wire, SPF_DDR_RING_REQUEST_MAGIC);
	put_le16(wire + 4, SPF_DDR_RING_REQUEST_VERSION);
	put_le16(wire + 6, SPF_DDR_RING_REQUEST_BYTES);
	put_le32(wire + 8, SPF_DDR_RING_FEATURE_QUEUE_IQ);
	put_le32(wire + 12, flags);
	put_le64(wire + 16, capacity);
	put_le64(wire + 24, frames);
}

int main(void)
{
	uint8_t wire[SPF_DDR_RING_REQUEST_BYTES];
	struct spf_ddr_ring_request request;

	build_request(wire, SPF_DDR_RING_FLAG_FINITE, UINT64_C(200000000), 900);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == 0);
	assert(request.flags == SPF_DDR_RING_FLAG_FINITE);
	assert(request.capacity_iq_bytes == UINT64_C(200000000));
	assert(request.capture_frames == 900);
	build_request(wire, SPF_DDR_RING_FLAG_CONTINUOUS, UINT64_C(300000000), 0);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == 0);
	build_request(wire, SPF_DDR_RING_FLAG_DIRECT_EXTENSION,
		UINT64_C(100000000), 0);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == 0);
	assert(request.flags == SPF_DDR_RING_FLAG_DIRECT_EXTENSION);

	assert(spf_ddr_ring_request_decode(NULL, wire, sizeof(wire)) == -EINVAL);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire) - 1U) ==
		-EINVAL);
	wire[4] = 2;
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) ==
		-EPROTONOSUPPORT);
	build_request(wire, SPF_DDR_RING_FLAG_CONTINUOUS, 1, 0);
	put_le32(wire + 8, SPF_DDR_RING_FEATURE_QUEUE_IQ | UINT32_C(2));
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) ==
		-EPROTONOSUPPORT);
	build_request(wire, 0, 1, 0);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, SPF_DDR_RING_FLAG_FINITE, 1, 0);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, SPF_DDR_RING_FLAG_CONTINUOUS, 1, 1);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, SPF_DDR_RING_FLAG_DIRECT_EXTENSION, 1, 1);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, SPF_DDR_RING_FLAG_CONTINUOUS, 0, 0);
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, SPF_DDR_RING_FLAG_CONTINUOUS, 1, 0);
	wire[32] = 1;
	assert(spf_ddr_ring_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	return 0;
}
