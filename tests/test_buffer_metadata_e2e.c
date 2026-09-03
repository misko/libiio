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

static const uint8_t stale_once_session_request[] = {
	0x53, 0x50, 0x46, 0x54,
	0x01, 0x00, 0x08, 0x02,
};

#define TEST_BURST_REQUEST_BYTES 32U
#define TEST_RING_REQUEST_BYTES 48U

static void put_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)value;
	destination[1] = (uint8_t)(value >> 8);
	destination[2] = (uint8_t)(value >> 16);
	destination[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *destination, uint64_t value)
{
	put_le32(destination, (uint32_t)value);
	put_le32(destination + 4, (uint32_t)(value >> 32));
}

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

static void build_burst_request(uint8_t *wire, const uint8_t *base,
	size_t base_bytes, uint64_t requested_iq_bytes)
{
	uint8_t *burst = wire + base_bytes;

	memcpy(wire, base, base_bytes);
	memset(burst, 0, TEST_BURST_REQUEST_BYTES);
	put_le32(burst, UINT32_C(0x42524653));
	put_le16(burst + 4, 1);
	put_le16(burst + 6, TEST_BURST_REQUEST_BYTES);
	put_le32(burst + 8, 1);
	put_le64(burst + 16, requested_iq_bytes);
}

static void build_ring_request(uint8_t *wire, const uint8_t *base,
	size_t base_bytes, uint64_t capacity_iq_bytes, uint64_t capture_frames)
{
	uint8_t *ring = wire + base_bytes;

	memcpy(wire, base, base_bytes);
	memset(ring, 0, TEST_RING_REQUEST_BYTES);
	put_le32(ring, UINT32_C(0x52524653));
	put_le16(ring + 4, 1);
	put_le16(ring + 6, TEST_RING_REQUEST_BYTES);
	put_le32(ring + 8, 1);
	put_le32(ring + 12, capture_frames ? 1 : 2);
	put_le64(ring + 16, capacity_iq_bytes);
	put_le64(ring + 24, capture_frames);
}

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
		assert(!strcmp(iio_context_get_attr_value(ctx,
			"iio,buffer-ddr-burst"), "1"));
		assert(!strcmp(iio_context_get_attr_value(ctx,
			"iio,buffer-ddr-burst-max-iq-bytes"), "200000000"));
		assert(!strcmp(iio_context_get_attr_value(ctx,
			"iio,buffer-ddr-ring"), "1"));
		assert(!strcmp(iio_context_get_attr_value(ctx,
			"iio,buffer-ddr-ring-max-iq-bytes"), "200000000"));
		assert(!strcmp(iio_context_get_attr_value(ctx,
			"iio,buffer-metadata-status"), "1"));
		assert(!strcmp(iio_context_get_attr_value(ctx,
			"iio,buffer-metadata-timing-log"), "1"));
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

	/* drop-backlog retires a DMA block that has fallen outside metadata
	 * coverage, then still returns the requested number of fresh frames. */
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		stale_once_session_request, sizeof(stale_once_session_request));
	assert(buffer);
	assert(iio_buffer_set_metadata_read_prequeue_async_policy(buffer, 3,
		sizeof(metadata),
		IIO_BUFFER_METADATA_OVERRUN_DROP_BACKLOG) == 0);
	for (uint64_t sequence = 0; sequence < 3; ++sequence) {
		metadata_bytes = 0;
		iq_bytes = iio_buffer_refill_with_metadata(buffer,
			metadata, sizeof(metadata), &metadata_bytes);
		assert(iq_bytes == 1024 * 8);
		assert(metadata_bytes == sizeof(struct test_metadata));
		record = (const struct test_metadata *)metadata;
		assert(record->sequence == sequence);
	}
	iio_buffer_destroy(buffer);

	/* A burst request captures every admitted frame before the first refill,
	 * then drains through the ordinary metadata refill contract. */
	uint8_t burst_request[sizeof(session_request) + TEST_BURST_REQUEST_BYTES];
	build_burst_request(burst_request, session_request,
		sizeof(session_request), UINT64_C(4) * 1024U * 8U);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		burst_request, sizeof(burst_request));
	if (!buffer)
		fprintf(stderr, "burst buffer open failed: %s (%d)\n",
			strerror(errno), errno);
	assert(buffer);
	for (uint64_t sequence = 0; sequence < 4; ++sequence) {
		metadata_bytes = 0;
		iq_bytes = iio_buffer_refill_with_metadata(buffer,
			metadata, sizeof(metadata), &metadata_bytes);
		assert(iq_bytes == 1024 * 8);
		assert(metadata_bytes == sizeof(struct test_metadata));
		record = (const struct test_metadata *)metadata;
		assert(record->sequence == sequence);
	}
	metadata_bytes = 123;
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == -ENODATA);
	assert(metadata_bytes == 0);
	iio_buffer_destroy(buffer);

	/* The ring streams immediately, wraps without overwrite, and terminates
	 * only after the finite target has been delivered. */
	uint8_t ring_request[sizeof(session_request) + TEST_RING_REQUEST_BYTES];
	build_ring_request(ring_request, session_request,
		sizeof(session_request), UINT64_C(2) * 1024U * 8U, 5);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		ring_request, sizeof(ring_request));
	assert(buffer);
	uint8_t ring_status[128];
	assert(iio_buffer_get_metadata_status(buffer, ring_status,
		sizeof(ring_status)) == (ssize_t)sizeof(ring_status));
	assert(get_le32(ring_status) == UINT32_C(0x53524653));
	assert(get_le16(ring_status + 4) == 1 &&
		get_le16(ring_status + 6) == sizeof(ring_status));
	assert(get_le64(ring_status + 24) == UINT64_C(2) * 1024U * 8U);
	assert(get_le64(ring_status + 32) == UINT64_C(2) * 1024U * 8U);
	assert(get_le64(ring_status + 40) == 5);
	for (uint64_t sequence = 0; sequence < 5; ++sequence) {
		metadata_bytes = 0;
		iq_bytes = iio_buffer_refill_with_metadata(buffer,
			metadata, sizeof(metadata), &metadata_bytes);
		assert(iq_bytes == 1024 * 8);
		assert(metadata_bytes == sizeof(struct test_metadata));
		record = (const struct test_metadata *)metadata;
		assert(record->sequence == sequence);
	}
	assert(iio_buffer_get_metadata_status(buffer, ring_status,
		sizeof(ring_status)) == (ssize_t)sizeof(ring_status));
	assert(get_le32(ring_status + 8) == 4);
	assert(get_le32(ring_status + 12) == 1);
	assert(get_le64(ring_status + 48) == 5);
	assert(get_le64(ring_status + 56) == 5);
	assert(get_le64(ring_status + 64) <= 2);
	assert(get_le64(ring_status + 72) == 2);
	metadata_bytes = 123;
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == -ENODATA);
	assert(metadata_bytes == 0);
	iio_buffer_destroy(buffer);

	/* A producer failure is ordered after every already-committed frame. */
	build_ring_request(ring_request, failure_session_request,
		sizeof(failure_session_request), UINT64_C(2) * 1024U * 8U, 3);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		ring_request, sizeof(ring_request));
	assert(buffer);
	metadata_bytes = 0;
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == 1024 * 8);
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 0);
	metadata_bytes = 123;
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == -EIO);
	assert(metadata_bytes == 0);
	assert(iio_buffer_get_metadata_status(buffer, ring_status,
		sizeof(ring_status)) == (ssize_t)sizeof(ring_status));
	assert(get_le32(ring_status + 8) == 5);
	assert((int32_t)get_le32(ring_status + 20) == -EIO);
	assert(get_le64(ring_status + 48) == 1);
	assert(get_le64(ring_status + 56) == 1);
	iio_buffer_destroy(buffer);

	/* Continuous mode remains bounded by the same two-slot ring and CLOSE is
	 * the explicit cancellation point. */
	build_ring_request(ring_request, session_request,
		sizeof(session_request), UINT64_C(2) * 1024U * 8U, 0);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		ring_request, sizeof(ring_request));
	assert(buffer);
	for (uint64_t sequence = 0; sequence < 5; ++sequence) {
		metadata_bytes = 0;
		assert(iio_buffer_refill_with_metadata(buffer,
			metadata, sizeof(metadata), &metadata_bytes) == 1024 * 8);
		record = (const struct test_metadata *)metadata;
		assert(record->sequence == sequence);
	}
	iio_buffer_destroy(buffer);

	/* Burst capture failure is atomic: no first frame is exposed, teardown is
	 * reusable, and a fresh ordinary session starts at sequence zero. */
	build_burst_request(burst_request, failure_session_request,
		sizeof(failure_session_request), UINT64_C(3) * 1024U * 8U);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		burst_request, sizeof(burst_request));
	assert(buffer);
	metadata_bytes = 0;
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == -EIO);
	assert(metadata_bytes == 0);
	iio_buffer_destroy(buffer);
	buffer = iio_device_create_buffer_with_metadata(dev, 1024,
		session_request, sizeof(session_request));
	assert(buffer);
	metadata_bytes = 0;
	assert(iio_buffer_refill_with_metadata(buffer,
		metadata, sizeof(metadata), &metadata_bytes) == 1024 * 8);
	record = (const struct test_metadata *)metadata;
	assert(record->sequence == 0);
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
