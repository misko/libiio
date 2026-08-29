/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_BUFFER_METADATA_H__
#define __IIOD_BUFFER_METADATA_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct iio_buffer;
struct iio_device;

struct iiod_buffer_burst_plan {
	uint64_t requested_iq_bytes;
	uint64_t ring_capacity_iq_bytes;
	uint64_t ring_capture_frames;
	uint32_t ring_flags;
	size_t metadata_capacity;
};

/*
 * Firmware may replace the default implementation at build time. Requests and
 * records are deliberately opaque to iiOD: the provider alone owns their
 * schemas and the record's exact association with the IIO buffer passed here.
 * The request is nonempty and no larger than
 * IIO_BUFFER_METADATA_REQUEST_MAX. It is valid only for the duration of open;
 * a provider that retains it must copy it.
 */
int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		size_t scan_bytes,
		const void *request, size_t request_bytes,
		void **provider_context, size_t *extra_samples,
		struct iiod_buffer_burst_plan *burst_plan);
int iiod_buffer_metadata_buffer_opened(void *provider_context,
		unsigned int kernel_buffers_count);
int iiod_buffer_metadata_before_refill(void *provider_context);
int iiod_buffer_metadata_after_refill(void *provider_context);
/* A finite DDR ring calls this with true only after its strict admitted prefix
 * is committed. Providers may then encode source gaps instead of rejecting the
 * remaining pressure-limited stream; sealed bursts remain strict throughout.
 */
void iiod_buffer_metadata_ring_prefix_complete(void *provider_context,
	bool complete);
void iiod_buffer_metadata_close(void *provider_context);
ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes);

#endif
