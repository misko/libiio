#include "spf-ddr-burst-request.h"

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

static void build_request(uint8_t wire[SPF_DDR_BURST_REQUEST_BYTES],
	uint64_t requested_iq_bytes)
{
	memset(wire, 0, SPF_DDR_BURST_REQUEST_BYTES);
	put_le32(wire, SPF_DDR_BURST_REQUEST_MAGIC);
	put_le16(wire + 4, SPF_DDR_BURST_REQUEST_VERSION);
	put_le16(wire + 6, SPF_DDR_BURST_REQUEST_BYTES);
	put_le32(wire + 8, SPF_DDR_BURST_FEATURE_CACHE_IQ);
	put_le64(wire + 16, requested_iq_bytes);
}

int main(void)
{
	uint8_t wire[SPF_DDR_BURST_REQUEST_BYTES];
	struct spf_ddr_burst_request request;

	build_request(wire, UINT64_C(300000000));
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire)) == 0);
	assert(request.required_features == SPF_DDR_BURST_FEATURE_CACHE_IQ);
	assert(request.requested_iq_bytes == UINT64_C(300000000));
	assert(spf_ddr_burst_request_decode(NULL, wire, sizeof(wire)) == -EINVAL);
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire) - 1U) ==
		-EINVAL);

	build_request(wire, 1);
	wire[4] = 2;
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire)) ==
		-EPROTONOSUPPORT);
	build_request(wire, 1);
	put_le32(wire + 8, SPF_DDR_BURST_FEATURE_CACHE_IQ | UINT32_C(2));
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire)) ==
		-EPROTONOSUPPORT);
	build_request(wire, 1);
	wire[12] = 1;
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, 1);
	wire[24] = 1;
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire)) == -EINVAL);
	build_request(wire, 0);
	assert(spf_ddr_burst_request_decode(&request, wire, sizeof(wire)) == -EINVAL);

	assert(spf_ddr_burst_validate_frame_period(0, 25000000) == -EINVAL);
	assert(spf_ddr_burst_validate_frame_period(200000, 0) == -EINVAL);
	assert(spf_ddr_burst_validate_frame_period(300000, 25000000) == 0);
	assert(spf_ddr_burst_validate_frame_period(299999, 25000000) ==
		-EOPNOTSUPP);
	assert(spf_ddr_burst_validate_frame_period(250000, 25000000) ==
		-EOPNOTSUPP);
	assert(spf_ddr_burst_validate_frame_period(125000, 5000000) == 0);
	assert(spf_ddr_burst_validate_frame_period(125000, 20500000) ==
		-EOPNOTSUPP);
	assert(spf_ddr_burst_validate_frame_period(1000000, 61440000) == 0);
	return 0;
}
