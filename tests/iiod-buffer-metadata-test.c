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

struct test_context {
	uint64_t sequence;
	bool fail_after_first;
};

int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		const void *request, size_t request_bytes,
		void **provider_context, size_t *extra_samples)
{
	struct test_context *context;
	(void)dev;
	(void)samples_count;
	(void)mask;
	(void)words;
	if (!request || request_bytes != sizeof(expected_session_request) ||
		(memcmp(request, expected_session_request, request_bytes) &&
		 memcmp(request, failure_session_request, request_bytes)) ||
		!provider_context || !extra_samples)
		return -EINVAL;
	context = calloc(1, sizeof(*context));
	if (!context)
		return -ENOMEM;
	context->fail_after_first =
		!memcmp(request, failure_session_request, request_bytes);
	*provider_context = context;
	*extra_samples = 0;
	return 0;
}

int iiod_buffer_metadata_buffer_opened(void *provider_context,
		unsigned int kernel_buffers_count)
{
	(void)provider_context;
	return kernel_buffers_count ? 0 : -EINVAL;
}

int iiod_buffer_metadata_before_refill(void *provider_context)
{
	(void)provider_context;
	return 0;
}

int iiod_buffer_metadata_after_refill(void *provider_context)
{
	(void)provider_context;
	return 0;
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
	if (!context || !metadata || metadata_capacity < sizeof(record) ||
		!iq_offset || !iq_bytes)
		return -EINVAL;
	if (context->fail_after_first && context->sequence == 1)
		return -EIO;
	record.magic = UINT32_C(0x54454d49); /* IMET */
	record.bytes = sizeof(record);
	record.sequence = context->sequence++;
	memcpy(metadata, &record, sizeof(record));
	*iq_offset = 0;
	*iq_bytes = raw_bytes;
	return sizeof(record);
}
