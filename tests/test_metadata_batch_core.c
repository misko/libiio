// SPDX-License-Identifier: LGPL-2.1-or-later
#include "iio-private.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef METADATA_BATCH_TEST_PTHREAD
#include <pthread.h>
#endif

struct test_metadata {
	uint32_t magic;
	uint32_t bytes;
	uint64_t sequence;
};

struct fake_state {
	unsigned int calls;
	unsigned int cancel_calls;
	unsigned int close_calls;
	unsigned int pending;
	unsigned int fail_call;
	unsigned int short_call;
	unsigned int mutate_mask_call;
	unsigned int event;
	unsigned int cancel_event;
	unsigned int close_event;
	uint64_t sequence;
	unsigned int requests[IIO_BUFFER_METADATA_BATCH_MAX];
	struct iio_buffer *buffer_to_cancel;
	unsigned int cancel_in_call;
#ifdef METADATA_BATCH_TEST_PTHREAD
	pthread_mutex_t sync_lock;
	pthread_cond_t sync_cond;
	unsigned int block_call;
	bool sync_initialized;
	bool read_entered;
	bool release_read;
#endif
};

struct fixture {
	struct iio_context context;
	struct iio_device device;
	struct iio_channel channel;
	struct iio_channel *channels[1];
	uint32_t device_mask[1];
	struct fake_state state;
};

static struct fake_state *fake_state(const struct iio_device *dev)
{
	return (struct fake_state *)(void *)dev->pdata;
}

static ssize_t fake_read_batch(const struct iio_device *dev,
		void *dst, size_t len, uint32_t *mask, size_t words,
		void *metadata, size_t metadata_capacity, size_t *metadata_bytes,
		unsigned int request_frames)
{
	struct fake_state *state = fake_state(dev);
	struct test_metadata record;
	unsigned int call = ++state->calls;
	size_t returned = len;

	assert(dst);
	assert(mask && words == 1 && mask[0] == 1);
	assert(metadata && metadata_capacity >= sizeof(record));
	assert(metadata_bytes);
	assert(call <= IIO_BUFFER_METADATA_BATCH_MAX);
	state->requests[call - 1U] = request_frames;
	if (request_frames) {
		assert(state->pending == 0);
		state->pending = request_frames;
	} else {
		assert(state->pending > 0);
	}
	if (state->cancel_in_call == call) {
		state->cancel_in_call = 0;
		iio_buffer_cancel(state->buffer_to_cancel);
	}
#ifdef METADATA_BATCH_TEST_PTHREAD
	if (state->sync_initialized && state->block_call == call) {
		pthread_mutex_lock(&state->sync_lock);
		state->read_entered = true;
		pthread_cond_signal(&state->sync_cond);
		while (!state->release_read)
			pthread_cond_wait(&state->sync_cond, &state->sync_lock);
		pthread_mutex_unlock(&state->sync_lock);
	}
#endif
	if (state->fail_call == call)
		return -EIO;
	if (state->short_call == call) {
		assert(len >= 8);
		returned -= 8;
	}
	if (state->mutate_mask_call == call)
		mask[0] = 0;

	record.magic = UINT32_C(0x5441424d); /* MBAT */
	record.bytes = sizeof(record);
	record.sequence = state->sequence;
	memset(dst, (int)(state->sequence & UINT8_MAX), returned);
	memcpy(metadata, &record, sizeof(record));
	*metadata_bytes = sizeof(record);
	state->sequence++;
	state->pending--;
	return (ssize_t)returned;
}

static void fake_cancel(const struct iio_device *dev)
{
	struct fake_state *state = fake_state(dev);

	state->cancel_calls++;
	state->cancel_event = ++state->event;
#ifdef METADATA_BATCH_TEST_PTHREAD
	if (state->sync_initialized) {
		pthread_mutex_lock(&state->sync_lock);
		state->release_read = true;
		pthread_cond_signal(&state->sync_cond);
		pthread_mutex_unlock(&state->sync_lock);
	}
#endif
}

static int fake_close(const struct iio_device *dev)
{
	struct fake_state *state = fake_state(dev);

	state->close_calls++;
	state->close_event = ++state->event;
	if (state->pending)
		assert(state->cancel_event &&
			state->cancel_event < state->close_event);
	return 0;
}

static const struct iio_backend_ops fake_ops = {
	.close = fake_close,
	.read_with_metadata_batch = fake_read_batch,
	.cancel = fake_cancel,
};

static const struct iio_backend_ops fake_ops_without_cancel = {
	.close = fake_close,
	.read_with_metadata_batch = fake_read_batch,
};

static struct iio_buffer *fixture_buffer(struct fixture *fixture)
{
	struct iio_buffer *buffer = calloc(1, sizeof(*buffer));

	assert(buffer);
	fixture->context.ops = &fake_ops;
	fixture->context.backend_api_version = IIO_BACKEND_API_V4;
	fixture->device.ctx = &fixture->context;
	fixture->device.pdata =
		(struct iio_device_pdata *)(void *)&fixture->state;
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

	buffer->dev = &fixture->device;
	buffer->length = 16;
	buffer->data_length = buffer->length;
	buffer->buffer = malloc(buffer->length);
	buffer->mask = malloc(sizeof(*buffer->mask));
	assert(buffer->buffer && buffer->mask);
	buffer->mask[0] = 1;
	buffer->dev_sample_size = 8;
	buffer->sample_size = 8;
	buffer->metadata_enabled = true;
	buffer->metadata_batch_size = 1;
	fixture->state.buffer_to_cancel = buffer;
	return buffer;
}

static void assert_frame(struct iio_buffer *buffer,
		const struct test_metadata *metadata, uint64_t sequence)
{
	const uint8_t *iq = iio_buffer_start(buffer);

	assert(metadata->magic == UINT32_C(0x5441424d));
	assert(metadata->bytes == sizeof(*metadata));
	assert(metadata->sequence == sequence);
	for (size_t i = 0; i < buffer->length; i++)
		assert(iq[i] == (uint8_t)sequence);
}

static void test_cache_replay(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 0;
	ssize_t ret;

	assert(iio_buffer_set_metadata_batch_size(buffer, 0) == -EINVAL);
	assert(iio_buffer_set_metadata_batch_size(buffer,
		IIO_BUFFER_METADATA_BATCH_MAX + 1U) == -E2BIG);
	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);

	ret = iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes);
	assert(ret == (ssize_t)buffer->length);
	assert(metadata_bytes == sizeof(metadata));
	assert(fixture.state.calls == 3);
	assert(fixture.state.requests[0] == 3);
	assert(fixture.state.requests[1] == 0);
	assert(fixture.state.requests[2] == 0);
	assert_frame(buffer, &metadata, 0);
	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == -EBUSY);

	metadata_bytes = sizeof(metadata);
	ret = iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata) - 1U, &metadata_bytes);
	assert(ret == -ENOSPC);
	assert(metadata_bytes == 0);
	assert(fixture.state.calls == 3);

	ret = iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes);
	assert(ret == (ssize_t)buffer->length);
	assert_frame(buffer, &metadata, 1);
	assert(fixture.state.calls == 3);
	ret = iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes);
	assert(ret == (ssize_t)buffer->length);
	assert_frame(buffer, &metadata, 2);
	assert(fixture.state.calls == 3);

	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == 0);
	ret = iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes);
	assert(ret == (ssize_t)buffer->length);
	assert_frame(buffer, &metadata, 3);
	assert(fixture.state.calls == 5);
	assert(fixture.state.pending == 0);

	/* Frame 4 is already in host memory. Destroy may discard it and issue a
	 * normal CLOSE because no wire response remains unread. */
	iio_buffer_destroy(buffer);
	assert(fixture.state.cancel_calls == 0);
	assert(fixture.state.close_calls == 1);
}

static void test_partial_failure_poison(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 123;

	fixture.state.fail_call = 2;
	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EIO);
	assert(metadata_bytes == 0);
	assert(fixture.state.calls == 2);
	assert(fixture.state.pending == 2);
	assert(fixture.state.cancel_calls == 1);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EBADF);
	assert(fixture.state.calls == 2);

	iio_buffer_destroy(buffer);
	assert(fixture.state.cancel_calls == 1);
	assert(fixture.state.close_calls == 1);
	assert(fixture.state.cancel_event < fixture.state.close_event);
}

static void test_short_response_poison(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 0;

	fixture.state.short_call = 2;
	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EIO);
	assert(metadata_bytes == 0);
	assert(fixture.state.calls == 2);
	assert(fixture.state.cancel_calls == 1);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EBADF);
	iio_buffer_destroy(buffer);
	assert(fixture.state.cancel_calls == 1);
	assert(fixture.state.cancel_event < fixture.state.close_event);
}

static void test_mask_change_poison(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 0;

	fixture.state.mutate_mask_call = 2;
	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EIO);
	assert(metadata_bytes == 0);
	assert(fixture.state.calls == 2);
	assert(fixture.state.cancel_calls == 1);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EBADF);
	iio_buffer_destroy(buffer);
	assert(fixture.state.cancel_calls == 1);
	assert(fixture.state.cancel_event < fixture.state.close_event);
}

static void test_batch_requires_cancel(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);

	fixture.context.ops = &fake_ops_without_cancel;
	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == -ENOSYS);
	assert(fixture.state.calls == 0);
	iio_buffer_destroy(buffer);
	assert(fixture.state.close_calls == 1);
}

static void test_cancel_cached_frames(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 0;

	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == (ssize_t)buffer->length);
	assert_frame(buffer, &metadata, 0);
	assert(fixture.state.calls == 3);

	iio_buffer_cancel(buffer);
	assert(fixture.state.cancel_calls == 1);
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -EBADF);
	assert(metadata_bytes == 0);
	assert(fixture.state.calls == 3);
	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == -EBADF);
	iio_buffer_cancel(buffer);
	assert(fixture.state.cancel_calls == 1);

	iio_buffer_destroy(buffer);
	assert(fixture.state.close_calls == 1);
	assert(fixture.state.cancel_event < fixture.state.close_event);
}

struct refill_result {
	struct iio_buffer *buffer;
	struct test_metadata metadata;
	size_t metadata_bytes;
	ssize_t result;
};

#ifdef METADATA_BATCH_TEST_PTHREAD
static void *refill_thread(void *opaque)
{
	struct refill_result *result = opaque;

	result->result = iio_buffer_refill_with_metadata(result->buffer,
		&result->metadata, sizeof(result->metadata),
		&result->metadata_bytes);
	return NULL;
}
#endif

static void test_cancel_during_drain(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct refill_result result = {.buffer = buffer};

	assert(iio_buffer_set_metadata_batch_size(buffer, 3) == 0);
#ifdef METADATA_BATCH_TEST_PTHREAD
	pthread_t thread;
	assert(pthread_mutex_init(&fixture.state.sync_lock, NULL) == 0);
	assert(pthread_cond_init(&fixture.state.sync_cond, NULL) == 0);
	fixture.state.sync_initialized = true;
	fixture.state.block_call = 1;
	assert(pthread_create(&thread, NULL, refill_thread, &result) == 0);
	pthread_mutex_lock(&fixture.state.sync_lock);
	while (!fixture.state.read_entered)
		pthread_cond_wait(&fixture.state.sync_cond,
			&fixture.state.sync_lock);
	pthread_mutex_unlock(&fixture.state.sync_lock);
	iio_buffer_cancel(buffer);
	assert(pthread_join(thread, NULL) == 0);
#else
	/* The same race boundary is exercised reentrantly on builds without
	 * pthreads: cancellation lands inside the first backend response. */
	fixture.state.cancel_in_call = 1;
	result.result = iio_buffer_refill_with_metadata(result.buffer,
		&result.metadata, sizeof(result.metadata),
		&result.metadata_bytes);
#endif
	assert(result.result == -EBADF);
	assert(result.metadata_bytes == 0);
	assert(fixture.state.calls == 1);
	assert(fixture.state.cancel_calls == 1);
	assert(iio_buffer_refill_with_metadata(buffer, &result.metadata,
		sizeof(result.metadata), &result.metadata_bytes) == -EBADF);
	iio_buffer_destroy(buffer);
	assert(fixture.state.cancel_calls == 1);
	assert(fixture.state.close_calls == 1);
	assert(fixture.state.cancel_event < fixture.state.close_event);
#ifdef METADATA_BATCH_TEST_PTHREAD
	fixture.state.sync_initialized = false;
	assert(pthread_cond_destroy(&fixture.state.sync_cond) == 0);
	assert(pthread_mutex_destroy(&fixture.state.sync_lock) == 0);
#endif
}

static void test_cache_bound(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct test_metadata metadata;
	size_t metadata_bytes = 0;

	assert(iio_buffer_set_metadata_batch_size(buffer, 2) == 0);
	buffer->length = IIO_BUFFER_METADATA_BATCH_BYTES_MAX / 2U;
	assert(iio_buffer_refill_with_metadata(buffer, &metadata,
		sizeof(metadata), &metadata_bytes) == -E2BIG);
	assert(fixture.state.calls == 0);
	buffer->length = 16;
	iio_buffer_destroy(buffer);
}

int main(void)
{
	test_cache_replay();
	test_partial_failure_poison();
	test_short_response_poison();
	test_mask_change_poison();
	test_batch_requires_cancel();
	test_cancel_cached_frames();
	test_cancel_during_drain();
	test_cache_bound();
	puts("metadata batch core: PASS");
	return 0;
}
