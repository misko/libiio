// SPDX-License-Identifier: LGPL-2.1-or-later
#include "iio-private.h"
#include "iiod-client.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FRAMES 64U
#define TEST_IQ_BYTES 16U
#define TEST_METADATA_BYTES 16U

struct test_metadata {
	uint32_t magic;
	uint32_t bytes;
	uint64_t sequence;
};

struct transport_state {
	uint8_t responses[TEST_FRAMES * 64U];
	size_t response_bytes;
	size_t response_offset;
	char writes[1024];
	size_t write_bytes;
	unsigned int write_calls;
	unsigned int cancel_calls;
	unsigned int close_calls;
	unsigned int event;
	unsigned int cancel_event;
	unsigned int close_event;
	bool short_write;
	struct iiod_client *client;
};

struct fixture {
	struct iio_context context;
	struct iio_device device;
	struct iio_channel channel;
	struct iio_channel *channels[1];
	uint32_t device_mask[1];
	char *context_attrs[1];
	char *context_values[1];
	struct transport_state state;
};

static struct transport_state *transport_state(struct iio_context_pdata *pdata)
{
	return (struct transport_state *)(void *)pdata;
}

static ssize_t mock_write(struct iio_context_pdata *pdata,
		struct iiod_client_pdata *desc, const char *src, size_t len)
{
	struct transport_state *state = transport_state(pdata);
	size_t accepted = state->short_write && len ? len - 1U : len;
	(void)desc;

	assert(state->write_bytes + accepted <= sizeof(state->writes));
	memcpy(state->writes + state->write_bytes, src, accepted);
	state->write_bytes += accepted;
	state->write_calls++;
	return (ssize_t)accepted;
}

static ssize_t mock_read(struct iio_context_pdata *pdata,
		struct iiod_client_pdata *desc, char *dst, size_t len)
{
	struct transport_state *state = transport_state(pdata);
	(void)desc;

	if (len > state->response_bytes - state->response_offset)
		return -EPIPE;
	memcpy(dst, state->responses + state->response_offset, len);
	state->response_offset += len;
	return (ssize_t)len;
}

static ssize_t mock_read_line(struct iio_context_pdata *pdata,
		struct iiod_client_pdata *desc, char *dst, size_t len)
{
	struct transport_state *state = transport_state(pdata);
	size_t bytes = 0;
	(void)desc;

	while (bytes < len && state->response_offset < state->response_bytes) {
		dst[bytes] = (char)state->responses[state->response_offset++];
		if (dst[bytes++] == '\n')
			return (ssize_t)bytes;
	}
	return bytes ? (ssize_t)bytes : -EPIPE;
}

static const struct iiod_client_ops client_ops = {
	.write = mock_write,
	.read = mock_read,
	.read_line = mock_read_line,
};

static void append_bytes(struct transport_state *state,
		const void *source, size_t bytes)
{
	assert(state->response_bytes + bytes <= sizeof(state->responses));
	memcpy(state->responses + state->response_bytes, source, bytes);
	state->response_bytes += bytes;
}

static void build_responses(struct transport_state *state)
{
	static const char iq_line[] = "16\n";
	static const char mask[] = "00000001\n";
	static const char metadata_line[] = "16\n";
	uint64_t sequence;

	for (sequence = 0; sequence < TEST_FRAMES; sequence++) {
		struct test_metadata metadata = {
			.magic = UINT32_C(0x51525053),
			.bytes = sizeof(metadata),
			.sequence = sequence,
		};
		uint8_t iq[TEST_IQ_BYTES];

		memset(iq, (int)sequence, sizeof(iq));
		append_bytes(state, iq_line, sizeof(iq_line) - 1U);
		append_bytes(state, mask, sizeof(mask) - 1U);
		append_bytes(state, metadata_line, sizeof(metadata_line) - 1U);
		append_bytes(state, &metadata, sizeof(metadata));
		append_bytes(state, iq, sizeof(iq));
	}
}

static struct transport_state *device_state(const struct iio_device *dev)
{
	return (struct transport_state *)(void *)dev->pdata;
}

static int fake_prequeue_async(const struct iio_device *dev, size_t len,
		size_t metadata_capacity, unsigned int frames)
{
	struct transport_state *state = device_state(dev);

	return iiod_client_prequeue_metadata_reads_async_unlocked(state->client,
		(struct iiod_client_pdata *)(void *)state, dev, len,
		metadata_capacity, frames);
}

static ssize_t fake_read(const struct iio_device *dev, void *dst, size_t len,
		uint32_t *mask, size_t words, void *metadata,
		size_t metadata_capacity, size_t *metadata_bytes,
		unsigned int request_frames)
{
	struct transport_state *state = device_state(dev);

	assert(request_frames == 0);
	return iiod_client_read_with_metadata_batch_unlocked(state->client,
		(struct iiod_client_pdata *)(void *)state, dev, dst, len, mask,
		words, metadata, metadata_capacity, metadata_bytes, 0);
}

static void fake_cancel(const struct iio_device *dev)
{
	struct transport_state *state = device_state(dev);

	state->cancel_calls++;
	state->cancel_event = ++state->event;
}

static int fake_close(const struct iio_device *dev)
{
	struct transport_state *state = device_state(dev);

	state->close_calls++;
	state->close_event = ++state->event;
	return 0;
}

static const struct iio_backend_ops backend_ops = {
	.close = fake_close,
	.cancel = fake_cancel,
	.read_with_metadata_batch = fake_read,
	.prequeue_metadata_reads_async = fake_prequeue_async,
};

static struct iio_buffer *fixture_buffer(struct fixture *fixture)
{
	struct iio_buffer *buffer = calloc(1, sizeof(*buffer));

	assert(buffer);
	fixture->context.ops = &backend_ops;
	fixture->context.backend_api_version = IIO_BACKEND_API_V6;
	fixture->context.attrs = fixture->context_attrs;
	fixture->context.values = fixture->context_values;
	fixture->context.nb_attrs = 1;
	fixture->context_attrs[0] = "iio,buffer-direct-async";
	fixture->context_values[0] = "1";
	fixture->device.ctx = &fixture->context;
	fixture->device.pdata =
		(struct iio_device_pdata *)(void *)&fixture->state;
	fixture->device.id = "dev0";
	fixture->device.channels = fixture->channels;
	fixture->device.nb_channels = 1;
	fixture->device.mask = fixture->device_mask;
	fixture->device.words = 1;
	fixture->device_mask[0] = 1;
	fixture->channels[0] = &fixture->channel;
	fixture->channel.dev = &fixture->device;
	fixture->channel.is_scan_element = true;
	fixture->channel.format.length = 64;
	fixture->channel.format.bits = 64;
	fixture->channel.format.repeat = 1;
	fixture->channel.index = 0;
	fixture->channel.number = 0;
	fixture->state.client = iiod_client_new(
		(struct iio_context_pdata *)(void *)&fixture->state, &client_ops);
	assert(fixture->state.client);
	build_responses(&fixture->state);

	buffer->dev = &fixture->device;
	buffer->length = TEST_IQ_BYTES;
	buffer->data_length = buffer->length;
	buffer->buffer = malloc(buffer->length);
	buffer->mask = malloc(sizeof(*buffer->mask));
	assert(buffer->buffer && buffer->mask);
	buffer->mask[0] = 1;
	buffer->dev_sample_size = 8;
	buffer->sample_size = 8;
	buffer->metadata_enabled = true;
	buffer->metadata_batch_size = 1;
	return buffer;
}

static void destroy_fixture_buffer(struct fixture *fixture,
		struct iio_buffer *buffer)
{
	iio_buffer_destroy(buffer);
	iiod_client_destroy(fixture->state.client);
}

static void test_fifo_drain_single_command(void)
{
	static const char command[] = "READBUFMA dev0 16 16 64\r\n";
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 0;
	uint64_t sequence;

	assert(iio_buffer_set_metadata_read_prequeue_async(buffer, TEST_FRAMES,
		TEST_METADATA_BYTES) == 0);
	assert(fixture.state.write_calls == 1);
	assert(fixture.state.write_bytes == sizeof(command) - 1U);
	assert(!memcmp(fixture.state.writes, command, sizeof(command) - 1U));
	for (sequence = 0; sequence < TEST_FRAMES; sequence++) {
		const uint8_t *iq;
		size_t byte;

		assert(iio_buffer_refill_with_metadata(buffer, &metadata,
			sizeof(metadata), &metadata_bytes) == TEST_IQ_BYTES);
		assert(metadata.magic == UINT32_C(0x51525053));
		assert(metadata.sequence == sequence);
		iq = iio_buffer_start(buffer);
		for (byte = 0; byte < TEST_IQ_BYTES; byte++)
			assert(iq[byte] == (uint8_t)sequence);
	}
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -ENODATA);
	assert(fixture.state.response_offset == fixture.state.response_bytes);
	destroy_fixture_buffer(&fixture, buffer);
	assert(fixture.state.cancel_calls == 0);
	assert(fixture.state.close_calls == 1);
}

static void test_gates_and_early_close(void)
{
	struct fixture missing = {0};
	struct iio_buffer *buffer = fixture_buffer(&missing);

	missing.context_values[0] = "0";
	assert(iio_buffer_set_metadata_read_prequeue_async(buffer, 2,
		TEST_METADATA_BYTES) == -EPERM);
	assert(missing.state.write_calls == 0);
	destroy_fixture_buffer(&missing, buffer);

	struct fixture old = {0};
	buffer = fixture_buffer(&old);
	old.context.backend_api_version = IIO_BACKEND_API_V5;
	assert(iio_buffer_set_metadata_read_prequeue_async(buffer, 2,
		TEST_METADATA_BYTES) == -ENOSYS);
	assert(old.state.write_calls == 0);
	destroy_fixture_buffer(&old, buffer);

	struct fixture early = {0};
	buffer = fixture_buffer(&early);
	assert(iio_buffer_set_metadata_read_prequeue_async(buffer, TEST_FRAMES,
		TEST_METADATA_BYTES) == 0);
	destroy_fixture_buffer(&early, buffer);
	assert(early.state.cancel_calls == 1);
	assert(early.state.cancel_event < early.state.close_event);
}

static void test_long_capture_is_one_command(void)
{
	static const char command[] = "READBUFMA dev0 16 16 250\r\n";
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);

	assert(iio_buffer_set_metadata_read_prequeue_async(buffer, 250,
		TEST_METADATA_BYTES) == 0);
	assert(fixture.state.write_calls == 1);
	assert(fixture.state.write_bytes == sizeof(command) - 1U);
	assert(!memcmp(fixture.state.writes, command, sizeof(command) - 1U));
	destroy_fixture_buffer(&fixture, buffer);
}

static void test_direct_capture_limit(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);

	assert(iio_buffer_set_metadata_read_prequeue_async(buffer,
		IIO_BUFFER_METADATA_DIRECT_MAX + 1U, TEST_METADATA_BYTES) == -E2BIG);
	assert(fixture.state.write_calls == 0);
	destroy_fixture_buffer(&fixture, buffer);
}

int main(void)
{
	test_fifo_drain_single_command();
	test_gates_and_early_close();
	test_long_capture_is_one_command();
	test_direct_capture_limit();
	puts("direct async transport: PASS");
	return 0;
}
