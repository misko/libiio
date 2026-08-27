#include <iio.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
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

static const uint8_t session_request[] = {
	0x53, 0x50, 0x46, 0x54, /* SPFT */
	0x01, 0x00, 0x08, 0x00,
};

static const uint8_t unsupported_session_request[] = {
	0x53, 0x50, 0x46, 0x54,
	0x02, 0x00, 0x08, 0x00,
};

static const uint8_t failure_session_request[] = {
	0x53, 0x50, 0x46, 0x54,
	0x01, 0x00, 0x08, 0x01,
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
		assert(!capability || (strcmp(capability, "2") &&
			strcmp(capability, "3")));
	else
		assert(capability && !strcmp(capability, "3"));
	if (!stock_server) {
		const char *layouts = iio_context_get_attr_value(ctx,
			"iio,buffer-metadata-layouts");
		assert(layouts && !strcmp(layouts,
			"00000003:1:4:2,0000000c:1:4:2,0000000f:2:8:1"));
	}
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
		buffer = iio_device_create_buffer_with_metadata(dev, 1024,
			session_request, sizeof(session_request));
		assert(!buffer && errno == ENOSYS);
		buffer = iio_device_create_buffer(dev, 1024, false);
		assert(buffer);
		assert(iio_buffer_refill(buffer) == 1024 * 8);
		iio_buffer_destroy(buffer);
		iio_context_destroy(ctx);
		puts("stock server compatibility: PASS");
		return 0;
	}
	errno = 0;
	buffer = iio_device_create_buffer_with_metadata(dev, 1024, NULL, 0);
	assert(!buffer && errno == EINVAL);
	errno = 0;
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		session_request, IIO_BUFFER_METADATA_REQUEST_MAX + 1U);
	assert(!buffer && errno == E2BIG);
	errno = 0;
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		unsupported_session_request, sizeof(unsupported_session_request));
	assert(!buffer && errno == EINVAL);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		session_request, sizeof(session_request));
	assert(buffer);
	assert(iio_buffer_set_metadata_batch_size(buffer, 0) == -EINVAL);
	assert(iio_buffer_set_metadata_batch_size(buffer,
		IIO_BUFFER_METADATA_BATCH_MAX + 1U) == -E2BIG);
	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
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
	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == -EBUSY);
	assert(iio_buffer_refill(buffer) == -EINVAL);
	uint8_t short_metadata[sizeof(struct test_metadata) - 1U];
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer, short_metadata,
		sizeof(short_metadata), &metadata_bytes);
	assert(iq_bytes == -ENOSPC);
	assert(metadata_bytes == 0);
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes == 1024 * 8);
	assert(metadata_bytes == sizeof(struct test_metadata));
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 1);
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes == 1024 * 8);
	assert(metadata_bytes == sizeof(struct test_metadata));
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 2);
	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == 0);
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes == 1024 * 8);
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 3);
	/* Response 4 was drained into host memory. Destroying now discards only
	 * cached data, so CLOSE is not queued behind an unread wire response. */
	iio_buffer_destroy(buffer);

	/* A new buffer gets a new provider context. This proves the old session
	 * was closed rather than accidentally retained across connections. */
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		session_request, sizeof(session_request));
	assert(buffer);
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes == 1024 * 8);
	assert(metadata_bytes == sizeof(struct test_metadata));
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 0);
	iio_buffer_destroy(buffer);

	/* A provider error after response zero leaves queued commands on the data
	 * pipe. The batch refill must poison/cancel that pipe, and destroy must not
	 * append CLOSE behind those unread responses. A fresh session must open. */
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		failure_session_request, sizeof(failure_session_request));
	assert(buffer);
	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes < 0);
	assert(metadata_bytes == 0);
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == -EBADF);
	/* A failed second open must return EBUSY without changing the old
	 * transport's poison; destroying it must still avoid wire CLOSE. */
	errno = 0;
	struct iio_buffer *busy = iio_device_create_buffer_with_metadata(dev, 1024,
		session_request, sizeof(session_request));
	assert(!busy);
	assert(errno == EBUSY);
	iio_buffer_destroy(buffer);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		session_request, sizeof(session_request));
	assert(buffer);
	metadata_bytes = 0;
	iq_bytes = iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes);
	assert(iq_bytes == 1024 * 8);
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 0);
	iio_buffer_destroy(buffer);
	iio_context_destroy(ctx);
	puts("buffer metadata transport e2e: PASS");
	return 0;
}
