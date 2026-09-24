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

int iiod_buffer_metadata_buffer_opened(void *provider_context,
		unsigned int kernel_buffers_count)
{
	(void)provider_context;
	(void)kernel_buffers_count;
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

ssize_t iiod_buffer_metadata_status(void *provider_context,
		void *status, size_t status_capacity)
{
	(void)provider_context;
	(void)status;
	(void)status_capacity;
	return -ENODATA;
}

int iiod_buffer_metadata_cancel(void *provider_context)
{
	(void)provider_context;
	return -ENODATA;
}

int iiod_buffer_metadata_describe_frame(void *provider_context,
		const void *metadata, size_t metadata_bytes,
		struct iiod_buffer_metadata_frame_info *info)
{
	(void)provider_context;
	(void)metadata;
	(void)metadata_bytes;
	(void)info;
	return -ENOSYS;
}

int iiod_buffer_metadata_rebase_frame(void *provider_context,
		void *metadata, size_t metadata_bytes,
		uint64_t previous_frame_end)
{
	(void)provider_context;
	(void)metadata;
	(void)metadata_bytes;
	(void)previous_frame_end;
	return -ENOSYS;
}

bool iiod_buffer_metadata_scan_enabled(void *provider_context)
{
	(void)provider_context;
	return false;
}

int iiod_buffer_metadata_scan_start(void *provider_context)
{
	(void)provider_context;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_feed(void *provider_context,
		struct iio_buffer_block *block, size_t raw_bytes)
{
	(void)provider_context;
	(void)block;
	(void)raw_bytes;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_take(void *provider_context,
		struct spf_scan_session_output *output)
{
	(void)provider_context;
	(void)output;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_complete(void *provider_context, uint64_t visit)
{
	(void)provider_context;
	(void)visit;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_abort(void *provider_context, uint64_t visit,
		int transport_error)
{
	(void)provider_context;
	(void)visit;
	(void)transport_error;
	return -EOPNOTSUPP;
}

enum spf_scan_feedback_result iiod_buffer_metadata_scan_feedback(
		void *provider_context, const struct spf_scan_feedback *feedback)
{
	(void)provider_context;
	(void)feedback;
	return SPF_SCAN_REJECTED;
}

int iiod_buffer_metadata_scan_take_ack(void *provider_context,
		struct spf_scan_ack *ack)
{
	(void)provider_context;
	(void)ack;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_terminal(void *provider_context,
		struct spf_scan_terminal *terminal)
{
	(void)provider_context;
	(void)terminal;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_cancel(void *provider_context)
{
	(void)provider_context;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_time(void *provider_context,
		const struct spf_scan_time_query *query, struct spf_scan_time *result)
{
	(void)provider_context; (void)query; (void)result;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_scan_capabilities(void *wire, size_t bytes, uint16_t version)
{
	(void)wire;
	(void)bytes;
	(void)version;
	return -EOPNOTSUPP;
}

int iiod_buffer_metadata_feedback(void *context, const void *feedback, size_t bytes)
{ (void)context; (void)feedback; (void)bytes; return -ENOSYS; }
