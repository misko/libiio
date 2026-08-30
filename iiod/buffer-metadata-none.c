/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "buffer-metadata.h"

#include <errno.h>

int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		size_t scan_bytes,
		const void *request, size_t request_bytes,
		void **provider_context, size_t *extra_samples,
		struct iiod_buffer_burst_plan *burst_plan)
{
	(void)dev;
	(void)samples_count;
	(void)mask;
	(void)words;
	(void)scan_bytes;
	(void)request;
	(void)request_bytes;
	(void)provider_context;
	(void)extra_samples;
	(void)burst_plan;
	return -ENOSYS;
}

int iiod_buffer_metadata_buffer_opening(void *provider_context,
		unsigned int kernel_buffers_count)
{
	(void)provider_context;
	(void)kernel_buffers_count;
	return -ENOSYS;
}

int iiod_buffer_metadata_buffer_opened(void *provider_context)
{
	(void)provider_context;
	return -ENOSYS;
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

void iiod_buffer_metadata_ring_prefix_complete(void *provider_context,
	bool complete)
{
	(void)provider_context;
	(void)complete;
}

void iiod_buffer_metadata_close(void *provider_context)
{
	(void)provider_context;
}

ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes)
{
	(void)provider_context;
	(void)dev;
	(void)buffer;
	(void)raw_bytes;
	(void)metadata;
	(void)metadata_capacity;
	(void)iq_offset;
	(void)iq_bytes;
	return -ENOSYS;
}

int iiod_buffer_metadata_get_failure(void *provider_context,
	struct iiod_buffer_failure *failure)
{
	(void)provider_context;
	if (!failure)
		return -EINVAL;
	*failure = (struct iiod_buffer_failure){0};
	return 0;
}
