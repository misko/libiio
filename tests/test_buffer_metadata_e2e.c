#include <iio.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct test_metadata {
	uint32_t magic;
	uint32_t bytes;
	uint64_t sequence;
};

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1:30432";
	const bool stock_server = argc > 2 && !strcmp(argv[2], "stock");
	struct iio_context *ctx = iio_create_network_context(host);
	assert(ctx);
	const char *capability =
		iio_context_get_attr_value(ctx, "iio,buffer-metadata");
	if (stock_server)
		assert(!capability);
	else
		assert(capability && !strcmp(capability, "1"));
	struct iio_device *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
	assert(dev);
	for (unsigned int i = 0; i < iio_device_get_channels_count(dev); ++i) {
		struct iio_channel *channel = iio_device_get_channel(dev, i);
		if (iio_channel_is_scan_element(channel))
			iio_channel_enable(channel);
	}
	assert(iio_device_set_kernel_buffers_count(dev, 2) == 0);
	struct iio_buffer *buffer;
	if (stock_server) {
		errno = 0;
		buffer = iio_device_create_buffer_with_metadata(dev, 1024);
		assert(!buffer && errno == ENOSYS);
		buffer = iio_device_create_buffer(dev, 1024, false);
		assert(buffer);
		assert(iio_buffer_refill(buffer) == 1024 * 8);
		iio_buffer_destroy(buffer);
		iio_context_destroy(ctx);
		puts("stock server compatibility: PASS");
		return 0;
	}
	buffer = iio_device_create_buffer_with_metadata(dev, 1024);
	assert(buffer);
	uint8_t metadata[64];
	size_t metadata_bytes = 0;
	ssize_t iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes == 1024 * 8);
	assert(metadata_bytes == sizeof(struct test_metadata));
	const struct test_metadata *record = (const struct test_metadata *)metadata;
	assert(record->magic == UINT32_C(0x54454d49));
	assert(record->bytes == sizeof(*record));
	assert(record->sequence == 0);
	assert(iio_buffer_refill(buffer) == -EINVAL);
	iio_buffer_destroy(buffer);
	iio_context_destroy(ctx);
	puts("buffer metadata transport e2e: PASS");
	return 0;
}
