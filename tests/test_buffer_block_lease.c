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

#define TEST_BLOCKS 3U
#define TEST_BYTES 16U

struct fake_state {
	uint8_t storage[TEST_BLOCKS][TEST_BYTES];
	bool leased[TEST_BLOCKS];
	unsigned int next_block;
	unsigned int acquire_calls;
	unsigned int release_calls;
	unsigned int close_calls;
	bool fail_release;
};

struct fixture {
	struct iio_context context;
	struct iio_device device;
	struct fake_state state;
};

static struct fake_state *fake_state(const struct iio_device *dev)
{
	return (struct fake_state *)(void *)dev->pdata;
}

static int fake_acquire(const struct iio_device *dev, void **addr,
		size_t *bytes_used, uintptr_t *token)
{
	struct fake_state *state = fake_state(dev);
	unsigned int id = state->next_block++;

	if (id >= TEST_BLOCKS)
		return -EAGAIN;
	assert(!state->leased[id]);
	state->leased[id] = true;
	state->acquire_calls++;
	*addr = state->storage[id];
	*bytes_used = TEST_BYTES;
	*token = (uintptr_t)id + 1U;
	return 0;
}

static int fake_release(const struct iio_device *dev, uintptr_t token,
		size_t bytes_used)
{
	struct fake_state *state = fake_state(dev);
	unsigned int id;

	assert(token > 0U);
	id = (unsigned int)(token - 1U);
	assert(id < TEST_BLOCKS);
	assert(bytes_used == TEST_BYTES);
	assert(state->leased[id]);
	state->release_calls++;
	if (state->fail_release) {
		state->fail_release = false;
		return -EIO;
	}
	state->leased[id] = false;
	return 0;
}

static int fake_close(const struct iio_device *dev)
{
	struct fake_state *state = fake_state(dev);
	unsigned int i;

	for (i = 0; i < TEST_BLOCKS; i++)
		assert(!state->leased[i]);
	state->close_calls++;
	return 0;
}

static const struct iio_backend_ops fake_ops = {
	.close = fake_close,
	.acquire_buffer_block = fake_acquire,
	.release_buffer_block = fake_release,
};

static struct iio_buffer *fixture_buffer(struct fixture *fixture)
{
	struct iio_buffer *buffer = calloc(1, sizeof(*buffer));

	assert(buffer);
	fixture->context.ops = &fake_ops;
	fixture->context.backend_api_version = IIO_BACKEND_API_V6;
	fixture->device.ctx = &fixture->context;
	fixture->device.pdata =
		(struct iio_device_pdata *)(void *)&fixture->state;
	buffer->dev = &fixture->device;
	buffer->length = TEST_BYTES;
	buffer->data_length = TEST_BYTES;
	buffer->dev_is_high_speed = true;
	buffer->metadata_batch_size = 1;
	buffer->mask = calloc(1, sizeof(*buffer->mask));
	assert(buffer->mask);
	return buffer;
}

static void test_multiple_leases_and_failed_release_retry(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);
	struct iio_buffer_block *first;
	struct iio_buffer_block *second;

	first = iio_buffer_block_acquire(buffer);
	second = iio_buffer_block_acquire(buffer);
	assert(first && second);
	assert(iio_buffer_block_start(first) == fixture.state.storage[0]);
	assert(iio_buffer_block_start(second) == fixture.state.storage[1]);
	assert(iio_buffer_block_bytes_used(first) == TEST_BYTES);
	assert(iio_buffer_start(buffer) == fixture.state.storage[1]);
	assert(iio_buffer_refill(buffer) == -EBUSY);

	assert(iio_buffer_block_release(first) == 0);
	fixture.state.fail_release = true;
	assert(iio_buffer_block_release(second) == -EIO);
	assert(iio_buffer_block_start(second) == fixture.state.storage[1]);
	assert(iio_buffer_block_release(second) == 0);

	iio_buffer_destroy(buffer);
	assert(fixture.state.acquire_calls == 2);
	assert(fixture.state.release_calls == 3);
	assert(fixture.state.close_calls == 1);
}

static void test_capability_gate(void)
{
	struct fixture fixture = {0};
	struct iio_buffer *buffer = fixture_buffer(&fixture);

	fixture.context.backend_api_version = IIO_BACKEND_API_V5;
	errno = 0;
	assert(!iio_buffer_block_acquire(buffer));
	assert(errno == ENOSYS);
	fixture.context.backend_api_version = IIO_BACKEND_API_V6;
	buffer->dev_is_high_speed = false;
	errno = 0;
	assert(!iio_buffer_block_acquire(buffer));
	assert(errno == ENOSYS);
	iio_buffer_destroy(buffer);
}

int main(void)
{
	test_multiple_leases_and_failed_release_retry();
	test_capability_gate();
	puts("buffer block leases: PASS");
	return 0;
}
