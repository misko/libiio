/* Focused transport-test provider; never used by production builds. */
#include "buffer-metadata.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct test_metadata {
	uint32_t magic;
	uint32_t bytes;
	uint64_t sequence;
};

int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		void **provider_context, size_t *extra_samples)
{
	uint64_t *sequence;
	(void)dev;
	(void)samples_count;
	(void)mask;
	(void)words;
	if (!provider_context || !extra_samples)
		return -EINVAL;
	sequence = calloc(1, sizeof(*sequence));
	if (!sequence)
		return -ENOMEM;
	*provider_context = sequence;
	*extra_samples = 0;
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
	uint64_t *sequence = provider_context;
	struct test_metadata record;
	(void)dev;
	(void)buffer;
	if (!sequence || !metadata || metadata_capacity < sizeof(record) ||
		!iq_offset || !iq_bytes)
		return -EINVAL;
	record.magic = UINT32_C(0x54454d49); /* IMET */
	record.bytes = sizeof(record);
	record.sequence = (*sequence)++;
	memcpy(metadata, &record, sizeof(record));
	*iq_offset = 0;
	*iq_bytes = raw_bytes;
	return sizeof(record);
}
