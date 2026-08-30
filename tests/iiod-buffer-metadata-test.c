/* Focused transport-test provider; never used by production builds. */
#include "buffer-metadata.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct test_metadata {
	uint32_t magic;
	uint32_t bytes;
	uint64_t sequence;
};

static const uint8_t expected_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x00,
};

static const uint8_t failure_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x01,
};

static const uint8_t before_failure_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x02,
};

static const uint8_t after_failure_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x03,
};

static const uint8_t legacy_overflow_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x04,
};

static const uint8_t legacy_timeout_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x05,
};

static const uint8_t v2_success_session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x06,
};

#define TEST_BURST_REQUEST_BYTES 32U
#define TEST_RING_REQUEST_BYTES 48U

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

struct test_context {
	uint64_t sequence;
	uint8_t failure_stage;
	struct iiod_buffer_failure failure;
	bool buffer_prepared;
	bool buffer_opened;
};

int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		size_t scan_bytes,
		const void *request, size_t request_bytes,
		void **provider_context, size_t *extra_samples,
		struct iiod_buffer_burst_plan *burst_plan)
{
	struct test_context *context;
	const uint8_t *wire = request;
	size_t base_bytes = request_bytes;
	(void)dev;
	(void)samples_count;
	(void)mask;
	(void)words;
	(void)scan_bytes;
	if (!request || !provider_context || !extra_samples || !burst_plan)
		return -EINVAL;
	memset(burst_plan, 0, sizeof(*burst_plan));
	if (request_bytes == sizeof(expected_session_request) +
			TEST_BURST_REQUEST_BYTES) {
		const uint8_t *burst = wire + sizeof(expected_session_request);
		if (get_le32(burst) != UINT32_C(0x42524653) ||
			get_le16(burst + 4) != 1 ||
			get_le16(burst + 6) != TEST_BURST_REQUEST_BYTES ||
			get_le32(burst + 8) != 1 || get_le32(burst + 12) ||
			!get_le64(burst + 16) || get_le64(burst + 24))
			return -EINVAL;
		burst_plan->requested_iq_bytes = get_le64(burst + 16);
		burst_plan->metadata_capacity = sizeof(struct test_metadata);
		base_bytes = sizeof(expected_session_request);
	} else if (request_bytes == sizeof(expected_session_request) +
			TEST_RING_REQUEST_BYTES) {
		const uint8_t *ring = wire + sizeof(expected_session_request);
		const uint32_t flags = get_le32(ring + 12);
		const uint64_t capture_frames = get_le64(ring + 24);
		if (get_le32(ring) != UINT32_C(0x52524653) ||
			get_le16(ring + 4) != 1 ||
			get_le16(ring + 6) != TEST_RING_REQUEST_BYTES ||
			get_le32(ring + 8) != 1 ||
			(flags != 1 && flags != 2) || !get_le64(ring + 16) ||
			((flags == 1) != (capture_frames != 0)) ||
			get_le64(ring + 32) || get_le64(ring + 40))
			return -EINVAL;
		burst_plan->ring_capacity_iq_bytes = get_le64(ring + 16);
		burst_plan->ring_capture_frames = capture_frames;
		burst_plan->ring_flags = flags;
		burst_plan->metadata_capacity = sizeof(struct test_metadata);
		base_bytes = sizeof(expected_session_request);
	}
	if (base_bytes != sizeof(expected_session_request) ||
		(memcmp(request, expected_session_request, base_bytes) &&
		 memcmp(request, failure_session_request, base_bytes) &&
		 memcmp(request, before_failure_session_request, base_bytes) &&
		 memcmp(request, after_failure_session_request, base_bytes) &&
		 memcmp(request, legacy_overflow_session_request, base_bytes) &&
		 memcmp(request, legacy_timeout_session_request, base_bytes) &&
		 memcmp(request, v2_success_session_request, base_bytes)))
		return -EINVAL;
	context = calloc(1, sizeof(*context));
	if (!context)
		return -ENOMEM;
	if (!memcmp(request, failure_session_request, base_bytes))
		context->failure_stage = 1;
	else if (!memcmp(request, before_failure_session_request, base_bytes))
		context->failure_stage = 2;
	else if (!memcmp(request, after_failure_session_request, base_bytes))
		context->failure_stage = 3;
	else if (!memcmp(request, legacy_overflow_session_request, base_bytes))
		context->failure_stage = 4;
	else if (!memcmp(request, legacy_timeout_session_request, base_bytes))
		context->failure_stage = 5;
	else if (!memcmp(request, v2_success_session_request, base_bytes))
		context->failure_stage = 6;
	if (burst_plan->ring_capacity_iq_bytes && context->failure_stage &&
			(context->failure_stage <= 3 || context->failure_stage == 6))
		burst_plan->ring_status_version = 2;
	*provider_context = context;
	*extra_samples = 0;
	return 0;
}

int iiod_buffer_metadata_buffer_opening(void *provider_context,
		unsigned int kernel_buffers_count)
{
	struct test_context *context = provider_context;

	if (!context || !kernel_buffers_count || context->buffer_prepared ||
		context->buffer_opened)
		return -EINVAL;
	context->buffer_prepared = true;
	return 0;
}

int iiod_buffer_metadata_buffer_opened(void *provider_context)
{
	struct test_context *context = provider_context;

	if (!context || !context->buffer_prepared || context->buffer_opened)
		return -EINVAL;
	context->buffer_opened = true;
	return 0;
}

int iiod_buffer_metadata_before_refill(void *provider_context)
{
	struct test_context *context = provider_context;
	if (context && context->failure_stage == 2) {
		context->failure = (struct iiod_buffer_failure){
			.kind = IIOD_BUFFER_FAILURE_GAIN_EVENT_OVERFLOW,
			.valid_fields = IIOD_BUFFER_FAILURE_VALID_FRAME,
			.frame_index = 0,
		};
		return -EOVERFLOW;
	}
	if (context && context->failure_stage == 4)
		return -EOVERFLOW;
	if (context && context->failure_stage == 5)
		return -ETIMEDOUT;
	return 0;
}

int iiod_buffer_metadata_after_refill(void *provider_context)
{
	struct test_context *context = provider_context;
	if (context && context->failure_stage == 3) {
		context->failure = (struct iiod_buffer_failure){
			.kind = IIOD_BUFFER_FAILURE_GAIN_EVENT_GAP,
			.valid_fields = IIOD_BUFFER_FAILURE_VALID_FRAME,
			.frame_index = 0,
		};
		return -EILSEQ;
	}
	return 0;
}

void iiod_buffer_metadata_ring_prefix_complete(void *provider_context,
	bool complete)
{
	(void)provider_context;
	(void)complete;
}

void iiod_buffer_metadata_close(void *provider_context)
{
	free(provider_context);
}

ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes)
{
	struct test_context *context = provider_context;
	struct test_metadata record;
	(void)dev;
	(void)buffer;
	if (!context || !context->buffer_opened || !metadata ||
		metadata_capacity < sizeof(record) ||
		!iq_offset || !iq_bytes)
		return -EINVAL;
	if (context->failure_stage == 1 && context->sequence == 1) {
		context->failure = (struct iiod_buffer_failure){
			.kind = IIOD_BUFFER_FAILURE_METADATA_PROTOCOL,
			.valid_fields = IIOD_BUFFER_FAILURE_VALID_FRAME |
				IIOD_BUFFER_FAILURE_VALID_SAMPLE,
			.frame_index = 1,
			.sample_sequence = 0,
		};
		return -EIO;
	}
	record.magic = UINT32_C(0x54454d49); /* IMET */
	record.bytes = sizeof(record);
	record.sequence = context->sequence++;
	memcpy(metadata, &record, sizeof(record));
	*iq_offset = 0;
	*iq_bytes = raw_bytes;
	return sizeof(record);
}

int iiod_buffer_metadata_get_failure(void *provider_context,
	struct iiod_buffer_failure *failure)
{
	struct test_context *context = provider_context;
	if (!context || !failure)
		return -EINVAL;
	*failure = context->failure;
	return 0;
}
