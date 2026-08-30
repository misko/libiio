#include "spf-ddr-ring-status.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
	uint8_t wire[SPF_DDR_RING_STATUS_BYTES];
	uint64_t exclusive_boundary;
	struct spf_ddr_ring_status decoded;
	const struct spf_ddr_ring_status status = {
		.version = SPF_DDR_RING_STATUS_VERSION_V1,
		.state = SPF_DDR_RING_STATE_FAILED,
		.terminal_reason = SPF_DDR_RING_REASON_COUNTER_GAP,
		.valid_fields = SPF_DDR_RING_STATUS_VALID_LAST_CONTIGUOUS |
			SPF_DDR_RING_STATUS_VALID_FIRST_UNAVAILABLE,
		.error_code = -EOVERFLOW,
		.requested_capacity_iq_bytes = UINT64_C(200000000),
		.admitted_capacity_iq_bytes = UINT64_C(200000000),
		.target_frames = 900,
		.produced_frames = 401,
		.consumed_frames = 351,
		.high_water_frames = 50,
		.wrap_count = 8,
		.producer_position = 1,
		.consumer_position = 1,
		.last_contiguous_sample_sequence = UINT64_C(0x100000001),
		.first_unavailable_sample_sequence = UINT64_C(0x100100001),
	};

	assert(spf_ddr_ring_status_encode(wire, sizeof(wire), &status) == 0);
	assert(spf_ddr_ring_status_decode(&decoded, wire, sizeof(wire)) == 0);
	assert(decoded.version == SPF_DDR_RING_STATUS_VERSION_V1);
	assert(decoded.state == status.state);
	assert(decoded.terminal_reason == status.terminal_reason);
	assert(decoded.valid_fields == status.valid_fields);
	assert(decoded.error_code == status.error_code);
	assert(decoded.produced_frames == status.produced_frames);
	assert(decoded.last_contiguous_sample_sequence ==
		status.last_contiguous_sample_sequence);
	assert(decoded.first_unavailable_sample_sequence ==
		status.first_unavailable_sample_sequence);
	assert(decoded.failure_frame_index == 0);
	assert(decoded.failure_sample_sequence == 0);
	assert(spf_ddr_ring_status_encode(NULL, sizeof(wire), &status) == -EINVAL);
	assert(spf_ddr_ring_status_decode(NULL, wire, sizeof(wire)) == -EINVAL);
	assert(spf_ddr_ring_status_decode(&decoded, wire, sizeof(wire) - 1U) ==
		-EINVAL);
	wire[4] = 3;
	assert(spf_ddr_ring_status_decode(&decoded, wire, sizeof(wire)) ==
		-EPROTONOSUPPORT);
	assert(spf_ddr_ring_status_encode(wire, sizeof(wire),
		&(struct spf_ddr_ring_status){
			.version = SPF_DDR_RING_STATUS_VERSION_V1,
			.state = SPF_DDR_RING_STATE_RUNNING,
			.produced_frames = 1,
			.consumed_frames = 2,
		}) == -EINVAL);
	const struct spf_ddr_ring_status v2 = {
		.version = SPF_DDR_RING_STATUS_VERSION_V2,
		.state = SPF_DDR_RING_STATE_FAILED,
		.terminal_reason = SPF_DDR_RING_REASON_GAIN_EVENT_GAP,
		.valid_fields = SPF_DDR_RING_STATUS_VALID_FAILURE_FRAME |
			SPF_DDR_RING_STATUS_VALID_FAILURE_SAMPLE,
		.error_code = -EILSEQ,
		.produced_frames = 7,
		.failure_frame_index = 7,
		.failure_sample_sequence = UINT64_C(0x100000123),
	};
	assert(spf_ddr_ring_status_encode(wire, sizeof(wire), &v2) == 0);
	assert(wire[4] == SPF_DDR_RING_STATUS_VERSION_V2 && wire[5] == 0);
	assert(spf_ddr_ring_status_decode(&decoded, wire, sizeof(wire)) == 0);
	assert(decoded.version == SPF_DDR_RING_STATUS_VERSION_V2);
	assert(decoded.terminal_reason == SPF_DDR_RING_REASON_GAIN_EVENT_GAP);
	assert(decoded.failure_frame_index == 7);
	assert(decoded.failure_sample_sequence == UINT64_C(0x100000123));
	assert(spf_ddr_ring_status_encode(wire, sizeof(wire),
		&(struct spf_ddr_ring_status){
			.version = SPF_DDR_RING_STATUS_VERSION_V2,
			.state = SPF_DDR_RING_STATE_RUNNING,
			.valid_fields = SPF_DDR_RING_STATUS_VALID_FAILURE_FRAME,
		}) == -EINVAL);
	assert(spf_ddr_ring_status_encode(wire, sizeof(wire),
		&(struct spf_ddr_ring_status){
			.version = SPF_DDR_RING_STATUS_VERSION_V2,
			.state = SPF_DDR_RING_STATE_FAILED,
			.terminal_reason = SPF_DDR_RING_REASON_METADATA_PROTOCOL,
			.error_code = -EPROTO,
			.failure_frame_index = 1,
		}) == -EINVAL);
	assert(spf_ddr_ring_status_encode(wire, sizeof(wire),
		&(struct spf_ddr_ring_status){
			.version = SPF_DDR_RING_STATUS_VERSION_V1,
			.state = SPF_DDR_RING_STATE_FAILED,
			.terminal_reason = SPF_DDR_RING_REASON_COUNTER_GAP,
			.valid_fields = SPF_DDR_RING_STATUS_VALID_FAILURE_FRAME,
			.error_code = -EOVERFLOW,
		}) == -EINVAL);
	assert(spf_ddr_ring_exclusive_boundary(1000, 1000000,
		&exclusive_boundary) == 0);
	assert(exclusive_boundary == UINT64_C(1001000));
	assert(spf_ddr_ring_exclusive_boundary(UINT64_MAX - 3U, 3,
		&exclusive_boundary) == 0);
	assert(exclusive_boundary == UINT64_MAX);
	assert(spf_ddr_ring_exclusive_boundary(UINT64_MAX - 3U, 4,
		&exclusive_boundary) == -EOVERFLOW);
	assert(spf_ddr_ring_exclusive_boundary(0, 0, &exclusive_boundary) ==
		-EINVAL);
	assert(spf_ddr_ring_exclusive_boundary(0, 1, NULL) == -EINVAL);
	return 0;
}
