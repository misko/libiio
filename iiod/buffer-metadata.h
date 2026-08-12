/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_BUFFER_METADATA_H__
#define __IIOD_BUFFER_METADATA_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct iio_buffer;
struct iio_device;

/*
 * Firmware may replace the default implementation at build time. The record
 * is deliberately opaque to iiOD: the provider alone owns its schema and its
 * exact association with the IIO buffer passed here.
 */
int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		void **provider_context, size_t *extra_samples);
void iiod_buffer_metadata_close(void *provider_context);
ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes);

#endif
