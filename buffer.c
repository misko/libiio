// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014-2015 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 */

#include "iio-config.h"
#include "iio-private.h"

#include <errno.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

static bool metadata_batch_is_failed(const struct iio_buffer *buffer)
{
#ifdef _MSC_VER
	return _InterlockedCompareExchange(
		(volatile long *)&buffer->metadata_batch_failed, 0, 0) != 0;
#else
	return __atomic_load_n(&buffer->metadata_batch_failed,
		__ATOMIC_ACQUIRE) != 0;
#endif
}

static bool metadata_batch_mark_failed(struct iio_buffer *buffer)
{
#ifdef _MSC_VER
	return _InterlockedExchange(&buffer->metadata_batch_failed, 1) == 0;
#else
	return __atomic_exchange_n(&buffer->metadata_batch_failed, 1,
		__ATOMIC_ACQ_REL) == 0;
#endif
}

static bool device_is_high_speed(const struct iio_device *dev)
{
	/* Little trick: We call the backend's get_buffer() function, which is
	 * for now only implemented in the Local backend, with a NULL pointer.
	 * It will return -ENOSYS if the device is not high speed, and either
	 * -EBADF or -EINVAL otherwise. */
	const struct iio_backend_ops *ops = dev->ctx->ops;
	return !!ops->get_buffer &&
		(ops->get_buffer(dev, NULL, 0, NULL, 0) != -ENOSYS);
}

static struct iio_buffer * create_buffer(const struct iio_device *dev,
		size_t samples_count, bool cyclic, bool metadata_enabled,
		const void *metadata_request, size_t metadata_request_bytes)
{
	ssize_t ret = -EINVAL;
	struct iio_buffer *buf;
	ssize_t sample_size = iio_device_get_sample_size(dev);

	if (!sample_size || !samples_count)
		goto err_set_errno;

	if (sample_size < 0) {
		ret = sample_size;
		goto err_set_errno;
	}

	buf = malloc(sizeof(*buf));
	if (!buf) {
		ret = -ENOMEM;
		goto err_set_errno;
	}

	buf->dev_sample_size = (unsigned int) sample_size;
	buf->length = sample_size * samples_count;
	buf->dev = dev;
	buf->mask = calloc(dev->words, sizeof(*buf->mask));
	if (!buf->mask) {
		ret = -ENOMEM;
		goto err_free_buf;
	}

	/* Set the default channel mask to the one used by the device.
	 * While input buffers will erase this as soon as the refill function
	 * is used, it is useful for output buffers, as it permits
	 * iio_buffer_foreach_sample to be used. */
	memcpy(buf->mask, dev->mask, dev->words * sizeof(*buf->mask));

	if (metadata_enabled) {
		if (cyclic ||
				dev->ctx->backend_api_version < IIO_BACKEND_API_V3 ||
				!dev->ctx->ops->open_with_metadata) {
			ret = -ENOSYS;
			goto err_free_mask;
		}
		if (!metadata_request || !metadata_request_bytes) {
			ret = -EINVAL;
			goto err_free_mask;
		}
		if (metadata_request_bytes > IIO_BUFFER_METADATA_REQUEST_MAX) {
			ret = -E2BIG;
			goto err_free_mask;
		}
		ret = dev->ctx->ops->open_with_metadata(dev, samples_count, false,
				metadata_request, metadata_request_bytes);
	} else {
		ret = iio_device_open(dev, samples_count, cyclic);
	}
	if (ret < 0)
		goto err_free_mask;
	buf->metadata_enabled = metadata_enabled;

	buf->dev_is_high_speed = device_is_high_speed(dev);
	if (buf->dev_is_high_speed) {
		/* Dequeue the first buffer, so that buf->buffer is correctly
		 * initialized */
		buf->buffer = NULL;
		if (iio_device_is_tx(dev)) {
			ret = dev->ctx->ops->get_buffer(dev, &buf->buffer,
					buf->length, buf->mask, dev->words);
			if (ret < 0)
				goto err_close_device;
		}
	} else {
		buf->buffer = malloc(buf->length);
		if (!buf->buffer) {
			ret = -ENOMEM;
			goto err_close_device;
		}
	}

	ret = iio_device_get_sample_size_mask(dev, buf->mask, dev->words);
	if (ret < 0)
		goto err_close_device;

	buf->sample_size = (unsigned int) ret;
	buf->data_length = buf->length;
	buf->metadata_batch_size = 1;
	buf->metadata_batch_capacity = 0;
	buf->metadata_batch_iq_cache = NULL;
	buf->metadata_batch_metadata_cache = NULL;
	buf->metadata_batch_iq_bytes = NULL;
	buf->metadata_batch_metadata_bytes = NULL;
	buf->metadata_batch_cached_frames = 0;
	buf->metadata_batch_next_frame = 0;
	buf->metadata_direct_frames = 0;
	buf->metadata_direct_pending = 0;
	buf->metadata_direct_capacity = 0;
	buf->metadata_direct_mask = NULL;
	buf->metadata_batch_failed = false;
	buf->block_lease_mode = false;
	return buf;

err_close_device:
	iio_device_close(dev);
err_free_mask:
	free(buf->mask);
err_free_buf:
	free(buf);
err_set_errno:
	errno = -(int)ret;
	return NULL;
}

struct iio_buffer * iio_device_create_buffer(const struct iio_device *dev,
		size_t samples_count, bool cyclic)
{
	return create_buffer(dev, samples_count, cyclic, false, NULL, 0);
}

struct iio_buffer * iio_device_create_buffer_with_metadata(
		const struct iio_device *dev, size_t samples_count,
		const void *request, size_t request_bytes)
{
	return create_buffer(dev, samples_count, false, true,
			request, request_bytes);
}

static void free_metadata_batch_cache(struct iio_buffer *buffer)
{
	free(buffer->metadata_batch_iq_cache);
	free(buffer->metadata_batch_metadata_cache);
	free(buffer->metadata_batch_iq_bytes);
	free(buffer->metadata_batch_metadata_bytes);
	buffer->metadata_batch_iq_cache = NULL;
	buffer->metadata_batch_metadata_cache = NULL;
	buffer->metadata_batch_iq_bytes = NULL;
	buffer->metadata_batch_metadata_bytes = NULL;
	buffer->metadata_batch_capacity = 0;
	buffer->metadata_batch_cached_frames = 0;
	buffer->metadata_batch_next_frame = 0;
}

static void fail_metadata_batch(struct iio_buffer *buffer)
{
	const struct iio_backend_ops *ops = buffer->dev->ctx->ops;
	bool first_failure = metadata_batch_mark_failed(buffer);

	free_metadata_batch_cache(buffer);
	if (first_failure && ops->cancel)
		ops->cancel(buffer->dev);
}

int iio_buffer_set_metadata_batch_size(struct iio_buffer *buffer,
		unsigned int frames)
{
	const struct iio_backend_ops *ops;

	if (!buffer || !buffer->metadata_enabled || !frames)
		return -EINVAL;
	if (frames > IIO_BUFFER_METADATA_BATCH_MAX)
		return -E2BIG;
	if (metadata_batch_is_failed(buffer))
		return -EBADF;
	if (buffer->metadata_batch_cached_frames !=
			buffer->metadata_batch_next_frame)
		return -EBUSY;
	if (buffer->metadata_direct_frames)
		return -EBUSY;

	ops = buffer->dev->ctx->ops;
	if (frames > 1 && (buffer->dev->ctx->backend_api_version <
			IIO_BACKEND_API_V4 ||
			!ops->cancel ||
			(buffer->dev_is_high_speed ?
				!ops->get_buffer_with_metadata_batch :
				!ops->read_with_metadata_batch)))
		return -ENOSYS;

	buffer->metadata_batch_size = frames;
	return 0;
}

int iio_buffer_set_metadata_read_prequeue_async(struct iio_buffer *buffer,
		unsigned int frames, size_t metadata_capacity)
{
	const struct iio_backend_ops *ops;
	const char *capability;
	int ret;

	if (!buffer || !buffer->metadata_enabled || !frames ||
		!metadata_capacity)
		return -EINVAL;
	if (frames > IIO_BUFFER_METADATA_DIRECT_MAX)
		return -E2BIG;
	if (metadata_batch_is_failed(buffer))
		return -EBADF;
	if (buffer->metadata_direct_frames || buffer->metadata_batch_size != 1 ||
		buffer->metadata_batch_cached_frames !=
			buffer->metadata_batch_next_frame)
		return -EBUSY;

	ops = buffer->dev->ctx->ops;
	capability = iio_context_get_attr_value(buffer->dev->ctx,
		"iio,buffer-direct-async");
	if (!capability || strcmp(capability, "1"))
		return -EPERM;
	if (buffer->dev->ctx->backend_api_version < IIO_BACKEND_API_V6 ||
		!ops->cancel || !ops->prequeue_metadata_reads_async ||
		(buffer->dev_is_high_speed ?
			!ops->get_buffer_with_metadata_batch :
			!ops->read_with_metadata_batch))
		return -ENOSYS;

	buffer->metadata_direct_mask = malloc(buffer->dev->words *
		sizeof(*buffer->metadata_direct_mask));
	if (!buffer->metadata_direct_mask)
		return -ENOMEM;
	memcpy(buffer->metadata_direct_mask, buffer->mask,
		buffer->dev->words * sizeof(*buffer->metadata_direct_mask));
	buffer->metadata_direct_frames = frames;
	buffer->metadata_direct_pending = frames;
	buffer->metadata_direct_capacity = metadata_capacity;
	ret = ops->prequeue_metadata_reads_async(buffer->dev, buffer->length,
		metadata_capacity, frames);
	if (ret < 0) {
		fail_metadata_batch(buffer);
		return ret;
	}
	return 0;
}

ssize_t iio_buffer_get_metadata_status(struct iio_buffer *buffer,
		void *status, size_t status_capacity)
{
	const struct iio_backend_ops *ops;

	if (!buffer || !status || !status_capacity)
		return -EINVAL;
	if (!buffer->metadata_enabled)
		return -EINVAL;
	if (buffer->metadata_direct_pending)
		return -EBUSY;
	ops = buffer->dev->ctx->ops;
	if (buffer->dev->ctx->backend_api_version < IIO_BACKEND_API_V5 ||
		!ops->get_buffer_metadata_status)
		return -ENOSYS;
	return ops->get_buffer_metadata_status(buffer->dev, status,
		status_capacity);
}

void iio_buffer_destroy(struct iio_buffer *buffer)
{
	if (buffer->metadata_direct_pending &&
		!metadata_batch_is_failed(buffer))
		fail_metadata_batch(buffer);
	iio_device_close(buffer->dev);
	free_metadata_batch_cache(buffer);
	free(buffer->metadata_direct_mask);
	if (!buffer->dev_is_high_speed)
		free(buffer->buffer);
	free(buffer->mask);
	free(buffer);
}

int iio_buffer_get_poll_fd(struct iio_buffer *buffer)
{
	return iio_device_get_poll_fd(buffer->dev);
}

int iio_buffer_set_blocking_mode(struct iio_buffer *buffer, bool blocking)
{
	return iio_device_set_blocking_mode(buffer->dev, blocking);
}

ssize_t iio_buffer_refill(struct iio_buffer *buffer)
{
	ssize_t read;
	const struct iio_device *dev = buffer->dev;
	ssize_t ret;
	if (buffer->block_lease_mode)
		return -EBUSY;
	if (buffer->metadata_enabled)
		return -EINVAL;

	if (buffer->dev_is_high_speed) {
		read = dev->ctx->ops->get_buffer(dev, &buffer->buffer,
				buffer->length, buffer->mask, dev->words);
	} else {
		read = iio_device_read_raw(dev, buffer->buffer, buffer->length,
				buffer->mask, dev->words);
	}

	if (read >= 0) {
		buffer->data_length = read;
		ret = iio_device_get_sample_size_mask(dev, buffer->mask, dev->words);
		if (ret < 0)
			return ret;
		buffer->sample_size = (unsigned int)ret;
	}
	return read;
}

struct iio_buffer_block *iio_buffer_block_acquire(struct iio_buffer *buffer)
{
	const struct iio_backend_ops *ops;
	struct iio_buffer_block *block;
	int ret;

	if (!buffer) {
		errno = EINVAL;
		return NULL;
	}
	ops = buffer->dev->ctx->ops;
	if (!buffer->dev_is_high_speed ||
		buffer->dev->ctx->backend_api_version < IIO_BACKEND_API_V6 ||
		!ops->acquire_buffer_block || !ops->release_buffer_block) {
		errno = ENOSYS;
		return NULL;
	}
	block = calloc(1, sizeof(*block));
	if (!block) {
		errno = ENOMEM;
		return NULL;
	}
	ret = ops->acquire_buffer_block(buffer->dev, &block->data,
		&block->bytes_used, &block->backend_token);
	if (ret < 0) {
		free(block);
		errno = -ret;
		return NULL;
	}
	block->buffer = buffer;
	buffer->buffer = block->data;
	buffer->data_length = block->bytes_used;
	buffer->block_lease_mode = true;
	return block;
}

void *iio_buffer_block_start(const struct iio_buffer_block *block)
{
	return block ? block->data : NULL;
}

size_t iio_buffer_block_bytes_used(const struct iio_buffer_block *block)
{
	return block ? block->bytes_used : 0;
}

int iio_buffer_block_release(struct iio_buffer_block *block)
{
	struct iio_buffer *buffer;
	const struct iio_backend_ops *ops;
	int ret;

	if (!block || !block->buffer)
		return -EINVAL;
	buffer = block->buffer;
	ops = buffer->dev->ctx->ops;
	ret = ops->release_buffer_block(buffer->dev, block->backend_token,
		block->bytes_used);
	if (ret < 0)
		return ret;
	block->buffer = NULL;
	free(block);
	return 0;
}

ssize_t iio_buffer_refill_with_metadata(struct iio_buffer *buffer,
		void *metadata, size_t metadata_capacity, size_t *metadata_bytes)
{
	ssize_t read, ret;
	const struct iio_device *dev;
	const struct iio_backend_ops *ops;
	unsigned int frame;

	if (!buffer || !metadata || !metadata_capacity || !metadata_bytes)
		return -EINVAL;
	if (!buffer->metadata_enabled)
		return -EINVAL;

	*metadata_bytes = 0;
	dev = buffer->dev;
	ops = dev->ctx->ops;
	if (dev->ctx->backend_api_version < IIO_BACKEND_API_V3)
		return -ENOSYS;
	if (metadata_batch_is_failed(buffer)) {
		free_metadata_batch_cache(buffer);
		return -EBADF;
	}
	if (buffer->metadata_direct_frames) {
		if (!buffer->metadata_direct_pending)
			return -ENODATA;
		if (metadata_capacity != buffer->metadata_direct_capacity ||
			memcmp(buffer->mask, buffer->metadata_direct_mask,
				dev->words * sizeof(*buffer->mask))) {
			fail_metadata_batch(buffer);
			return -EINVAL;
		}
		if (buffer->dev_is_high_speed)
			read = ops->get_buffer_with_metadata_batch(dev,
				&buffer->buffer, buffer->length, buffer->mask,
				dev->words, metadata, metadata_capacity,
				metadata_bytes, 0);
		else
			read = ops->read_with_metadata_batch(dev, buffer->buffer,
				buffer->length, buffer->mask, dev->words, metadata,
				metadata_capacity, metadata_bytes, 0);
		if (read <= 0 || (size_t)read != buffer->length ||
			!*metadata_bytes || *metadata_bytes > metadata_capacity ||
			memcmp(buffer->mask, buffer->metadata_direct_mask,
				dev->words * sizeof(*buffer->mask))) {
			ret = read < 0 ? read : -EIO;
			*metadata_bytes = 0;
			fail_metadata_batch(buffer);
			return ret;
		}
		buffer->metadata_direct_pending--;
		goto update_buffer;
	}

	if (buffer->metadata_batch_cached_frames !=
			buffer->metadata_batch_next_frame) {
		frame = buffer->metadata_batch_next_frame;
		if (metadata_capacity <
				buffer->metadata_batch_metadata_bytes[frame])
			return -ENOSPC;
		if (!buffer->buffer) {
			fail_metadata_batch(buffer);
			return -EIO;
		}
		read = (ssize_t)buffer->metadata_batch_iq_bytes[frame];
		memcpy(buffer->buffer,
			(uint8_t *)buffer->metadata_batch_iq_cache +
				(size_t)frame * buffer->length,
			(size_t)read);
		*metadata_bytes = buffer->metadata_batch_metadata_bytes[frame];
		memcpy(metadata,
			(uint8_t *)buffer->metadata_batch_metadata_cache +
				(size_t)frame * buffer->metadata_batch_capacity,
			*metadata_bytes);
		if (metadata_batch_is_failed(buffer)) {
			*metadata_bytes = 0;
			return -EBADF;
		}
		buffer->metadata_batch_next_frame++;
		if (buffer->metadata_batch_next_frame ==
				buffer->metadata_batch_cached_frames)
			free_metadata_batch_cache(buffer);
		goto update_buffer;
	}

	if (buffer->metadata_batch_size == 1 && buffer->dev_is_high_speed) {
		if (!ops->get_buffer_with_metadata)
			return -ENOSYS;
		read = ops->get_buffer_with_metadata(dev, &buffer->buffer,
				buffer->length, buffer->mask, dev->words,
				metadata, metadata_capacity, metadata_bytes);
	} else if (buffer->metadata_batch_size == 1) {
		if (!ops->read_with_metadata)
			return -ENOSYS;
		read = ops->read_with_metadata(dev, buffer->buffer, buffer->length,
				buffer->mask, dev->words, metadata,
				metadata_capacity, metadata_bytes);
	} else {
		const unsigned int frames = buffer->metadata_batch_size;
		const size_t array_bytes = (size_t)frames * sizeof(size_t) * 2U;
		size_t iq_cache_bytes, metadata_cache_bytes;
		uint32_t *expected_mask;

		if (array_bytes > IIO_BUFFER_METADATA_BATCH_BYTES_MAX ||
			buffer->length >
				(IIO_BUFFER_METADATA_BATCH_BYTES_MAX - array_bytes) / frames)
			return -E2BIG;
		iq_cache_bytes = buffer->length * frames;
		if (metadata_capacity >
				(IIO_BUFFER_METADATA_BATCH_BYTES_MAX - array_bytes -
				 iq_cache_bytes) / frames)
			return -E2BIG;
		metadata_cache_bytes = metadata_capacity * frames;

		buffer->metadata_batch_iq_cache = malloc(iq_cache_bytes);
		buffer->metadata_batch_metadata_cache = malloc(metadata_cache_bytes);
		buffer->metadata_batch_iq_bytes = malloc(
			(size_t)frames * sizeof(*buffer->metadata_batch_iq_bytes));
		buffer->metadata_batch_metadata_bytes = malloc(
			(size_t)frames * sizeof(*buffer->metadata_batch_metadata_bytes));
		expected_mask = malloc(dev->words * sizeof(*expected_mask));
		if (!buffer->metadata_batch_iq_cache ||
			!buffer->metadata_batch_metadata_cache ||
			!buffer->metadata_batch_iq_bytes ||
			!buffer->metadata_batch_metadata_bytes || !expected_mask) {
			free(expected_mask);
			free_metadata_batch_cache(buffer);
			return -ENOMEM;
		}
		memcpy(expected_mask, buffer->mask,
			dev->words * sizeof(*expected_mask));
		buffer->metadata_batch_capacity = metadata_capacity;
		if (metadata_batch_is_failed(buffer)) {
			free(expected_mask);
			free_metadata_batch_cache(buffer);
			return -EBADF;
		}

		for (frame = 0; frame < frames; frame++) {
			size_t frame_metadata_bytes = 0;
			void *frame_metadata =
				(uint8_t *)buffer->metadata_batch_metadata_cache +
				(size_t)frame * metadata_capacity;
			unsigned int request_frames = frame ? 0 : frames;

			if (buffer->dev_is_high_speed)
				read = ops->get_buffer_with_metadata_batch(dev,
					&buffer->buffer, buffer->length, buffer->mask,
					dev->words, frame_metadata, metadata_capacity,
					&frame_metadata_bytes, request_frames);
			else
				read = ops->read_with_metadata_batch(dev,
					buffer->buffer, buffer->length, buffer->mask,
					dev->words, frame_metadata, metadata_capacity,
					&frame_metadata_bytes, request_frames);
			if (metadata_batch_is_failed(buffer)) {
				free(expected_mask);
				free_metadata_batch_cache(buffer);
				return -EBADF;
			}
			if (read <= 0 || (size_t)read != buffer->length ||
				!frame_metadata_bytes ||
				frame_metadata_bytes > metadata_capacity ||
				memcmp(buffer->mask, expected_mask,
					dev->words * sizeof(*expected_mask))) {
				ret = read < 0 ? read : -EIO;
				free(expected_mask);
				fail_metadata_batch(buffer);
				return ret;
			}
			memcpy((uint8_t *)buffer->metadata_batch_iq_cache +
				(size_t)frame * buffer->length,
				buffer->buffer, (size_t)read);
			buffer->metadata_batch_iq_bytes[frame] = (size_t)read;
			buffer->metadata_batch_metadata_bytes[frame] =
				frame_metadata_bytes;
			if (metadata_batch_is_failed(buffer)) {
				free(expected_mask);
				free_metadata_batch_cache(buffer);
				return -EBADF;
			}
		}
		free(expected_mask);
		if (metadata_batch_is_failed(buffer)) {
			free_metadata_batch_cache(buffer);
			return -EBADF;
		}
		buffer->metadata_batch_cached_frames = frames;
		buffer->metadata_batch_next_frame = 0;
		return iio_buffer_refill_with_metadata(buffer, metadata,
			metadata_capacity, metadata_bytes);
	}

update_buffer:
	if (read >= 0 && metadata_batch_is_failed(buffer)) {
		*metadata_bytes = 0;
		return -EBADF;
	}
	if (read >= 0) {
		buffer->data_length = read;
		ret = iio_device_get_sample_size_mask(dev, buffer->mask, dev->words);
		if (ret < 0)
			return ret;
		buffer->sample_size = (unsigned int)ret;
	}

	return read;
}

ssize_t iio_buffer_push(struct iio_buffer *buffer)
{
	const struct iio_device *dev = buffer->dev;
	ssize_t ret;

	if (buffer->dev_is_high_speed) {
		void *buf;
		ret = dev->ctx->ops->get_buffer(dev, &buf,
				buffer->data_length, buffer->mask, dev->words);
		if (ret >= 0) {
			buffer->buffer = buf;
			ret = (ssize_t) buffer->data_length;
		}
	} else {
		void *ptr = buffer->buffer;
		size_t tmp_len;

		/* iio_device_write_raw doesn't guarantee that all bytes are
		 * written */
		for (tmp_len = buffer->data_length; tmp_len; ) {
			ret = iio_device_write_raw(dev, ptr, tmp_len);
			if (ret < 0)
				goto out_reset_data_length;

			tmp_len -= ret;
			ptr = (void *) ((uintptr_t) ptr + ret);
		}

		ret = (ssize_t) buffer->data_length;
	}

out_reset_data_length:
	buffer->data_length = buffer->length;
	return ret;
}

ssize_t iio_buffer_push_partial(struct iio_buffer *buffer, size_t samples_count)
{
	size_t new_len = samples_count * buffer->dev_sample_size;

	if (new_len == 0 || new_len > buffer->length)
		return -EINVAL;

	buffer->data_length = new_len;
	return iio_buffer_push(buffer);
}

ssize_t iio_buffer_foreach_sample(struct iio_buffer *buffer,
		ssize_t (*callback)(const struct iio_channel *,
			void *, size_t, void *), void *d)
{
	uintptr_t ptr = (uintptr_t) buffer->buffer,
		  start = ptr,
		  end = ptr + buffer->data_length;
	const struct iio_device *dev = buffer->dev;
	ssize_t processed = 0;

	if (buffer->sample_size == 0)
		return -EINVAL;

	if (buffer->data_length < buffer->dev_sample_size)
		return 0;

	while (end - ptr >= (size_t) buffer->sample_size) {
		unsigned int i;

		for (i = 0; i < dev->nb_channels; i++) {
			const struct iio_channel *chn = dev->channels[i];
			unsigned int length = chn->format.length / 8;

			if (chn->index < 0)
				break;

			/* Test if the buffer has samples for this channel */
			if (!TEST_BIT(buffer->mask, chn->number))
				continue;

			if ((ptr - start) % length)
				ptr += length - ((ptr - start) % length);

			/* Test if the client wants samples from this channel */
			if (TEST_BIT(dev->mask, chn->number)) {
				ssize_t ret = callback(chn,
						(void *) ptr, length, d);
				if (ret < 0)
					return ret;
				else
					processed += ret;
			}

			if (i == dev->nb_channels - 1 || dev->channels[
					i + 1]->index != chn->index)
				ptr += length * chn->format.repeat;
		}
	}
	return processed;
}

void * iio_buffer_start(const struct iio_buffer *buffer)
{
	return buffer->buffer;
}

void * iio_buffer_first(const struct iio_buffer *buffer,
		const struct iio_channel *chn)
{
	size_t len;
	unsigned int i;
	uintptr_t ptr = (uintptr_t) buffer->buffer,
		  start = ptr;

	if (!iio_channel_is_enabled(chn))
		return iio_buffer_end(buffer);

	for (i = 0; i < buffer->dev->nb_channels; i++) {
		struct iio_channel *cur = buffer->dev->channels[i];
		len = cur->format.length / 8 * cur->format.repeat;

		/* NOTE: dev->channels are ordered by index */
		if (cur->index < 0 || cur->index == chn->index)
			break;

		/* Test if the buffer has samples for this channel */
		if (!TEST_BIT(buffer->mask, cur->number))
			continue;

		/* Two channels with the same index use the same samples */
		if (i > 0 && cur->index == buffer->dev->channels[i - 1]->index)
			continue;

		if ((ptr - start) % len)
			ptr += len - ((ptr - start) % len);
		ptr += len;
	}

	len = chn->format.length / 8;
	if ((ptr - start) % len)
		ptr += len - ((ptr - start) % len);
	return (void *) ptr;
}

ptrdiff_t iio_buffer_step(const struct iio_buffer *buffer)
{
	return (ptrdiff_t) buffer->sample_size;
}

void * iio_buffer_end(const struct iio_buffer *buffer)
{
	return (void *) ((uintptr_t) buffer->buffer + buffer->data_length);
}

void iio_buffer_set_data(struct iio_buffer *buf, void *data)
{
	buf->userdata = data;
}

void * iio_buffer_get_data(const struct iio_buffer *buf)
{
	return buf->userdata;
}

const struct iio_device * iio_buffer_get_device(const struct iio_buffer *buf)
{
	return buf->dev;
}

void iio_buffer_cancel(struct iio_buffer *buf)
{
	const struct iio_backend_ops *ops = buf->dev->ctx->ops;
	bool cancel = true;

	if (buf->metadata_enabled)
		cancel = metadata_batch_mark_failed(buf);

	if (cancel && ops->cancel)
		ops->cancel(buf->dev);
}
