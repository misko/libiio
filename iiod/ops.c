// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 */

#include "ops.h"
#include "parser.h"
#include "thread-pool.h"
#include "buffer-metadata.h"
#include "ddr-ring-core.h"
#include "spf-ddr-ring-request.h"
#include "spf-ddr-ring-status.h"
#include "../debug.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

int yyparse(yyscan_t scanner);

struct DevEntry;

/* Corresponds to a thread reading from a device */
struct ThdEntry {
	SLIST_ENTRY(ThdEntry) parser_list_entry;
	SLIST_ENTRY(ThdEntry) dev_list_entry;
	unsigned int nb, sample_size, samples_count;
	unsigned int metadata_capacity;
	ssize_t err;

	int eventfd;

	struct parser_pdata *pdata;
	struct iio_device *dev;
	struct DevEntry *entry;

	uint32_t *mask;
	bool active, is_writer, new_client, wait_for_open;
	bool metadata_enabled;
	bool async_direct;
	unsigned int async_frames_remaining;
};

#define IIOD_MAX_BUFFER_METADATA_BYTES (64U * 1024U)
#define IIOD_BURST_MAX_IQ_BYTES UINT64_C(200000000)
#define IIOD_BURST_MEMORY_RESERVE_BYTES (UINT64_C(128) * 1024U * 1024U)
#define IIOD_BURST_CMA_RESERVE_BYTES (UINT64_C(16) * 1024U * 1024U)
#define IIOD_BURST_MAX_STARTUP_DISCARDS 8U
#define IIOD_BURST_SEALED_IDLE_SECONDS 30

enum iiod_burst_state {
	IIOD_BURST_OFF = 0,
	IIOD_BURST_RESERVED,
	IIOD_BURST_CAPTURING,
	IIOD_BURST_SEALED,
	IIOD_BURST_DRAINED,
	IIOD_BURST_FAILED,
};

struct iiod_burst_frame {
	size_t metadata_bytes;
};

struct iiod_burst_cache {
	enum iiod_burst_state state;
	struct iiod_burst_frame *frames;
	uint8_t *metadata;
	uint8_t *iq;
	void *mapping;
	size_t mapping_bytes;
	size_t frame_count;
	size_t captured_frames;
	size_t next_frame;
	size_t frame_iq_bytes;
	size_t metadata_capacity;
	uint64_t requested_iq_bytes;
	uint64_t admitted_iq_bytes;
	int error;
	bool reservation_held;
};

struct iiod_ddr_ring_frame {
	size_t metadata_bytes;
};

struct iiod_ddr_ring {
	struct iiod_ddr_ring_core core;
	struct iiod_ddr_ring_frame *frames;
	enum iiod_ddr_ring_slot_state *slots;
	uint8_t *metadata;
	uint8_t *iq;
	void *mapping;
	size_t mapping_bytes;
	size_t frame_iq_bytes;
	size_t metadata_capacity;
	uint64_t requested_iq_bytes;
	uint64_t admitted_iq_bytes;
	bool reservation_held;
	bool producer_started;
	bool producer_exited;
	bool direct_extension;
};

struct iiod_direct_async_frame {
	struct iio_buffer_block *block;
	size_t metadata_bytes;
	size_t iq_offset;
	size_t iq_bytes;
	size_t ring_slot;
	bool ring_backed;
};

struct iiod_direct_async {
	struct iiod_direct_async_frame *frames;
	uint8_t *metadata;
	size_t metadata_capacity;
	size_t capacity;
	size_t dma_capacity;
	size_t dma_count;
	size_t head;
	size_t tail;
	size_t count;
	uint64_t target_frames;
	uint64_t produced_frames;
	uint64_t consumed_frames;
	int error;
	bool requested;
	bool producer_started;
	bool producer_exited;
	bool cancelled;
};

enum iiod_timing_stage {
	IIOD_TIMING_SAMPLER_ADMIT = 0,
	IIOD_TIMING_DMA_REFILL,
	IIOD_TIMING_SAMPLER_FINISH,
	IIOD_TIMING_METADATA_BUILD,
	IIOD_TIMING_DDR_COPY,
	IIOD_TIMING_TRANSPORT_FRAME,
	IIOD_TIMING_TRANSPORT_IQ,
	IIOD_TIMING_RING_PRODUCER_WAIT,
	IIOD_TIMING_RING_CONSUMER_WAIT,
	IIOD_TIMING_STAGE_COUNT,
};

struct iiod_timing_accumulator {
	uint64_t count;
	uint64_t total_ns;
	uint64_t max_ns;
};

struct iiod_stage_timing {
	pthread_mutex_t lock;
	uint64_t first_ns;
	uint64_t last_ns;
	uint64_t transported_iq_bytes;
	uint64_t next_snapshot_frame;
	struct iiod_timing_accumulator stages[IIOD_TIMING_STAGE_COUNT];
};

#define IIOD_TIMING_SNAPSHOT_FRAMES UINT64_C(100)

static pthread_mutex_t burst_reservation_lock = PTHREAD_MUTEX_INITIALIZER;
static bool burst_reserved;
static int rw_cpu_affinity = -1;

void iiod_set_rw_cpu_affinity(int cpu)
{
	rw_cpu_affinity = cpu;
}

/*
 * The local context is shared by every IIOD connection.  Each newly created
 * network context sends its default timeout to that shared context, so an
 * unrelated connection can otherwise shorten an active metadata refill to
 * 1-2.5 seconds.  ABI-1 and ABI-2 accept up to 4,194,304 samples/channel; at
 * the lowest supported AD9361 rate one frame spans just over eight seconds.
 * Keep active metadata DMA dequeues above that duration while retaining a
 * finite 15-second stuck-device bound.  Client disconnect still cancels the
 * buffer immediately.
 */
#define IIOD_METADATA_LOCAL_IO_TIMEOUT_MIN_MS 15000U

static pthread_mutex_t metadata_timeout_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int metadata_timeout_floor_users;

static int metadata_timeout_floor_acquire(struct iio_context *ctx)
{
	int ret;

	pthread_mutex_lock(&metadata_timeout_lock);
	ret = iio_context_set_timeout(ctx,
		IIOD_METADATA_LOCAL_IO_TIMEOUT_MIN_MS);
	if (ret == 0)
		metadata_timeout_floor_users++;
	pthread_mutex_unlock(&metadata_timeout_lock);

	return ret;
}

static void metadata_timeout_floor_release(void)
{
	pthread_mutex_lock(&metadata_timeout_lock);
	if (metadata_timeout_floor_users != 0U)
		metadata_timeout_floor_users--;
	pthread_mutex_unlock(&metadata_timeout_lock);
}

static void thd_entry_event_signal(struct ThdEntry *thd)
{
	uint64_t e = 1;
	int ret;

	do {
		ret = write(thd->eventfd, &e, sizeof(e));
	} while (ret == -1 && errno == EINTR);
}

static int thd_entry_event_wait(struct ThdEntry *thd, pthread_mutex_t *mutex,
	int fd_in)
{
	struct pollfd pfd[3];
	uint64_t e;
	int ret;

	pthread_mutex_unlock(mutex);

	pfd[0].fd = thd->eventfd;
	pfd[0].events = POLLIN;
	pfd[1].fd = fd_in;
	pfd[1].events = POLLRDHUP;
	pfd[2].fd = thread_pool_get_poll_fd(thd->pdata->pool);
	pfd[2].events = POLLIN;

	do {
		poll_nointr(pfd, 3);

		if ((pfd[1].revents & POLLRDHUP) || (pfd[2].revents & POLLIN)) {
			pthread_mutex_lock(mutex);
			return -EPIPE;
		}

		do {
			ret = read(thd->eventfd, &e, sizeof(e));
		} while (ret == -1 && errno == EINTR);
	} while (ret == -1 && errno == EAGAIN);

	pthread_mutex_lock(mutex);

	return 0;
}

static bool size_mul(size_t left, size_t right, size_t *result)
{
	if (right && left > SIZE_MAX / right)
		return false;
	*result = left * right;
	return true;
}

static bool size_add(size_t left, size_t right, size_t *result)
{
	if (left > SIZE_MAX - right)
		return false;
	*result = left + right;
	return true;
}

static bool uint64_add(uint64_t left, uint64_t right, uint64_t *result)
{
	if (left > UINT64_MAX - right)
		return false;
	*result = left + right;
	return true;
}

static bool align_size(size_t value, size_t alignment, size_t *result)
{
	size_t rounded;

	if (!alignment || !size_add(value, alignment - 1U, &rounded))
		return false;
	*result = rounded / alignment * alignment;
	return true;
}

static int read_memory_available(uint64_t *available_bytes,
	uint64_t *cma_free_bytes, bool *has_cma)
{
	char line[160];
	uint64_t available_kib = 0;
	uint64_t cma_free_kib = 0;
	bool found_available = false;
	FILE *stream;

	if (!available_bytes || !cma_free_bytes || !has_cma)
		return -EINVAL;
	stream = fopen("/proc/meminfo", "r");
	if (!stream)
		return -errno;
	while (fgets(line, sizeof(line), stream)) {
		uint64_t value;

		if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &value) == 1) {
			available_kib = value;
			found_available = true;
		} else if (sscanf(line, "CmaFree: %" SCNu64 " kB", &value) == 1) {
			cma_free_kib = value;
			*has_cma = true;
		}
	}
	if (ferror(stream)) {
		int ret = errno ? -errno : -EIO;
		fclose(stream);
		return ret;
	}
	fclose(stream);
	if (!found_available)
		return -ENODATA;
	if (available_kib > UINT64_MAX / 1024U ||
		cma_free_kib > UINT64_MAX / 1024U)
		return -EOVERFLOW;
	*available_bytes = available_kib * 1024U;
	*cma_free_bytes = cma_free_kib * 1024U;
	return 0;
}

static int buffered_reservation_acquire(bool *reservation_held)
{
	int ret = 0;

	if (!reservation_held)
		return -EINVAL;
	pthread_mutex_lock(&burst_reservation_lock);
	if (burst_reserved)
		ret = -EBUSY;
	else {
		burst_reserved = true;
		*reservation_held = true;
	}
	pthread_mutex_unlock(&burst_reservation_lock);
	return ret;
}

static void buffered_reservation_release(bool *reservation_held)
{
	if (!reservation_held || !*reservation_held)
		return;
	pthread_mutex_lock(&burst_reservation_lock);
	burst_reserved = false;
	*reservation_held = false;
	pthread_mutex_unlock(&burst_reservation_lock);
}

static void burst_release_storage(struct iiod_burst_cache *cache)
{
	if (!cache)
		return;
	if (cache->mapping)
		munmap(cache->mapping, cache->mapping_bytes);
	cache->mapping = NULL;
	cache->frames = NULL;
	cache->metadata = NULL;
	cache->iq = NULL;
	cache->mapping_bytes = 0;
	buffered_reservation_release(&cache->reservation_held);
}

static void burst_fail(struct iiod_burst_cache *cache, int error)
{
	burst_release_storage(cache);
	cache->error = error < 0 ? error : -EIO;
	cache->state = IIOD_BURST_FAILED;
}

static int burst_prepare(struct iiod_burst_cache *cache,
	const struct iiod_buffer_burst_plan *plan, size_t frame_iq_bytes,
	size_t raw_frame_bytes, unsigned int kernel_buffers, bool check_cma)
{
	uint64_t available_bytes, cma_free_bytes;
	uint64_t required_available, required_cma;
	size_t descriptor_bytes, metadata_bytes, iq_bytes;
	size_t mapping_bytes, offset;
	long page_size;
	bool has_cma = false;
	volatile uint8_t *pages;
	int ret;

	if (!cache || !plan)
		return -EINVAL;
	if (!plan->requested_iq_bytes) {
		cache->state = IIOD_BURST_OFF;
		return 0;
	}
	if (!frame_iq_bytes || !raw_frame_bytes || !kernel_buffers ||
		!plan->metadata_capacity ||
		plan->metadata_capacity > IIOD_MAX_BUFFER_METADATA_BYTES)
		return -EINVAL;
	if (plan->requested_iq_bytes > IIOD_BURST_MAX_IQ_BYTES)
		return -E2BIG;
	if (plan->requested_iq_bytes > SIZE_MAX)
		return -EOVERFLOW;
	cache->frame_count = (size_t)plan->requested_iq_bytes / frame_iq_bytes;
	if (!cache->frame_count)
		return -ENOSPC;
	if (!size_mul(cache->frame_count, sizeof(*cache->frames),
			&descriptor_bytes) ||
		!align_size(descriptor_bytes, 64U, &descriptor_bytes) ||
		!size_mul(cache->frame_count, plan->metadata_capacity,
			&metadata_bytes) ||
		!size_mul(cache->frame_count, frame_iq_bytes, &iq_bytes) ||
		!size_add(descriptor_bytes, metadata_bytes, &offset) ||
		!size_add(offset, iq_bytes, &mapping_bytes))
		return -EOVERFLOW;
	if (!uint64_add((uint64_t)mapping_bytes,
			IIOD_BURST_MEMORY_RESERVE_BYTES, &required_available))
		return -EOVERFLOW;
	if ((uint64_t)raw_frame_bytes > UINT64_MAX / kernel_buffers)
		return -EOVERFLOW;
	required_cma = (uint64_t)raw_frame_bytes * kernel_buffers;
	if (required_cma > UINT64_MAX - IIOD_BURST_CMA_RESERVE_BYTES)
		return -EOVERFLOW;
	required_cma += IIOD_BURST_CMA_RESERVE_BYTES;

	ret = buffered_reservation_acquire(&cache->reservation_held);
	if (ret)
		return ret;
	ret = read_memory_available(&available_bytes, &cma_free_bytes, &has_cma);
	if (ret)
		goto fail;
	if (!check_cma)
		has_cma = false;
	if (available_bytes < required_available ||
		(has_cma && cma_free_bytes < required_cma)) {
		ret = -ENOMEM;
		goto fail;
	}
	cache->mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (cache->mapping == MAP_FAILED) {
		cache->mapping = NULL;
		ret = -errno;
		goto fail;
	}
	cache->mapping_bytes = mapping_bytes;
#ifdef MADV_DONTDUMP
	(void)madvise(cache->mapping, mapping_bytes, MADV_DONTDUMP);
#endif
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		ret = -EIO;
		goto fail;
	}
	pages = cache->mapping;
	for (offset = 0; offset < mapping_bytes; offset += (size_t)page_size)
		pages[offset] = 0;
	pages[mapping_bytes - 1U] = 0;

	has_cma = false;
	ret = read_memory_available(&available_bytes, &cma_free_bytes, &has_cma);
	if (ret)
		goto fail;
	if (!check_cma)
		has_cma = false;
	if (available_bytes < IIOD_BURST_MEMORY_RESERVE_BYTES ||
		(has_cma && cma_free_bytes < required_cma)) {
		ret = -ENOMEM;
		goto fail;
	}

	cache->frames = cache->mapping;
	cache->metadata = (uint8_t *)cache->mapping + descriptor_bytes;
	cache->iq = cache->metadata + metadata_bytes;
	cache->frame_iq_bytes = frame_iq_bytes;
	cache->metadata_capacity = plan->metadata_capacity;
	cache->requested_iq_bytes = plan->requested_iq_bytes;
	cache->admitted_iq_bytes = iq_bytes;
	cache->state = IIOD_BURST_RESERVED;
	return 0;

fail:
	burst_release_storage(cache);
	return ret;
}

static void *burst_metadata_slot(struct iiod_burst_cache *cache, size_t frame)
{
	return cache->metadata + frame * cache->metadata_capacity;
}

static void *burst_iq_slot(struct iiod_burst_cache *cache, size_t frame)
{
	return cache->iq + frame * cache->frame_iq_bytes;
}

static void ring_release_storage(struct iiod_ddr_ring *ring)
{
	if (!ring)
		return;
	if (ring->mapping)
		munmap(ring->mapping, ring->mapping_bytes);
	ring->mapping = NULL;
	ring->frames = NULL;
	ring->slots = NULL;
	ring->metadata = NULL;
	ring->iq = NULL;
	ring->mapping_bytes = 0;
	buffered_reservation_release(&ring->reservation_held);
}

static int ring_prepare(struct iiod_ddr_ring *ring,
	const struct iiod_buffer_burst_plan *plan, size_t frame_iq_bytes,
	size_t raw_frame_bytes, unsigned int kernel_buffers, bool check_cma)
{
	uint64_t available_bytes, cma_free_bytes;
	uint64_t required_available, required_cma;
	size_t slot_bytes, descriptor_bytes, metadata_bytes, iq_bytes;
	size_t mapping_bytes, offset;
	long page_size;
	bool has_cma = false;
	volatile uint8_t *pages;
	int ret;

	if (!ring || !plan)
		return -EINVAL;
	if (!plan->ring_capacity_iq_bytes)
		return 0;
	if (!frame_iq_bytes || !raw_frame_bytes || !kernel_buffers ||
		!plan->metadata_capacity ||
		plan->metadata_capacity > IIOD_MAX_BUFFER_METADATA_BYTES)
		return -EINVAL;
	if (plan->ring_capacity_iq_bytes > IIOD_BURST_MAX_IQ_BYTES)
		return -E2BIG;
	if (plan->ring_capacity_iq_bytes > SIZE_MAX)
		return -EOVERFLOW;

	const size_t frame_count =
		(size_t)plan->ring_capacity_iq_bytes / frame_iq_bytes;
	if (!frame_count)
		return -ENOSPC;
	if (!size_mul(frame_count, sizeof(*ring->slots), &slot_bytes) ||
		!align_size(slot_bytes, 64U, &slot_bytes) ||
		!size_mul(frame_count, sizeof(*ring->frames), &descriptor_bytes) ||
		!align_size(descriptor_bytes, 64U, &descriptor_bytes) ||
		!size_mul(frame_count, plan->metadata_capacity, &metadata_bytes) ||
		!size_mul(frame_count, frame_iq_bytes, &iq_bytes) ||
		!size_add(slot_bytes, descriptor_bytes, &offset) ||
		!size_add(offset, metadata_bytes, &offset) ||
		!size_add(offset, iq_bytes, &mapping_bytes))
		return -EOVERFLOW;
	if (!uint64_add((uint64_t)mapping_bytes,
			IIOD_BURST_MEMORY_RESERVE_BYTES, &required_available))
		return -EOVERFLOW;
	if ((uint64_t)raw_frame_bytes > UINT64_MAX / kernel_buffers)
		return -EOVERFLOW;
	required_cma = (uint64_t)raw_frame_bytes * kernel_buffers;
	if (required_cma > UINT64_MAX - IIOD_BURST_CMA_RESERVE_BYTES)
		return -EOVERFLOW;
	required_cma += IIOD_BURST_CMA_RESERVE_BYTES;

	ret = buffered_reservation_acquire(&ring->reservation_held);
	if (ret)
		return ret;
	ret = read_memory_available(&available_bytes, &cma_free_bytes, &has_cma);
	if (ret)
		goto fail;
	if (!check_cma)
		has_cma = false;
	if (available_bytes < required_available ||
		(has_cma && cma_free_bytes < required_cma)) {
		ret = -ENOMEM;
		goto fail;
	}
	ring->mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ring->mapping == MAP_FAILED) {
		ring->mapping = NULL;
		ret = -errno;
		goto fail;
	}
	ring->mapping_bytes = mapping_bytes;
#ifdef MADV_DONTDUMP
	(void)madvise(ring->mapping, mapping_bytes, MADV_DONTDUMP);
#endif
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		ret = -EIO;
		goto fail;
	}
	pages = ring->mapping;
	for (offset = 0; offset < mapping_bytes; offset += (size_t)page_size)
		pages[offset] = 0;
	pages[mapping_bytes - 1U] = 0;

	has_cma = false;
	ret = read_memory_available(&available_bytes, &cma_free_bytes, &has_cma);
	if (ret)
		goto fail;
	if (!check_cma)
		has_cma = false;
	if (available_bytes < IIOD_BURST_MEMORY_RESERVE_BYTES ||
		(has_cma && cma_free_bytes < required_cma)) {
		ret = -ENOMEM;
		goto fail;
	}

	ring->slots = ring->mapping;
	ring->frames = (struct iiod_ddr_ring_frame *)
		((uint8_t *)ring->mapping + slot_bytes);
	ring->metadata = (uint8_t *)ring->frames + descriptor_bytes;
	ring->iq = ring->metadata + metadata_bytes;
	ring->frame_iq_bytes = frame_iq_bytes;
	ring->metadata_capacity = plan->metadata_capacity;
	ring->requested_iq_bytes = plan->ring_capacity_iq_bytes;
	ring->admitted_iq_bytes = iq_bytes;
	ring->direct_extension =
		plan->ring_flags == SPF_DDR_RING_FLAG_DIRECT_EXTENSION;
	ret = iiod_ddr_ring_core_init(&ring->core, ring->slots, frame_count,
		plan->ring_capture_frames);
	if (ret)
		goto fail;
	return 0;

fail:
	ring_release_storage(ring);
	return ret;
}

static void *ring_metadata_slot(struct iiod_ddr_ring *ring, size_t frame)
{
	return ring->metadata + frame * ring->metadata_capacity;
}

static void *ring_iq_slot(struct iiod_ddr_ring *ring, size_t frame)
{
	return ring->iq + frame * ring->frame_iq_bytes;
}

static void direct_async_release_storage(struct iiod_direct_async *direct)
{
	size_t i;

	if (!direct)
		return;
	for (i = 0; i < direct->capacity; i++) {
		if (direct->frames && direct->frames[i].block) {
			int ret = iio_buffer_block_release(direct->frames[i].block);

			if (ret)
				IIO_ERROR("Unable to release direct DMA block: %d\n",
					ret);
			direct->frames[i].block = NULL;
		}
	}
	free(direct->metadata);
	free(direct->frames);
	memset(direct, 0, sizeof(*direct));
}

static int direct_async_prepare(struct iiod_direct_async *direct,
		unsigned int dma_capacity, size_t ram_capacity,
		size_t metadata_capacity, unsigned int target_frames)
{
	size_t capacity, metadata_bytes, useful_ram;

	if (!direct || dma_capacity < 2U || !metadata_capacity || !target_frames ||
		metadata_capacity > IIOD_MAX_BUFFER_METADATA_BYTES)
		return -EINVAL;
	if (direct->requested)
		return -EBUSY;
	useful_ram = target_frames > dma_capacity ?
		(size_t)target_frames - dma_capacity : 0U;
	if (useful_ram > ram_capacity)
		useful_ram = ram_capacity;
	if (!size_add(dma_capacity, useful_ram, &capacity))
		return -EOVERFLOW;
	if (!size_mul(capacity, metadata_capacity, &metadata_bytes))
		return -EOVERFLOW;
	direct->frames = calloc(capacity, sizeof(*direct->frames));
	direct->metadata = malloc(metadata_bytes);
	if (!direct->frames || !direct->metadata) {
		direct_async_release_storage(direct);
		return -ENOMEM;
	}
	direct->metadata_capacity = metadata_capacity;
	direct->capacity = capacity;
	direct->dma_capacity = dma_capacity;
	direct->target_frames = target_frames;
	direct->requested = true;
	return 0;
}

static void *direct_async_metadata_slot(struct iiod_direct_async *direct,
		size_t slot)
{
	return direct->metadata + slot * direct->metadata_capacity;
}

/* Corresponds to an opened device */
struct DevEntry {
	unsigned int ref_count;

	struct iio_device *dev;
	struct iio_buffer *buf;
	unsigned int sample_size, nb_clients;
	unsigned int samples_count;
	size_t metadata_extra_samples;
	void *metadata_provider_context;
	struct iiod_buffer_burst_plan burst_plan;
	struct iiod_burst_cache burst;
	struct iiod_ddr_ring ring;
	struct iiod_direct_async direct;
	bool update_mask;
	bool cyclic;
	bool closed;
	bool cancelled;
	bool metadata_enabled;
	bool metadata_timeout_floor_active;
	bool direct_used;

	/* Linked list of ThdEntry structures corresponding
	 * to all the threads who opened the device */
	SLIST_HEAD(ThdHead, ThdEntry) thdlist_head;
	pthread_mutex_t thdlist_lock;

	pthread_cond_t rw_ready_cond;
	pthread_mutex_t ring_lock;
	pthread_cond_t ring_ready_cond;
	struct iiod_stage_timing timing;

	uint32_t *mask;
	size_t nb_words;
};

static inline const char *dev_label_or_name_or_id(const struct iio_device *dev);

static const char *const iiod_timing_stage_names[IIOD_TIMING_STAGE_COUNT] = {
	[IIOD_TIMING_SAMPLER_ADMIT] = "sampler_admit",
	[IIOD_TIMING_DMA_REFILL] = "dma_refill",
	[IIOD_TIMING_SAMPLER_FINISH] = "sampler_finish",
	[IIOD_TIMING_METADATA_BUILD] = "metadata_build",
	[IIOD_TIMING_DDR_COPY] = "ddr_copy",
	[IIOD_TIMING_TRANSPORT_FRAME] = "transport_frame",
	[IIOD_TIMING_TRANSPORT_IQ] = "transport_iq",
	[IIOD_TIMING_RING_PRODUCER_WAIT] = "ring_producer_wait",
	[IIOD_TIMING_RING_CONSUMER_WAIT] = "ring_consumer_wait",
};

static uint64_t iiod_timing_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &now))
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
}

static void iiod_timing_record(struct DevEntry *entry,
	enum iiod_timing_stage stage, uint64_t started_ns)
{
	struct iiod_timing_accumulator *accumulator;
	uint64_t finished_ns;
	uint64_t elapsed_ns;

	if (!started_ns || stage >= IIOD_TIMING_STAGE_COUNT)
		return;
	finished_ns = iiod_timing_now();
	if (!finished_ns || finished_ns < started_ns)
		return;
	elapsed_ns = finished_ns - started_ns;

	pthread_mutex_lock(&entry->timing.lock);
	if (!entry->timing.first_ns || started_ns < entry->timing.first_ns)
		entry->timing.first_ns = started_ns;
	if (finished_ns > entry->timing.last_ns)
		entry->timing.last_ns = finished_ns;
	accumulator = &entry->timing.stages[stage];
	accumulator->count++;
	accumulator->total_ns += elapsed_ns;
	if (elapsed_ns > accumulator->max_ns)
		accumulator->max_ns = elapsed_ns;
	pthread_mutex_unlock(&entry->timing.lock);
}

static void iiod_timing_log(struct DevEntry *entry, bool snapshot)
{
	const char *device = dev_label_or_name_or_id(entry->dev);
	uint64_t transported_frames = 0;
	uint64_t frame_iq_bytes;
	uint64_t wall_ns = 0;
	unsigned int i;

	pthread_mutex_lock(&entry->timing.lock);
	frame_iq_bytes = (uint64_t)entry->sample_size * entry->samples_count;
	if (frame_iq_bytes)
		transported_frames = entry->timing.transported_iq_bytes /
			frame_iq_bytes;
	if (entry->timing.last_ns >= entry->timing.first_ns)
		wall_ns = entry->timing.last_ns - entry->timing.first_ns;
	for (i = 0; i < IIOD_TIMING_STAGE_COUNT; i++) {
		const struct iiod_timing_accumulator *stage =
			&entry->timing.stages[i];

		if (!stage->count)
			continue;
		fprintf(stderr,
			"SPF_IIOD_TIMING_V1 snapshot=%u dev=%s metadata=%u burst=%u ring=%u direct=%u "
			"frame_iq_bytes=%zu transported_iq_bytes=%" PRIu64
			" transported_frames=%" PRIu64 " wall_ns=%" PRIu64
			" stage=%s count=%" PRIu64
			" total_ns=%" PRIu64 " max_ns=%" PRIu64 "\n",
			snapshot, device ? device : "unknown", entry->metadata_enabled,
			entry->burst_plan.requested_iq_bytes != 0,
			entry->burst_plan.ring_capacity_iq_bytes != 0,
			entry->direct_used,
			(size_t)frame_iq_bytes, entry->timing.transported_iq_bytes,
			transported_frames, wall_ns,
			iiod_timing_stage_names[i], stage->count, stage->total_ns,
			stage->max_ns);
	}
	fflush(stderr);
	pthread_mutex_unlock(&entry->timing.lock);
}

static void iiod_timing_transport_complete(struct DevEntry *entry,
	ssize_t iq_bytes)
{
	uint64_t transported_frames = 0;
	uint64_t frame_iq_bytes;
	bool due = false;

	if (iq_bytes <= 0)
		return;
	pthread_mutex_lock(&entry->timing.lock);
	frame_iq_bytes = (uint64_t)entry->sample_size * entry->samples_count;
	if (UINT64_MAX - entry->timing.transported_iq_bytes <
			(uint64_t)iq_bytes)
		entry->timing.transported_iq_bytes = UINT64_MAX;
	else
		entry->timing.transported_iq_bytes += (uint64_t)iq_bytes;
	if (frame_iq_bytes)
		transported_frames = entry->timing.transported_iq_bytes /
			frame_iq_bytes;
	if (entry->timing.next_snapshot_frame &&
			transported_frames >= entry->timing.next_snapshot_frame) {
		do {
			if (UINT64_MAX - entry->timing.next_snapshot_frame <
					IIOD_TIMING_SNAPSHOT_FRAMES) {
				entry->timing.next_snapshot_frame = 0;
				break;
			}
			entry->timing.next_snapshot_frame +=
				IIOD_TIMING_SNAPSHOT_FRAMES;
		} while (entry->timing.next_snapshot_frame &&
			transported_frames >= entry->timing.next_snapshot_frame);
		due = true;
	}
	pthread_mutex_unlock(&entry->timing.lock);
	if (due)
		iiod_timing_log(entry, true);
}

static int iiod_timed_metadata_before_refill(struct DevEntry *entry)
{
	uint64_t started_ns = iiod_timing_now();
	int ret = iiod_buffer_metadata_before_refill(
		entry->metadata_provider_context);

	iiod_timing_record(entry, IIOD_TIMING_SAMPLER_ADMIT, started_ns);
	return ret;
}

static ssize_t iiod_timed_buffer_refill(struct DevEntry *entry)
{
	uint64_t started_ns = iiod_timing_now();
	ssize_t ret = iio_buffer_refill(entry->buf);

	iiod_timing_record(entry, IIOD_TIMING_DMA_REFILL, started_ns);
	return ret;
}

static struct iio_buffer_block *iiod_timed_buffer_block_acquire(
		struct DevEntry *entry)
{
	uint64_t started_ns = iiod_timing_now();
	struct iio_buffer_block *block = iio_buffer_block_acquire(entry->buf);

	iiod_timing_record(entry, IIOD_TIMING_DMA_REFILL, started_ns);
	return block;
}

static int iiod_timed_metadata_after_refill(struct DevEntry *entry)
{
	uint64_t started_ns = iiod_timing_now();
	int ret = iiod_buffer_metadata_after_refill(
		entry->metadata_provider_context);

	iiod_timing_record(entry, IIOD_TIMING_SAMPLER_FINISH, started_ns);
	return ret;
}

static ssize_t iiod_timed_metadata_get(struct DevEntry *entry, size_t raw_bytes,
	void *metadata, size_t metadata_capacity, size_t *iq_offset,
	size_t *iq_bytes)
{
	uint64_t started_ns = iiod_timing_now();
	ssize_t ret = iiod_buffer_metadata_get(entry->metadata_provider_context,
		entry->dev, entry->buf, raw_bytes, metadata, metadata_capacity,
		iq_offset, iq_bytes);

	iiod_timing_record(entry, IIOD_TIMING_METADATA_BUILD, started_ns);
	return ret;
}

static void iiod_timed_ddr_copy(struct DevEntry *entry, void *destination,
	const void *source, size_t bytes)
{
	uint64_t started_ns = iiod_timing_now();

	memcpy(destination, source, bytes);
	iiod_timing_record(entry, IIOD_TIMING_DDR_COPY, started_ns);
}

/* entry->ring_lock must be held. */
static int direct_async_spill_newest_dma(struct DevEntry *entry)
{
	struct iiod_direct_async *direct = &entry->direct;
	struct iiod_ddr_ring *ring = &entry->ring;
	struct iiod_direct_async_frame *frame = NULL;
	struct iio_buffer_block *block;
	size_t queue_slot, ring_slot, offset;
	int ret;

	if (!ring->direct_extension || direct->dma_count < direct->dma_capacity)
		return 0;
	/* Never spill the head: the network worker may already be using it. */
	for (offset = 1; offset <= direct->count; offset++) {
		queue_slot = (direct->tail + direct->capacity - offset) %
			direct->capacity;
		if (queue_slot == direct->head)
			continue;
		if (direct->frames[queue_slot].block &&
				!direct->frames[queue_slot].ring_backed) {
			frame = &direct->frames[queue_slot];
			break;
		}
	}
	if (!frame)
		return -EIO;
	ret = iiod_ddr_ring_core_producer_reserve(&ring->core, &ring_slot);
	if (ret)
		return ret;
	block = frame->block;
	if (!block || !frame->iq_bytes || frame->iq_bytes != ring->frame_iq_bytes) {
		(void)iiod_ddr_ring_core_producer_abort(&ring->core);
		return -EIO;
	}
	iiod_timed_ddr_copy(entry, ring_iq_slot(ring, ring_slot),
		(uint8_t *)iio_buffer_block_start(block) + frame->iq_offset,
		frame->iq_bytes);
	ret = iio_buffer_block_release(block);
	if (ret) {
		(void)iiod_ddr_ring_core_producer_abort(&ring->core);
		return ret;
	}
	ret = iiod_ddr_ring_core_producer_commit(&ring->core);
	if (ret)
		return ret;
	frame->block = NULL;
	frame->iq_offset = 0;
	frame->ring_slot = ring_slot;
	frame->ring_backed = true;
	direct->dma_count--;
	return 0;
}

struct sample_cb_info {
	struct parser_pdata *pdata;
	unsigned int nb_bytes, cpt;
	uint32_t *mask;
};

/* Protects iio_device_{set,get}_data() from concurrent access from multiple
 * clients */
static pthread_mutex_t devlist_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned int get_channel_number(const struct iio_channel *chn)
{
	const struct iio_device *dev = iio_channel_get_device(chn);
	const struct iio_channel *other;
	unsigned int i = 0;

	for (i = 0; i < iio_device_get_channels_count(dev); i++) {
		other = iio_device_get_channel(dev, i);
		if (other == chn)
			break;
	}

	return i;
}

static inline const char *dev_label_or_name_or_id(const struct iio_device *dev)
{
	const char *name;

	name = iio_device_get_label(dev);
	if (name)
		return name;

	name = iio_device_get_name(dev);
	if (name)
		return name;

	return iio_device_get_id(dev);
}

#if WITH_AIO
static ssize_t async_io(struct parser_pdata *pdata, void *buf, size_t len,
	bool do_read)
{
	ssize_t ret;
	struct pollfd pfd[2];
	unsigned int num_pfds;
	struct iocb iocb;
	struct iocb *ios[1];
	struct io_event e[1];

	ios[0] = &iocb;

	if (do_read)
		io_prep_pread(&iocb, pdata->fd_in, buf, len, 0);
	else
		io_prep_pwrite(&iocb, pdata->fd_out, buf, len, 0);

	io_set_eventfd(&iocb, pdata->aio_eventfd);

	pthread_mutex_lock(&pdata->aio_mutex);

	ret = io_submit(pdata->aio_ctx, 1, ios);
	if (ret != 1) {
		pthread_mutex_unlock(&pdata->aio_mutex);
		IIO_ERROR("Failed to submit IO operation: %zd\n", ret);
		return -EIO;
	}

	pfd[0].fd = pdata->aio_eventfd;
	pfd[0].events = POLLIN;
	pfd[0].revents = 0;
	pfd[1].fd = thread_pool_get_poll_fd(pdata->pool);
	pfd[1].events = POLLIN;
	pfd[1].revents = 0;
	num_pfds = 2;

	do {
		poll_nointr(pfd, num_pfds);

		if (pfd[0].revents & POLLIN) {
			uint64_t event;
			ret = read(pdata->aio_eventfd, &event, sizeof(event));
			if (ret != sizeof(event)) {
				IIO_ERROR("Failed to read from eventfd: %d\n", -errno);
				ret = -EIO;
				break;
			}

			ret = io_getevents(pdata->aio_ctx, 0, 1, e, NULL);
			if (ret != 1) {
				IIO_ERROR("Failed to read IO events: %zd\n", ret);
				ret = -EIO;
				break;
			} else {
				ret = (long)e[0].res;
			}
		} else if ((num_pfds > 1 && pfd[1].revents & POLLIN)) {
			/* Got a STOP event to abort this whole session */
			ret = io_cancel(pdata->aio_ctx, &iocb, e);
			if (ret != -EINPROGRESS && ret != -EINVAL) {
				IIO_ERROR("Failed to cancel IO transfer: %zd\n", ret);
				ret = -EIO;
				break;
			}
			/* It should not be long now until we get the cancellation event */
			num_pfds = 1;
		}
	} while (!(pfd[0].revents & POLLIN));

	pthread_mutex_unlock(&pdata->aio_mutex);

	/* Got STOP event, treat it as EOF */
	if (num_pfds == 1)
		return 0;

	return ret;
}

#define MAX_AIO_REQ_SIZE (1024 * 1024)

static ssize_t readfd_aio(struct parser_pdata *pdata, void *dest, size_t len)
{
	if (len > MAX_AIO_REQ_SIZE)
		len = MAX_AIO_REQ_SIZE;
	return async_io(pdata, dest, len, true);
}

static ssize_t writefd_aio(struct parser_pdata *pdata, const void *dest,
		size_t len)
{
	if (len > MAX_AIO_REQ_SIZE)
		len = MAX_AIO_REQ_SIZE;
	return async_io(pdata, (void *)dest, len, false);
}
#endif /* WITH_AIO */

static ssize_t readfd_io(struct parser_pdata *pdata, void *dest, size_t len)
{
	ssize_t ret;
	struct pollfd pfd[2];

	pfd[0].fd = pdata->fd_in;
	pfd[0].events = POLLIN | POLLRDHUP;
	pfd[0].revents = 0;
	pfd[1].fd = thread_pool_get_poll_fd(pdata->pool);
	pfd[1].events = POLLIN;
	pfd[1].revents = 0;

	do {
		poll_nointr(pfd, 2);

		/* Got STOP event, or client closed the socket: treat it as EOF */
		if (pfd[1].revents & POLLIN || pfd[0].revents & POLLRDHUP)
			return 0;
		if (pfd[0].revents & POLLERR)
			return -EIO;
		if (!(pfd[0].revents & POLLIN))
			continue;

		do {
			if (pdata->fd_in_is_socket)
				ret = recv(pdata->fd_in, dest, len, MSG_NOSIGNAL);
			else
				ret = read(pdata->fd_in, dest, len);
		} while (ret == -1 && errno == EINTR);

		if (ret != -1 || errno != EAGAIN)
			break;
	} while (true);

	if (ret == -1)
		return -errno;

	return ret;
}

static ssize_t writefd_io(struct parser_pdata *pdata, const void *src, size_t len)
{
	ssize_t ret;
	struct pollfd pfd[2];

	pfd[0].fd = pdata->fd_out;
	pfd[0].events = POLLOUT;
	pfd[0].revents = 0;
	pfd[1].fd = thread_pool_get_poll_fd(pdata->pool);
	pfd[1].events = POLLIN;
	pfd[1].revents = 0;

	do {
		poll_nointr(pfd, 2);

		/* Got STOP event, or client closed the socket: treat it as EOF */
		if (pfd[1].revents & POLLIN || pfd[0].revents & POLLHUP)
			return 0;
		if (pfd[0].revents & POLLERR)
			return -EIO;
		if (!(pfd[0].revents & POLLOUT))
			continue;

		do {
			if (pdata->fd_out_is_socket)
				ret = send(pdata->fd_out, src, len, MSG_NOSIGNAL);
			else
				ret = write(pdata->fd_out, src, len);
		} while (ret == -1 && errno == EINTR);

		if (ret != -1 || errno != EAGAIN)
			break;
	} while (true);

	if (ret == -1)
		return -errno;

	return ret;
}

ssize_t write_all(struct parser_pdata *pdata, const void *src, size_t len)
{
	uintptr_t ptr = (uintptr_t) src;

	while (len) {
		ssize_t ret = pdata->writefd(pdata, (void *) ptr, len);
		if (ret < 0)
			return ret;
		if (!ret)
			return -EPIPE;
		ptr += ret;
		len -= ret;
	}

	return ptr - (uintptr_t) src;
}

static ssize_t read_all(struct parser_pdata *pdata,
		void *dst, size_t len)
{
	uintptr_t ptr = (uintptr_t) dst;

	while (len) {
		ssize_t ret = pdata->readfd(pdata, (void *) ptr, len);
		if (ret < 0)
			return ret;
		if (!ret)
			return -EPIPE;
		ptr += ret;
		len -= ret;
	}

	return ptr - (uintptr_t) dst;
}

static void print_value(struct parser_pdata *pdata, long value)
{
	if (pdata->verbose && value < 0) {
		char buf[1024];
		iio_strerror(-value, buf, sizeof(buf));
		output(pdata, "ERROR: ");
		output(pdata, buf);
		output(pdata, "\n");
	} else {
		char buf[128];
		snprintf(buf, sizeof(buf), "%li\n", value);
		output(pdata, buf);
	}
}

static ssize_t send_sample(const struct iio_channel *chn,
		void *src, size_t length, void *d)
{
	unsigned int number = get_channel_number(chn);
	struct sample_cb_info *info = d;

	if (iio_channel_get_index(chn) < 0 || !TEST_BIT(info->mask, number))
		return 0;
	if (info->nb_bytes < length)
		return 0;

	if (info->cpt % length) {
		unsigned int i, goal = length - info->cpt % length;
		char zero = 0;
		ssize_t ret;

		for (i = 0; i < goal; i++) {
			ret = info->pdata->writefd(info->pdata, &zero, 1);
			if (ret < 0)
				return ret;
		}
		info->cpt += goal;
	}

	info->cpt += length;
	info->nb_bytes -= length;
	return write_all(info->pdata, src, length);
}

static ssize_t receive_sample(const struct iio_channel *chn,
		void *dst, size_t length, void *d)
{
	unsigned int number = get_channel_number(chn);
	struct sample_cb_info *info = d;

	if (iio_channel_get_index(chn) < 0 || !TEST_BIT(info->mask, number))
		return 0;
	if (info->cpt == info->nb_bytes)
		return 0;

	/* Skip the padding if needed */
	if (info->cpt % length) {
		unsigned int i, goal = length - info->cpt % length;
		char foo;
		ssize_t ret;

		for (i = 0; i < goal; i++) {
			ret = info->pdata->readfd(info->pdata, &foo, 1);
			if (ret < 0)
				return ret;
		}
		info->cpt += goal;
	}

	info->cpt += length;
	return read_all(info->pdata, dst, length);
}

static ssize_t send_data(struct DevEntry *dev, struct ThdEntry *thd, size_t len)
{
	struct parser_pdata *pdata = thd->pdata;
	bool demux = server_demux && dev->sample_size != thd->sample_size;
	void *metadata = NULL;
	ssize_t metadata_len = 0;
	ssize_t ret;
	void *start;
	size_t iq_offset = 0;
	size_t iq_bytes = len;

	if (demux && !thd->metadata_enabled)
		len = (len / dev->sample_size) * thd->sample_size;
	if (!thd->metadata_enabled && len > thd->nb)
		len = thd->nb;

	if (thd->metadata_enabled) {
		metadata = malloc(thd->metadata_capacity);
		if (!metadata)
			return -ENOMEM;
		metadata_len = iiod_timed_metadata_get(dev, len, metadata,
			thd->metadata_capacity, &iq_offset, &iq_bytes);
		if (metadata_len <= 0 ||
				metadata_len > (ssize_t)thd->metadata_capacity ||
				iq_offset > len || iq_bytes > len - iq_offset ||
				iq_bytes != thd->nb) {
			ret = metadata_len < 0 ? metadata_len : -EIO;
			free(metadata);
			return ret;
		}
		len = iq_bytes;
	}

	print_value(pdata, len);

	if (thd->new_client) {
		unsigned int i;
		char buf[129], *ptr = buf;
		uint32_t *mask = demux ? thd->mask : dev->mask;
		ssize_t length;

		length = sizeof(buf);
		/* Send the current mask */
		for (i = dev->nb_words; i > 0 && ptr < buf + sizeof(buf);
				i--, ptr += 8) {
			snprintf(ptr, length, "%08x", mask[i - 1]);
			length -= 8;
		}

		*ptr = '\n';
		length--;

		if (length < 0) {
			IIO_ERROR("send_data: string length error\n");
			return -ENOSPC;
		}

		ret = write_all(pdata, buf, ptr + 1 - buf);
		if (ret < 0)
			goto out_free_metadata;

		thd->new_client = false;
	}

	if (thd->metadata_enabled) {
		print_value(pdata, metadata_len);
		ret = write_all(pdata, metadata, (size_t)metadata_len);
		if (ret < 0)
			goto out_free_metadata;
	}

	if (!demux) {
		/* Short path */
		uint64_t started_ns = iiod_timing_now();

		start = (void *)((uintptr_t)iio_buffer_start(dev->buf) + iq_offset);
		ret = write_all(pdata, start, len);
		iiod_timing_record(dev, IIOD_TIMING_TRANSPORT_IQ, started_ns);
	} else {
		uint64_t started_ns = iiod_timing_now();
		struct sample_cb_info info = {
			.pdata = pdata,
			.cpt = 0,
			.nb_bytes = len,
			.mask = thd->mask,
		};

		ret = iio_buffer_foreach_sample(dev->buf, send_sample, &info);
		iiod_timing_record(dev, IIOD_TIMING_TRANSPORT_IQ, started_ns);
	}

out_free_metadata:
	free(metadata);
	return ret;
}

static int capture_burst(struct DevEntry *entry)
{
	struct iiod_burst_cache *cache = &entry->burst;
	unsigned int startup_discards = 0;

	while (cache->captured_frames < cache->frame_count) {
		ssize_t metadata_len;
		size_t iq_offset = 0;
		size_t iq_bytes = 0;
		ssize_t raw_bytes;
		int ret;

		pthread_mutex_lock(&entry->thdlist_lock);
		if (entry->cancelled || SLIST_EMPTY(&entry->thdlist_head)) {
			pthread_mutex_unlock(&entry->thdlist_lock);
			return -ECANCELED;
		}
		pthread_mutex_unlock(&entry->thdlist_lock);

		ret = iiod_timed_metadata_before_refill(entry);
		if (ret)
			return ret;
		raw_bytes = iiod_timed_buffer_refill(entry);
		ret = iiod_timed_metadata_after_refill(entry);
		if (raw_bytes < 0)
			return (int)raw_bytes;
		if (ret)
			return ret;
		metadata_len = iiod_timed_metadata_get(entry, (size_t)raw_bytes,
			burst_metadata_slot(cache, cache->captured_frames),
			cache->metadata_capacity, &iq_offset, &iq_bytes);
		if (metadata_len == -EAGAIN) {
			if (++startup_discards > IIOD_BURST_MAX_STARTUP_DISCARDS)
				return -ETIMEDOUT;
			continue;
		}
		if (metadata_len < 0)
			return (int)metadata_len;
		if (!metadata_len || metadata_len > (ssize_t)cache->metadata_capacity ||
			iq_bytes != cache->frame_iq_bytes ||
			iq_offset > (size_t)raw_bytes ||
			iq_bytes > (size_t)raw_bytes - iq_offset)
			return -EIO;
		iiod_timed_ddr_copy(entry,
			burst_iq_slot(cache, cache->captured_frames),
			(uint8_t *)iio_buffer_start(entry->buf) + iq_offset, iq_bytes);
		cache->frames[cache->captured_frames].metadata_bytes =
			(size_t)metadata_len;
		cache->captured_frames++;
	}
	return 0;
}

static void finish_burst_capture(struct DevEntry *entry, int capture_error)
{
	struct iio_buffer *buffer;
	void *provider_context;

	pthread_mutex_lock(&entry->thdlist_lock);
	buffer = entry->buf;
	entry->buf = NULL;
	provider_context = entry->metadata_provider_context;
	entry->metadata_provider_context = NULL;
	pthread_mutex_unlock(&entry->thdlist_lock);

	if (buffer)
		iio_buffer_destroy(buffer);
	if (provider_context)
		iiod_buffer_metadata_close(provider_context);

	pthread_mutex_lock(&entry->thdlist_lock);
	if (capture_error ||
		entry->burst.captured_frames != entry->burst.frame_count)
		burst_fail(&entry->burst, capture_error ? capture_error : -EIO);
	else
		entry->burst.state = IIOD_BURST_SEALED;
	pthread_cond_broadcast(&entry->rw_ready_cond);
	pthread_mutex_unlock(&entry->thdlist_lock);
}

static ssize_t send_burst_data(struct DevEntry *entry, struct ThdEntry *thd)
{
	struct iiod_burst_cache *cache = &entry->burst;
	struct parser_pdata *pdata = thd->pdata;
	struct iiod_burst_frame *frame;
	void *metadata;
	void *iq;
	ssize_t ret;

	if (cache->state == IIOD_BURST_FAILED)
		return cache->error ? cache->error : -EIO;
	if (cache->state == IIOD_BURST_DRAINED)
		return -ENODATA;
	if (cache->state != IIOD_BURST_SEALED ||
		cache->next_frame >= cache->frame_count)
		return -EIO;
	frame = &cache->frames[cache->next_frame];
	if (thd->nb != cache->frame_iq_bytes ||
		thd->metadata_capacity < frame->metadata_bytes)
		return -ENOSPC;
	metadata = burst_metadata_slot(cache, cache->next_frame);
	iq = burst_iq_slot(cache, cache->next_frame);

	print_value(pdata, cache->frame_iq_bytes);
	if (thd->new_client) {
		unsigned int i;
		char mask[129], *ptr = mask;
		ssize_t remaining = sizeof(mask);

		for (i = entry->nb_words; i > 0 && ptr < mask + sizeof(mask);
				i--, ptr += 8) {
			snprintf(ptr, (size_t)remaining, "%08x", entry->mask[i - 1]);
			remaining -= 8;
		}
		if (remaining <= 0)
			return -ENOSPC;
		*ptr = '\n';
		ret = write_all(pdata, mask, (size_t)(ptr + 1 - mask));
		if (ret < 0)
			return ret;
		thd->new_client = false;
	}
	print_value(pdata, frame->metadata_bytes);
	ret = write_all(pdata, metadata, frame->metadata_bytes);
	if (ret < 0)
		return ret;
	{
		uint64_t started_ns = iiod_timing_now();

		ret = write_all(pdata, iq, cache->frame_iq_bytes);
		iiod_timing_record(entry, IIOD_TIMING_TRANSPORT_IQ, started_ns);
	}
	if (ret < 0)
		return ret;

	cache->next_frame++;
	if (cache->next_frame == cache->frame_count) {
		burst_release_storage(cache);
		cache->state = IIOD_BURST_DRAINED;
	}
	return (ssize_t)cache->frame_iq_bytes;
}

static ssize_t send_ring_data(struct DevEntry *entry, struct ThdEntry *thd)
{
	struct iiod_ddr_ring *ring = &entry->ring;
	struct parser_pdata *pdata = thd->pdata;
	struct iiod_ddr_ring_frame *frame;
	void *metadata;
	void *iq;
	size_t slot;
	ssize_t ret;

	pthread_mutex_lock(&entry->ring_lock);
	ret = iiod_ddr_ring_core_consumer_reserve(&ring->core, &slot);
	if (ret) {
		pthread_mutex_unlock(&entry->ring_lock);
		return ret;
	}
	frame = &ring->frames[slot];
	if (thd->nb != ring->frame_iq_bytes ||
		thd->metadata_capacity < frame->metadata_bytes) {
		(void)iiod_ddr_ring_core_cancel(&ring->core,
			SPF_DDR_RING_REASON_CLIENT_CANCELLED);
		pthread_cond_broadcast(&entry->ring_ready_cond);
		pthread_mutex_unlock(&entry->ring_lock);
		return -ENOSPC;
	}
	metadata = ring_metadata_slot(ring, slot);
	iq = ring_iq_slot(ring, slot);
	pthread_mutex_unlock(&entry->ring_lock);

	print_value(pdata, ring->frame_iq_bytes);
	if (thd->new_client) {
		unsigned int i;
		char mask[129], *ptr = mask;
		ssize_t remaining = sizeof(mask);

		for (i = entry->nb_words; i > 0 && ptr < mask + sizeof(mask);
				i--, ptr += 8) {
			snprintf(ptr, (size_t)remaining, "%08x", entry->mask[i - 1]);
			remaining -= 8;
		}
		if (remaining <= 0) {
			ret = -ENOSPC;
			goto transport_failure;
		}
		*ptr = '\n';
		ret = write_all(pdata, mask, (size_t)(ptr + 1 - mask));
		if (ret < 0)
			goto transport_failure;
		thd->new_client = false;
	}
	print_value(pdata, frame->metadata_bytes);
	ret = write_all(pdata, metadata, frame->metadata_bytes);
	if (ret < 0)
		goto transport_failure;
	{
		uint64_t started_ns = iiod_timing_now();

		ret = write_all(pdata, iq, ring->frame_iq_bytes);
		iiod_timing_record(entry, IIOD_TIMING_TRANSPORT_IQ, started_ns);
	}
	if (ret < 0)
		goto transport_failure;

	pthread_mutex_lock(&entry->ring_lock);
	ret = iiod_ddr_ring_core_consumer_release(&ring->core);
	pthread_cond_broadcast(&entry->ring_ready_cond);
	pthread_mutex_unlock(&entry->ring_lock);
	return ret ? ret : (ssize_t)ring->frame_iq_bytes;

transport_failure:
	pthread_mutex_lock(&entry->ring_lock);
	(void)iiod_ddr_ring_core_cancel(&ring->core,
		SPF_DDR_RING_REASON_CLIENT_DISCONNECTED);
	pthread_cond_broadcast(&entry->ring_ready_cond);
	pthread_mutex_unlock(&entry->ring_lock);
	return ret;
}

static ssize_t send_direct_async_data(struct DevEntry *entry,
		struct ThdEntry *thd)
{
	struct iiod_direct_async *direct = &entry->direct;
	struct iiod_ddr_ring *ring = &entry->ring;
	struct parser_pdata *pdata = thd->pdata;
	struct iiod_direct_async_frame *frame;
	struct iio_buffer_block *block = NULL;
	void *metadata;
	void *iq;
	size_t iq_bytes, ring_slot = 0, slot;
	bool ring_backed;
	ssize_t ret;
	int complete_ret = 0, release_ret = 0;

	pthread_mutex_lock(&entry->ring_lock);
	if (!direct->count) {
		ret = direct->error ? direct->error : -ENODATA;
		pthread_mutex_unlock(&entry->ring_lock);
		return ret;
	}
	slot = direct->head;
	frame = &direct->frames[slot];
	ring_backed = frame->ring_backed;
	if ((ring_backed == (frame->block != NULL)) ||
		thd->nb != frame->iq_bytes ||
		thd->metadata_capacity < frame->metadata_bytes) {
		direct->error = -ENOSPC;
		direct->cancelled = true;
		if (ring->direct_extension &&
				!iiod_ddr_ring_core_is_terminal(&ring->core))
			(void)iiod_ddr_ring_core_fail(&ring->core,
				SPF_DDR_RING_REASON_INTERNAL_ERROR, -ENOSPC);
		pthread_cond_broadcast(&entry->ring_ready_cond);
		pthread_mutex_unlock(&entry->ring_lock);
		return -ENOSPC;
	}
	iq_bytes = frame->iq_bytes;
	metadata = direct_async_metadata_slot(direct, slot);
	if (ring_backed) {
		ret = iiod_ddr_ring_core_consumer_reserve(&ring->core, &ring_slot);
		if (ret || ring_slot != frame->ring_slot) {
			ret = ret ? ret : -EIO;
			direct->error = (int)ret;
			direct->cancelled = true;
			if (!iiod_ddr_ring_core_is_terminal(&ring->core))
				(void)iiod_ddr_ring_core_fail(&ring->core,
					SPF_DDR_RING_REASON_INTERNAL_ERROR, (int)ret);
			pthread_cond_broadcast(&entry->ring_ready_cond);
			pthread_mutex_unlock(&entry->ring_lock);
			return ret;
		}
		iq = ring_iq_slot(ring, ring_slot);
	} else {
		block = frame->block;
		iq = (uint8_t *)iio_buffer_block_start(block) + frame->iq_offset;
	}
	pthread_mutex_unlock(&entry->ring_lock);

	print_value(pdata, frame->iq_bytes);
	if (thd->new_client) {
		unsigned int i;
		char mask[129], *ptr = mask;
		ssize_t remaining = sizeof(mask);

		for (i = entry->nb_words; i > 0 && ptr < mask + sizeof(mask);
				i--, ptr += 8) {
			snprintf(ptr, (size_t)remaining, "%08x", entry->mask[i - 1]);
			remaining -= 8;
		}
		if (remaining <= 0) {
			ret = -ENOSPC;
			goto complete;
		}
		*ptr = '\n';
		ret = write_all(pdata, mask, (size_t)(ptr + 1 - mask));
		if (ret < 0)
			goto complete;
		thd->new_client = false;
	}
	print_value(pdata, frame->metadata_bytes);
	ret = write_all(pdata, metadata, frame->metadata_bytes);
	if (ret < 0)
		goto complete;
	{
		uint64_t started_ns = iiod_timing_now();

		ret = write_all(pdata, iq, frame->iq_bytes);
		iiod_timing_record(entry, IIOD_TIMING_TRANSPORT_IQ, started_ns);
	}

complete:
	if (!ring_backed)
		release_ret = iio_buffer_block_release(block);
	pthread_mutex_lock(&entry->ring_lock);
	if (ring_backed)
		release_ret = iiod_ddr_ring_core_consumer_release(&ring->core);
	if (ret >= 0 && release_ret < 0)
		ret = release_ret;
	/* A failed DMA release keeps the lease reachable for teardown retry. */
	if (!release_ret) {
		frame->block = NULL;
		frame->ring_backed = false;
		frame->ring_slot = 0;
		if (!ring_backed)
			direct->dma_count--;
	}
	direct->head = (direct->head + 1U) % direct->capacity;
	direct->count--;
	direct->consumed_frames++;
	if (ret < 0) {
		direct->error = (int)ret;
		direct->cancelled = true;
		if (ring->direct_extension &&
				!iiod_ddr_ring_core_is_terminal(&ring->core))
			(void)iiod_ddr_ring_core_fail(&ring->core,
				SPF_DDR_RING_REASON_TRANSPORT_ERROR, (int)ret);
	} else if (ring->direct_extension &&
			direct->consumed_frames == direct->target_frames) {
		complete_ret = iiod_ddr_ring_core_complete_extension(&ring->core);
		if (complete_ret) {
			direct->error = complete_ret;
			direct->cancelled = true;
			ret = complete_ret;
		}
	}
	pthread_cond_broadcast(&entry->ring_ready_cond);
	pthread_mutex_unlock(&entry->ring_lock);
	return ret < 0 ? ret : (ssize_t)iq_bytes;
}

static ssize_t receive_data(struct DevEntry *dev, struct ThdEntry *thd)
{
	struct parser_pdata *pdata = thd->pdata;

	/* Inform that no error occurred, and that we'll start reading data */
	if (thd->new_client) {
		print_value(thd->pdata, 0);
		thd->new_client = false;
	}

	if (dev->sample_size == thd->sample_size) {
		/* Short path: Receive directly in the buffer */

		size_t len = dev->sample_size * dev->samples_count;
		if (thd->nb < len)
			len = thd->nb;

		return read_all(pdata, iio_buffer_start(dev->buf), len);
	} else {
		/* Long path: Mux the samples to the buffer */

		struct sample_cb_info info = {
			.pdata = pdata,
			.cpt = 0,
			.nb_bytes = thd->nb,
			.mask = thd->mask,
		};

		return iio_buffer_foreach_sample(dev->buf,
				receive_sample, &info);
	}
}

static void dev_entry_put(struct DevEntry *entry)
{
	bool free_entry = false;

	pthread_mutex_lock(&entry->thdlist_lock);
	entry->ref_count--;
	if (entry->ref_count == 0)
		free_entry = true;
	pthread_mutex_unlock(&entry->thdlist_lock);

	if (free_entry) {
		iiod_timing_log(entry, false);
		pthread_mutex_destroy(&entry->thdlist_lock);
		pthread_cond_destroy(&entry->rw_ready_cond);
		pthread_mutex_destroy(&entry->ring_lock);
		pthread_cond_destroy(&entry->ring_ready_cond);
		pthread_mutex_destroy(&entry->timing.lock);

		free(entry->mask);
		free(entry);
	}
}

static uint32_t ring_failure_reason(int error)
{
	if (error == -EOVERFLOW)
		return SPF_DDR_RING_REASON_COUNTER_GAP;
	if (error == -ETIMEDOUT)
		return SPF_DDR_RING_REASON_CONSUMER_STALL;
	if (error == -ECANCELED || error == -EPIPE)
		return SPF_DDR_RING_REASON_CLIENT_DISCONNECTED;
	return SPF_DDR_RING_REASON_DMA_ERROR;
}

static void direct_async_producer_thd(struct thread_pool *pool, void *data)
{
	struct DevEntry *entry = data;
	struct iiod_direct_async *direct = &entry->direct;
	unsigned int startup_discards = 0;
	int error = 0;

	while (!thread_pool_is_stopped(pool)) {
		struct iiod_direct_async_frame *frame;
		struct iio_buffer_block *block = NULL;
		ssize_t metadata_len = 0;
		size_t iq_offset = 0;
		size_t iq_bytes = 0;
		size_t raw_bytes;
		size_t slot;
		int ret;

		pthread_mutex_lock(&entry->ring_lock);
		while (!direct->cancelled && direct->count == direct->capacity)
			pthread_cond_wait(&entry->ring_ready_cond, &entry->ring_lock);
		if (direct->cancelled ||
			direct->produced_frames >= direct->target_frames) {
			pthread_mutex_unlock(&entry->ring_lock);
			break;
		}
		ret = direct_async_spill_newest_dma(entry);
		if (ret) {
			error = ret;
			pthread_mutex_unlock(&entry->ring_lock);
			break;
		}
		slot = direct->tail;
		pthread_mutex_unlock(&entry->ring_lock);

		ret = iiod_timed_metadata_before_refill(entry);
		if (ret == 0) {
			block = iiod_timed_buffer_block_acquire(entry);
			ret = block ? iiod_timed_metadata_after_refill(entry) : -errno;
		}
		if (ret) {
			error = ret;
			goto release_block;
		}
		raw_bytes = iio_buffer_block_bytes_used(block);
		metadata_len = iiod_timed_metadata_get(entry, raw_bytes,
			direct_async_metadata_slot(direct, slot),
			direct->metadata_capacity, &iq_offset, &iq_bytes);
		if (metadata_len == -EAGAIN) {
			if (++startup_discards > IIOD_BURST_MAX_STARTUP_DISCARDS)
				error = -ETIMEDOUT;
			goto release_block;
		}
		if (metadata_len < 0) {
			error = (int)metadata_len;
			goto release_block;
		}
		if (!metadata_len || metadata_len >
				(ssize_t)direct->metadata_capacity ||
			!iq_bytes || iq_offset > raw_bytes ||
			iq_bytes > raw_bytes - iq_offset) {
			error = -EIO;
			goto release_block;
		}

		pthread_mutex_lock(&entry->ring_lock);
		if (direct->cancelled) {
			pthread_mutex_unlock(&entry->ring_lock);
			goto release_block;
		}
		frame = &direct->frames[slot];
		frame->block = block;
		frame->metadata_bytes = (size_t)metadata_len;
		frame->iq_offset = iq_offset;
		frame->iq_bytes = iq_bytes;
		frame->ring_slot = 0;
		frame->ring_backed = false;
		block = NULL;
		direct->dma_count++;
		direct->tail = (direct->tail + 1U) % direct->capacity;
		direct->count++;
		direct->produced_frames++;
		pthread_cond_broadcast(&entry->ring_ready_cond);
		pthread_mutex_unlock(&entry->ring_lock);
		continue;

release_block:
		if (block) {
			int release_ret = iio_buffer_block_release(block);

			if (release_ret) {
				/* Keep a failed lease reachable for the teardown retry. */
				direct->frames[slot].block = block;
				block = NULL;
				error = release_ret;
			}
		}
		if (metadata_len == -EAGAIN && !error)
			continue;
		break;
	}

	pthread_mutex_lock(&entry->ring_lock);
	if (error && !direct->cancelled) {
		direct->error = error;
		if (entry->ring.direct_extension &&
				!iiod_ddr_ring_core_is_terminal(&entry->ring.core))
			(void)iiod_ddr_ring_core_fail(&entry->ring.core,
				ring_failure_reason(error), error < 0 ? error : -EIO);
	}
	direct->producer_exited = true;
	pthread_cond_broadcast(&entry->ring_ready_cond);
	pthread_mutex_unlock(&entry->ring_lock);
	pthread_mutex_lock(&entry->thdlist_lock);
	pthread_cond_broadcast(&entry->rw_ready_cond);
	pthread_mutex_unlock(&entry->thdlist_lock);
	dev_entry_put(entry);
}

static int direct_async_start_producer(struct DevEntry *entry)
{
	int ret;

	pthread_mutex_lock(&entry->ring_lock);
	if (!entry->direct.requested || entry->direct.producer_started) {
		pthread_mutex_unlock(&entry->ring_lock);
		return -EINVAL;
	}
	if (entry->ring.direct_extension) {
		ret = iiod_ddr_ring_core_start_extension(&entry->ring.core);
		if (ret) {
			pthread_mutex_unlock(&entry->ring_lock);
			return ret;
		}
	}
	entry->direct.producer_started = true;
	entry->direct.producer_exited = false;
	entry->direct_used = true;
	pthread_mutex_unlock(&entry->ring_lock);
	if (entry->ring.direct_extension)
		iiod_buffer_metadata_ring_prefix_complete(
			entry->metadata_provider_context, true);

	entry->ref_count++;
	ret = thread_pool_add_thread(main_thread_pool,
		direct_async_producer_thd, entry, "direct_dma");
	if (ret) {
		entry->ref_count--;
		pthread_mutex_lock(&entry->ring_lock);
		entry->direct.error = ret < 0 ? ret : -EIO;
		entry->direct.producer_started = false;
		entry->direct.producer_exited = true;
		if (entry->ring.direct_extension &&
				!iiod_ddr_ring_core_is_terminal(&entry->ring.core))
			(void)iiod_ddr_ring_core_fail(&entry->ring.core,
				SPF_DDR_RING_REASON_INTERNAL_ERROR,
				ret < 0 ? ret : -EIO);
		pthread_mutex_unlock(&entry->ring_lock);
	}
	return ret;
}

static void ring_producer_thd(struct thread_pool *pool, void *data)
{
	struct DevEntry *entry = data;
	struct iiod_ddr_ring *ring = &entry->ring;
	unsigned int startup_discards = 0;
	int error = 0;

	while (!thread_pool_is_stopped(pool)) {
		ssize_t metadata_len;
		uint64_t first_sample_sequence = 0;
		void *raw_start;
		size_t iq_offset = 0;
		size_t iq_bytes = 0;
		ssize_t raw_bytes;
		size_t slot;
		int ret;
		bool prefix_complete;
		uint64_t wait_started_ns = iiod_timing_now();

		pthread_mutex_lock(&entry->ring_lock);
		while ((ret = iiod_ddr_ring_core_producer_reserve(
				&ring->core, &slot)) == -EAGAIN)
			pthread_cond_wait(&entry->ring_ready_cond, &entry->ring_lock);
		prefix_complete = iiod_ddr_ring_core_prefix_complete(&ring->core);
		pthread_mutex_unlock(&entry->ring_lock);
		iiod_timing_record(entry, IIOD_TIMING_RING_PRODUCER_WAIT,
			wait_started_ns);
		if (ret == -ESHUTDOWN || ret == -ENODATA)
			break;
		if (ret) {
			error = ret;
			break;
		}
		iiod_buffer_metadata_ring_prefix_complete(
			entry->metadata_provider_context, prefix_complete);

		ret = iiod_timed_metadata_before_refill(entry);
		if (ret == 0) {
			raw_bytes = iiod_timed_buffer_refill(entry);
			ret = iiod_timed_metadata_after_refill(entry);
			if (raw_bytes < 0)
				ret = (int)raw_bytes;
		} else {
			raw_bytes = ret;
		}
		if (ret) {
			error = ret;
			goto abort_slot;
		}
		raw_start = iio_buffer_start(entry->buf);
		if (!raw_start) {
			error = -EIO;
			goto abort_slot;
		}
		if (entry->metadata_extra_samples &&
				(size_t)raw_bytes >= sizeof(first_sample_sequence))
			memcpy(&first_sample_sequence, raw_start,
				sizeof(first_sample_sequence));
		metadata_len = iiod_timed_metadata_get(entry, (size_t)raw_bytes,
			ring_metadata_slot(ring, slot),
			ring->metadata_capacity, &iq_offset, &iq_bytes);
		if (metadata_len == -EAGAIN) {
			pthread_mutex_lock(&entry->ring_lock);
			(void)iiod_ddr_ring_core_producer_abort(&ring->core);
			pthread_mutex_unlock(&entry->ring_lock);
			if (++startup_discards > IIOD_BURST_MAX_STARTUP_DISCARDS) {
				error = -ETIMEDOUT;
				break;
			}
			continue;
		}
		if (metadata_len < 0) {
			error = (int)metadata_len;
			if (entry->metadata_extra_samples) {
				pthread_mutex_lock(&entry->ring_lock);
				iiod_ddr_ring_core_mark_unavailable(&ring->core,
					first_sample_sequence);
				pthread_mutex_unlock(&entry->ring_lock);
			}
			goto abort_slot;
		}
		if (!metadata_len || metadata_len > (ssize_t)ring->metadata_capacity ||
			iq_bytes != ring->frame_iq_bytes ||
			iq_offset > (size_t)raw_bytes ||
			iq_bytes > (size_t)raw_bytes - iq_offset) {
			error = -EIO;
			goto abort_slot;
		}
		iiod_timed_ddr_copy(entry, ring_iq_slot(ring, slot),
			(uint8_t *)raw_start + iq_offset, iq_bytes);
		ring->frames[slot].metadata_bytes = (size_t)metadata_len;

		pthread_mutex_lock(&entry->ring_lock);
		ret = iiod_ddr_ring_core_producer_commit(&ring->core);
		if (!ret && entry->metadata_extra_samples)
			ret = iiod_ddr_ring_core_observe_samples(&ring->core,
				first_sample_sequence, entry->samples_count);
		pthread_cond_broadcast(&entry->ring_ready_cond);
		pthread_mutex_unlock(&entry->ring_lock);
		if (ret) {
			error = ret;
			break;
		}
		continue;

abort_slot:
		pthread_mutex_lock(&entry->ring_lock);
		(void)iiod_ddr_ring_core_producer_abort(&ring->core);
		pthread_mutex_unlock(&entry->ring_lock);
		break;
	}

	pthread_mutex_lock(&entry->ring_lock);
	if (error && ring->core.state != SPF_DDR_RING_STATE_CANCELLED)
		(void)iiod_ddr_ring_core_fail(&ring->core,
			ring_failure_reason(error), error);
	else if (!error && ring->core.state == SPF_DDR_RING_STATE_RUNNING)
		(void)iiod_ddr_ring_core_cancel(&ring->core,
			SPF_DDR_RING_REASON_CLIENT_CANCELLED);
	pthread_cond_broadcast(&entry->ring_ready_cond);
	pthread_mutex_unlock(&entry->ring_lock);

	pthread_mutex_lock(&entry->thdlist_lock);
	struct iio_buffer *buffer = entry->buf;
	void *provider_context = entry->metadata_provider_context;
	entry->buf = NULL;
	entry->metadata_provider_context = NULL;
	pthread_mutex_unlock(&entry->thdlist_lock);
	if (buffer)
		iio_buffer_destroy(buffer);
	if (provider_context)
		iiod_buffer_metadata_close(provider_context);

	pthread_mutex_lock(&entry->ring_lock);
	ring->producer_exited = true;
	pthread_cond_broadcast(&entry->ring_ready_cond);
	pthread_mutex_unlock(&entry->ring_lock);
	pthread_mutex_lock(&entry->thdlist_lock);
	pthread_cond_broadcast(&entry->rw_ready_cond);
	pthread_mutex_unlock(&entry->thdlist_lock);
	dev_entry_put(entry);
}

static int ring_start_producer(struct DevEntry *entry)
{
	int ret;

	pthread_mutex_lock(&entry->ring_lock);
	ret = iiod_ddr_ring_core_start(&entry->ring.core);
	if (!ret) {
		entry->ring.producer_started = true;
		entry->ring.producer_exited = false;
	}
	pthread_mutex_unlock(&entry->ring_lock);
	if (ret)
		return ret;

	entry->ref_count++;
	ret = thread_pool_add_thread(main_thread_pool, ring_producer_thd, entry,
		"ddr_ring");
	if (ret) {
		entry->ref_count--;
		pthread_mutex_lock(&entry->ring_lock);
		entry->ring.producer_started = false;
		entry->ring.producer_exited = true;
		(void)iiod_ddr_ring_core_fail(&entry->ring.core,
			SPF_DDR_RING_REASON_INTERNAL_ERROR, ret < 0 ? ret : -EIO);
		pthread_mutex_unlock(&entry->ring_lock);
	}
	return ret;
}

static void signal_thread(struct ThdEntry *thd, ssize_t ret)
{
	thd->err = ret;
	thd->nb = 0;
	thd->active = false;
	thd_entry_event_signal(thd);
}

static void rw_thd(struct thread_pool *pool, void *d)
{
	struct DevEntry *entry = d;
	struct ThdEntry *thd, *next_thd;
	struct iio_device *dev = entry->dev;
	unsigned int nb_words = entry->nb_words;
	ssize_t ret = 0;

	IIO_DEBUG("R/W thread started for device %s\n",
		  dev_label_or_name_or_id(dev));

	while (true) {
		bool has_readers = false, has_writers = false,
		     mask_updated = false, capture_burst_now = false;
		unsigned int sample_size;

		/* NOTE: this while loop must exit with thdlist_lock locked. */
		pthread_mutex_lock(&entry->thdlist_lock);

		if (SLIST_EMPTY(&entry->thdlist_head) &&
				(entry->ring.producer_started ||
				 entry->direct.producer_started)) {
			pthread_mutex_unlock(&entry->thdlist_lock);
			pthread_mutex_lock(&entry->ring_lock);
			while ((entry->ring.producer_started &&
					!entry->ring.producer_exited) ||
				(entry->direct.producer_started &&
					!entry->direct.producer_exited))
				pthread_cond_wait(&entry->ring_ready_cond,
					&entry->ring_lock);
			pthread_mutex_unlock(&entry->ring_lock);
			pthread_mutex_lock(&entry->thdlist_lock);
		}
		if (SLIST_EMPTY(&entry->thdlist_head))
			break;

		if (entry->update_mask) {
			unsigned int i;
			unsigned int samples_count = 0;

			memset(entry->mask, 0, nb_words * sizeof(*entry->mask));
			SLIST_FOREACH(thd, &entry->thdlist_head, dev_list_entry) {
				for (i = 0; i < nb_words; i++)
					entry->mask[i] |= thd->mask[i];

				if (thd->samples_count > samples_count)
					samples_count = thd->samples_count;
			}

			if (entry->buf)
				iio_buffer_destroy(entry->buf);

			for (i = 0; i < iio_device_get_channels_count(dev); i++) {
				struct iio_channel *chn = iio_device_get_channel(dev, i);
				unsigned int number = get_channel_number(chn);
				long index = iio_channel_get_index(chn);

				if (index < 0)
					continue;

				if (TEST_BIT(entry->mask, number))
					iio_channel_enable(chn);
				else
					iio_channel_disable(chn);
			}
			entry->sample_size = iio_device_get_sample_size(dev);
			if (entry->burst_plan.requested_iq_bytes ||
					entry->burst_plan.ring_capacity_iq_bytes) {
				size_t frame_iq_bytes, raw_samples, raw_frame_bytes;
				const char *context_name = iio_context_get_name(
					iio_device_get_context(dev));
				const bool check_cma = context_name &&
					!strcmp(context_name, "local");
				unsigned int kernel_buffers =
					iio_device_get_kernel_buffers_count(dev);

				if (!entry->sample_size ||
					!size_mul(samples_count, entry->sample_size,
						&frame_iq_bytes) ||
					!size_add(samples_count, entry->metadata_extra_samples,
						&raw_samples) ||
					!size_mul(raw_samples, entry->sample_size,
						&raw_frame_bytes)) {
					ret = -EOVERFLOW;
					break;
				}
				if (entry->burst_plan.requested_iq_bytes)
					ret = burst_prepare(&entry->burst, &entry->burst_plan,
						frame_iq_bytes, raw_frame_bytes, kernel_buffers,
						check_cma);
				else
					ret = ring_prepare(&entry->ring, &entry->burst_plan,
						frame_iq_bytes, raw_frame_bytes, kernel_buffers,
						check_cma);
				if (ret)
					break;
			}

			entry->buf = iio_device_create_buffer(dev,
					samples_count + entry->metadata_extra_samples,
					entry->cyclic);
			if (!entry->buf) {
				ret = -errno;
				IIO_ERROR("Unable to create buffer\n");
				break;
			}
			if (entry->metadata_enabled) {
				ret = iiod_buffer_metadata_buffer_opened(
					entry->metadata_provider_context,
					iio_device_get_kernel_buffers_count(dev));
				if (ret < 0) {
					iio_buffer_destroy(entry->buf);
					entry->buf = NULL;
					break;
				}
			}
			if (entry->burst.state == IIOD_BURST_RESERVED)
				entry->burst.state = IIOD_BURST_CAPTURING;
			entry->cancelled = false;
			entry->samples_count = samples_count;
			if (entry->ring.core.state == SPF_DDR_RING_STATE_RESERVED &&
					!entry->ring.direct_extension) {
				ret = ring_start_producer(entry);
				if (ret)
					break;
			}

			/* Signal the threads that we opened the device */
			SLIST_FOREACH(thd, &entry->thdlist_head, dev_list_entry) {
				if (thd->wait_for_open) {
					thd->wait_for_open = false;
					signal_thread(thd, 0);
				}
			}

			IIO_DEBUG("IIO device %s reopened with new mask:\n",
				  dev_label_or_name_or_id(dev));
			for (i = 0; i < nb_words; i++)
				IIO_DEBUG("Mask[%i] = 0x%08x\n", i, entry->mask[i]);
			entry->update_mask = false;

			mask_updated = true;
		}

		sample_size = entry->sample_size;

		SLIST_FOREACH(thd, &entry->thdlist_head, dev_list_entry) {
			thd->active = !thd->err && thd->nb >= sample_size;
			if (mask_updated && thd->active)
				signal_thread(thd, thd->nb);

			if (thd->is_writer)
				has_writers |= thd->active;
			else
				has_readers |= thd->active;
		}
		capture_burst_now = entry->burst.state == IIOD_BURST_CAPTURING;

		if (!has_readers && !has_writers && !capture_burst_now) {
			if (entry->burst.state == IIOD_BURST_SEALED) {
				struct timespec deadline;
				int wait_ret;

				if (clock_gettime(CLOCK_REALTIME, &deadline)) {
					ret = -errno;
					break;
				}
				deadline.tv_sec += IIOD_BURST_SEALED_IDLE_SECONDS;
				wait_ret = pthread_cond_timedwait(&entry->rw_ready_cond,
					&entry->thdlist_lock, &deadline);
				if (wait_ret == ETIMEDOUT &&
						entry->burst.state == IIOD_BURST_SEALED)
					burst_fail(&entry->burst, -ETIMEDOUT);
				else if (wait_ret && wait_ret != ETIMEDOUT) {
					ret = -wait_ret;
					break;
				}
			} else {
				pthread_cond_wait(&entry->rw_ready_cond,
						&entry->thdlist_lock);
			}
		}

		pthread_mutex_unlock(&entry->thdlist_lock);

		if (capture_burst_now) {
			ret = capture_burst(entry);
			finish_burst_capture(entry, (int)ret);
			continue;
		}

		if (!has_readers && !has_writers)
			continue;

		if (has_readers) {
			ssize_t nb_bytes;

			if (entry->direct.producer_started) {
				pthread_mutex_lock(&entry->ring_lock);
				while (!entry->direct.count && !entry->direct.error &&
						!entry->direct.producer_exited)
					pthread_cond_wait(&entry->ring_ready_cond,
						&entry->ring_lock);
				pthread_mutex_unlock(&entry->ring_lock);

				pthread_mutex_lock(&entry->thdlist_lock);
				for (thd = SLIST_FIRST(&entry->thdlist_head);
						thd; thd = next_thd) {
					next_thd = SLIST_NEXT(thd, dev_list_entry);
					if (!thd->active || thd->is_writer ||
							!thd->async_direct)
						continue;
					uint64_t frame_started_ns = iiod_timing_now();
					ret = send_direct_async_data(entry, thd);
					iiod_timing_record(entry,
						IIOD_TIMING_TRANSPORT_FRAME, frame_started_ns);
					iiod_timing_transport_complete(entry, ret);
					if (ret > 0) {
						thd->nb -= (unsigned int)ret;
						if (thd->async_frames_remaining)
							thd->async_frames_remaining--;
						if (thd->async_frames_remaining) {
							thd->nb = (unsigned int)ret;
							thd->new_client = true;
						}
					}
					if (ret < 0 || !thd->async_frames_remaining)
						signal_thread(thd, ret < 0 ? ret :
							(ssize_t)thd->nb);
				}
				pthread_mutex_unlock(&entry->thdlist_lock);
				continue;
			}

			if (entry->ring.producer_started) {
				uint64_t wait_started_ns = iiod_timing_now();

				pthread_mutex_lock(&entry->ring_lock);
				while (entry->ring.core.state ==
						SPF_DDR_RING_STATE_RUNNING &&
						!iiod_ddr_ring_core_consumer_ready(
							&entry->ring.core))
					pthread_cond_wait(&entry->ring_ready_cond,
						&entry->ring_lock);
				pthread_mutex_unlock(&entry->ring_lock);
				iiod_timing_record(entry, IIOD_TIMING_RING_CONSUMER_WAIT,
					wait_started_ns);

				pthread_mutex_lock(&entry->thdlist_lock);
				for (thd = SLIST_FIRST(&entry->thdlist_head);
						thd; thd = next_thd) {
					next_thd = SLIST_NEXT(thd, dev_list_entry);
					if (!thd->active || thd->is_writer)
						continue;
					uint64_t frame_started_ns = iiod_timing_now();
					ret = send_ring_data(entry, thd);
					iiod_timing_record(entry, IIOD_TIMING_TRANSPORT_FRAME,
						frame_started_ns);
					iiod_timing_transport_complete(entry, ret);
					if (ret == -EAGAIN)
						continue;
					if (ret > 0)
						thd->nb -= (unsigned int)ret;
					if (ret < 0 || thd->nb < sample_size)
						signal_thread(thd, ret < 0 ? ret :
							(ssize_t)thd->nb);
				}
				pthread_mutex_unlock(&entry->thdlist_lock);
				continue;
			}

			if (entry->burst.state != IIOD_BURST_OFF) {
				pthread_mutex_lock(&entry->thdlist_lock);
				for (thd = SLIST_FIRST(&entry->thdlist_head);
						thd; thd = next_thd) {
					next_thd = SLIST_NEXT(thd, dev_list_entry);
					if (!thd->active || thd->is_writer)
						continue;
					uint64_t frame_started_ns = iiod_timing_now();
					ret = send_burst_data(entry, thd);
					iiod_timing_record(entry, IIOD_TIMING_TRANSPORT_FRAME,
						frame_started_ns);
					iiod_timing_transport_complete(entry, ret);
					if (ret > 0)
						thd->nb -= (unsigned int)ret;
					else if (ret < 0 &&
						entry->burst.state == IIOD_BURST_SEALED)
						burst_fail(&entry->burst, (int)ret);
					if (ret < 0 || thd->nb < sample_size)
						signal_thread(thd, ret < 0 ? ret :
							(ssize_t)thd->nb);
				}
				pthread_mutex_unlock(&entry->thdlist_lock);
				continue;
			}

			if (entry->metadata_enabled) {
				ret = iiod_timed_metadata_before_refill(entry);
				if (ret == 0) {
					ssize_t refill_ret = iiod_timed_buffer_refill(entry);
					int metadata_ret =
						iiod_timed_metadata_after_refill(entry);
					ret = refill_ret < 0 ? refill_ret :
						(metadata_ret < 0 ? metadata_ret : refill_ret);
				}
			} else {
				ret = iiod_timed_buffer_refill(entry);
			}

			pthread_mutex_lock(&entry->thdlist_lock);

			/*
			 * When the last client disconnects the buffer is
			 * cancelled and iio_buffer_refill() returns an error. A
			 * new client might have connected before we got here
			 * though, in that case the rw thread has to stay active
			 * and a new buffer is created. If the list is still empty the loop
			 * will exit normally.
			 */
			if (entry->cancelled) {
				pthread_mutex_unlock(&entry->thdlist_lock);
				continue;
			}

			if (ret < 0) {
				/* Reading from the device failed - signal the
				 * error to all connected clients. */

				/* Don't use SLIST_FOREACH - see comment below */
				for (thd = SLIST_FIRST(&entry->thdlist_head);
				     thd; thd = next_thd) {
					next_thd = SLIST_NEXT(thd, dev_list_entry);

					if (!thd->active || thd->is_writer)
						continue;

					signal_thread(thd, ret);
				}

				pthread_mutex_unlock(&entry->thdlist_lock);
				continue;
			}

			nb_bytes = ret;

			/* We don't use SLIST_FOREACH here. As soon as a thread is
			 * signaled, its "thd" structure might be freed;
			 * SLIST_FOREACH would then cause a segmentation fault, as it
			 * reads "thd" to get the address of the next element. */
			for (thd = SLIST_FIRST(&entry->thdlist_head);
					thd; thd = next_thd) {
				next_thd = SLIST_NEXT(thd, dev_list_entry);

				if (!thd->active || thd->is_writer)
					continue;

				uint64_t frame_started_ns = iiod_timing_now();
				ret = send_data(entry, thd, nb_bytes);
				iiod_timing_record(entry, IIOD_TIMING_TRANSPORT_FRAME,
					frame_started_ns);
				iiod_timing_transport_complete(entry, ret);
				if (ret > 0)
					thd->nb -= ret;

				if (ret < 0 || thd->nb < sample_size)
					signal_thread(thd, (ret < 0) ?
							ret : (ssize_t) thd->nb);
			}

			pthread_mutex_unlock(&entry->thdlist_lock);
		}

		if (has_writers) {
			ssize_t nb_bytes = 0;

			pthread_mutex_lock(&entry->thdlist_lock);

			/* Reset the size of the buffer to its maximum size.
			 *
			 * XXX(pcercuei): There is no way to perform this with
			 * the public libiio API. However, it probably does not
			 * matter; we only need to reset the size of the buffer
			 * if the buffer was used for receiving samples, and
			 * to date there is no IIO device that supports both
			 * receiving and sending samples.
			 *
			 * entry->buf->data_length = entry->buf->length;
			 */

			/* Same comment as above */
			for (thd = SLIST_FIRST(&entry->thdlist_head);
					thd; thd = next_thd) {
				next_thd = SLIST_NEXT(thd, dev_list_entry);

				if (!thd->active || !thd->is_writer)
					continue;

				ret = receive_data(entry, thd);
				if (ret > 0) {
					thd->nb -= ret;
					if (ret > nb_bytes)
						nb_bytes = ret;
				}

				if (ret < 0)
					signal_thread(thd, ret);
			}

			ret = iio_buffer_push_partial(entry->buf,
				nb_bytes / sample_size);
			if (entry->cancelled) {
				pthread_mutex_unlock(&entry->thdlist_lock);
				continue;
			}

			/* Signal threads which completed their RW command */
			for (thd = SLIST_FIRST(&entry->thdlist_head);
					thd; thd = next_thd) {
				next_thd = SLIST_NEXT(thd, dev_list_entry);

				if (!thd->active || !thd->is_writer)
					continue;

				if (ret < 0)
					signal_thread(thd, ret);
				else if (thd->nb < sample_size)
					signal_thread(thd, thd->nb);
			}

			pthread_mutex_unlock(&entry->thdlist_lock);
		}
	}

	if (entry->ring.producer_started) {
		pthread_mutex_lock(&entry->ring_lock);
		(void)iiod_ddr_ring_core_cancel(&entry->ring.core,
			SPF_DDR_RING_REASON_CLIENT_DISCONNECTED);
		pthread_cond_broadcast(&entry->ring_ready_cond);
		pthread_mutex_unlock(&entry->ring_lock);
		if (entry->buf)
			iio_buffer_cancel(entry->buf);
		pthread_mutex_unlock(&entry->thdlist_lock);
		pthread_mutex_lock(&entry->ring_lock);
		while (!entry->ring.producer_exited)
			pthread_cond_wait(&entry->ring_ready_cond,
				&entry->ring_lock);
		pthread_mutex_unlock(&entry->ring_lock);
		pthread_mutex_lock(&entry->thdlist_lock);
	}
	if (entry->direct.producer_started) {
		pthread_mutex_lock(&entry->ring_lock);
		entry->direct.cancelled = true;
		if (entry->ring.direct_extension &&
				!iiod_ddr_ring_core_is_terminal(&entry->ring.core))
			(void)iiod_ddr_ring_core_cancel(&entry->ring.core,
				SPF_DDR_RING_REASON_CLIENT_DISCONNECTED);
		pthread_cond_broadcast(&entry->ring_ready_cond);
		pthread_mutex_unlock(&entry->ring_lock);
		if (entry->buf)
			iio_buffer_cancel(entry->buf);
		pthread_mutex_unlock(&entry->thdlist_lock);
		pthread_mutex_lock(&entry->ring_lock);
		while (!entry->direct.producer_exited)
			pthread_cond_wait(&entry->ring_ready_cond,
				&entry->ring_lock);
		pthread_mutex_unlock(&entry->ring_lock);
		pthread_mutex_lock(&entry->thdlist_lock);
	}

	/* Signal all remaining threads */
	for (thd = SLIST_FIRST(&entry->thdlist_head); thd; thd = next_thd) {
		next_thd = SLIST_NEXT(thd, dev_list_entry);
		SLIST_REMOVE(&entry->thdlist_head, thd, ThdEntry, dev_list_entry);
		thd->wait_for_open = false;
		signal_thread(thd, ret);
	}
	/* Direct frames lease entry->buf storage and must be returned first. */
	direct_async_release_storage(&entry->direct);
	if (entry->buf) {
		iio_buffer_destroy(entry->buf);
		entry->buf = NULL;
	}
	if (entry->metadata_provider_context) {
		iiod_buffer_metadata_close(entry->metadata_provider_context);
		entry->metadata_provider_context = NULL;
	}
	burst_release_storage(&entry->burst);
	ring_release_storage(&entry->ring);
	if (entry->metadata_timeout_floor_active) {
		metadata_timeout_floor_release();
		entry->metadata_timeout_floor_active = false;
	}
	entry->closed = true;
	pthread_cond_broadcast(&entry->rw_ready_cond);
	pthread_mutex_unlock(&entry->thdlist_lock);

	pthread_mutex_lock(&devlist_lock);
	/* It is possible that a new thread has already started, make sure to
	 * not overwrite it. */
	if (iio_device_get_data(dev) == entry)
		iio_device_set_data(dev, NULL);
	pthread_mutex_unlock(&devlist_lock);

	IIO_DEBUG("Stopping R/W thread for device %s\n",
		  dev_label_or_name_or_id(dev));

	dev_entry_put(entry);
}

static struct ThdEntry *parser_lookup_thd_entry(struct parser_pdata *pdata,
	struct iio_device *dev)
{
	struct ThdEntry *t;

	SLIST_FOREACH(t, &pdata->thdlist_head, parser_list_entry) {
		if (t->dev == dev)
			return t;
	}

	return NULL;
}

static ssize_t rw_buffer(struct parser_pdata *pdata,
		struct iio_device *dev, unsigned int nb, bool is_write,
		unsigned int metadata_capacity, unsigned int async_frames)
{
	struct DevEntry *entry;
	struct ThdEntry *thd;
	bool ring_extension;
	ssize_t ret;

	if (!dev)
		return -ENODEV;

	thd = parser_lookup_thd_entry(pdata, dev);
	if (!thd)
		return -EBADF;

	entry = thd->entry;
	ring_extension = entry->ring.direct_extension;
	if (metadata_capacity && (is_write ||
			metadata_capacity > IIOD_MAX_BUFFER_METADATA_BYTES ||
			thd->sample_size != entry->sample_size ||
			nb != entry->sample_size * entry->samples_count))
		return -EINVAL;
	if (async_frames && (!metadata_capacity || is_write ||
		async_frames > IIO_BUFFER_METADATA_BATCH_MAX ||
		entry->burst_plan.requested_iq_bytes ||
		(entry->burst_plan.ring_capacity_iq_bytes && !ring_extension)))
		return -EINVAL;
	if (!async_frames && metadata_capacity && ring_extension)
		return -EINVAL;

	if (nb < entry->sample_size)
		return 0;

	pthread_mutex_lock(&entry->thdlist_lock);
	if (entry->closed) {
		pthread_mutex_unlock(&entry->thdlist_lock);
		return -EBADF;
	}

	if (thd->nb) {
		pthread_mutex_unlock(&entry->thdlist_lock);
		return -EBUSY;
	}
	if (async_frames) {
		ret = direct_async_prepare(&entry->direct,
			iio_device_get_kernel_buffers_count(dev),
			ring_extension ? entry->ring.core.slot_count : 0U,
			metadata_capacity, async_frames);
		if (ret) {
			pthread_mutex_unlock(&entry->thdlist_lock);
			return ret;
		}
		ret = direct_async_start_producer(entry);
		if (ret) {
			direct_async_release_storage(&entry->direct);
			pthread_mutex_unlock(&entry->thdlist_lock);
			return ret;
		}
	}

	thd->new_client = true;
	thd->nb = nb;
	thd->err = 0;
	thd->is_writer = is_write;
	thd->metadata_enabled = metadata_capacity != 0;
	thd->metadata_capacity = metadata_capacity;
	thd->async_direct = async_frames != 0U;
	thd->async_frames_remaining = async_frames;
	thd->active = true;

	pthread_cond_signal(&entry->rw_ready_cond);

	IIO_DEBUG("Waiting for completion...\n");
	while (thd->active) {
		ret = thd_entry_event_wait(thd, &entry->thdlist_lock, pdata->fd_in);
		if (ret)
			break;
	}
	if (ret == 0)
		ret = thd->err;
	pthread_mutex_unlock(&entry->thdlist_lock);

	if (ret > 0 && ret < (ssize_t) nb)
		print_value(thd->pdata, 0);

	IIO_DEBUG("Exiting rw_buffer with code %li\n", (long) ret);
	if (ret < 0)
		return ret;
	else
		return nb - ret;
}

static uint32_t *get_mask(const char *mask, size_t *len)
{
	size_t nb = (*len + 7) / 8;
	uint32_t *ptr, *words = calloc(nb, sizeof(*words));
	if (!words)
		return NULL;

	ptr = words + nb;
	while (*mask) {
		char buf[9];
		snprintf(buf, sizeof(buf), "%.*s", 8, mask);
		sscanf(buf, "%08x", --ptr);
		mask += 8;
		IIO_DEBUG("Mask[%lu]: 0x%08x\n",
				(unsigned long) (words - ptr) / 4, *ptr);
	}

	*len = nb;
	return words;
}

static void free_thd_entry(struct ThdEntry *t)
{
	close(t->eventfd);
	free(t->mask);
	free(t);
}

static void remove_thd_entry(struct ThdEntry *t)
{
	struct DevEntry *entry = t->entry;

	pthread_mutex_lock(&entry->thdlist_lock);
	if (!entry->closed) {
		entry->update_mask = true;
		SLIST_REMOVE(&entry->thdlist_head, t, ThdEntry, dev_list_entry);
		if (SLIST_EMPTY(&entry->thdlist_head) && entry->buf) {
			entry->cancelled = true;
			if (entry->ring.producer_started) {
				pthread_mutex_lock(&entry->ring_lock);
				(void)iiod_ddr_ring_core_cancel(&entry->ring.core,
					SPF_DDR_RING_REASON_CLIENT_DISCONNECTED);
				pthread_cond_broadcast(&entry->ring_ready_cond);
				pthread_mutex_unlock(&entry->ring_lock);
			}
			if (entry->direct.producer_started) {
				pthread_mutex_lock(&entry->ring_lock);
				entry->direct.cancelled = true;
				if (entry->ring.direct_extension &&
						!iiod_ddr_ring_core_is_terminal(
							&entry->ring.core))
					(void)iiod_ddr_ring_core_cancel(&entry->ring.core,
						SPF_DDR_RING_REASON_CLIENT_DISCONNECTED);
				pthread_cond_broadcast(&entry->ring_ready_cond);
				pthread_mutex_unlock(&entry->ring_lock);
			}
			iio_buffer_cancel(entry->buf); /* Wakeup the rw thread */
		}

		pthread_cond_signal(&entry->rw_ready_cond);
	}
	pthread_mutex_unlock(&entry->thdlist_lock);
	dev_entry_put(entry);

	free_thd_entry(t);
}

static ssize_t get_dev_sample_size_mask(const struct iio_device *dev,
					const uint32_t *mask, size_t words)
{
	unsigned int i, len, number,
		     nb_channels = iio_device_get_channels_count(dev);
	const struct iio_channel *prev = NULL;
	const struct iio_channel *chn;
	const struct iio_data_format *fmt;
	long index;
	ssize_t size = 0;

	if (words != (nb_channels + 31) / 32)
		return -EINVAL;

	for (i = 0; i < nb_channels; i++) {
		chn = iio_device_get_channel(dev, i);
		number = get_channel_number(chn);
		fmt = iio_channel_get_data_format(chn);
		index = iio_channel_get_index(chn);
		len = fmt->length / 8 * fmt->repeat;

		if (index < 0)
			break;
		if (!TEST_BIT(mask, number))
			continue;

		if (prev && index == iio_channel_get_index(prev)) {
			prev = chn;
			continue;
		}

		if (size % len)
			size += 2 * len - (size % len);
		else
			size += len;

		prev = chn;
	}

	return size;
}

static int open_dev_helper(struct parser_pdata *pdata, struct iio_device *dev,
		size_t samples_count, const char *mask, bool cyclic,
		bool metadata_enabled, const void *metadata_request,
		size_t metadata_request_bytes)
{
	int ret = -ENOMEM;
	struct DevEntry *entry;
	struct ThdEntry *thd;
	size_t len = strlen(mask);
	uint32_t *words;
	unsigned int nb_channels;
	unsigned int cyclic_retry = 500;

	if (!dev)
		return -ENODEV;

	nb_channels = iio_device_get_channels_count(dev);
	if (len != ((nb_channels + 31) / 32) * 8)
		return -EINVAL;

	words = get_mask(mask, &len);
	if (!words)
		return -ENOMEM;

	thd = zalloc(sizeof(*thd));
	if (!thd)
		goto err_free_words;

	thd->wait_for_open = true;
	thd->mask = words;
	thd->nb = 0;
	thd->samples_count = samples_count;
	thd->sample_size = get_dev_sample_size_mask(dev, words, len);
	thd->pdata = pdata;
	thd->dev = dev;
	thd->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

retry:
	/* Atomically look up the thread and make sure that it is still active
	 * or allocate new one. */
	pthread_mutex_lock(&devlist_lock);
	entry = iio_device_get_data(dev);
	if (entry) {
		if (cyclic || entry->cyclic || metadata_enabled ||
				entry->metadata_enabled) {
			/* Only one client allowed in cyclic mode */
			pthread_mutex_unlock(&devlist_lock);

			/* There is an inherent race condition if a client
			 * creates a new cyclic buffer shortly after destroying
			 * a previous. E.g. like
			 *
			 *     iio_buffer_destroy(buf);
			 *     buf = iio_device_create_buffer(dev, n, true);
			 *
			 * In this case the two buffers each use their own
			 * communication channel which are unordered to each
			 * other. E.g. the socket open might arrive before the
			 * socket close on the host side, even though they were
			 * sent in the opposite order on the client side. This
			 * race condition can cause an error being reported back
			 * to the client, even though the code on the client
			 * side was well formed and would work fine e.g. using
			 * the local backend.
			 *
			 * To avoid this issue go to sleep for up to 50ms in
			 * intervals of 100us. This should be enough time for
			 * the issue to resolve itself. If there actually is
			 * contention on the buffer an error will eventually be
			 * returned in which case the additional delay cause by
			 * the retires should not matter too much.
			 *
			 * This is not pretty but it works.
			 */
			if (cyclic_retry) {
				struct timespec wait;
				wait.tv_sec = 0;
				wait.tv_nsec = (100 * 1000);
				cyclic_retry--;
				nanosleep(&wait, &wait);
				goto retry;
			}

			ret = -EBUSY;
			goto err_free_thd;
		}

		pthread_mutex_lock(&entry->thdlist_lock);
		if (!entry->closed) {
			pthread_mutex_unlock(&devlist_lock);

			entry->ref_count++;

			SLIST_INSERT_HEAD(&entry->thdlist_head, thd, dev_list_entry);
			thd->entry = entry;
			entry->update_mask = true;
			IIO_DEBUG("Added thread to client list\n");

			pthread_cond_signal(&entry->rw_ready_cond);

			/* Wait until the device is opened by the rw thread */
			while (thd->wait_for_open) {
				ret = thd_entry_event_wait(thd, &entry->thdlist_lock, pdata->fd_in);
				if (ret)
					break;
			}
			pthread_mutex_unlock(&entry->thdlist_lock);

			if (ret == 0)
				ret = (int) thd->err;
			if (ret < 0)
				remove_thd_entry(thd);
			else
				SLIST_INSERT_HEAD(&pdata->thdlist_head, thd, parser_list_entry);
			return ret;
		} else {
			pthread_mutex_unlock(&entry->thdlist_lock);
		}
	}

	entry = zalloc(sizeof(*entry));
	if (!entry) {
		pthread_mutex_unlock(&devlist_lock);
		goto err_free_thd;
	}

	entry->ref_count = 2; /* One for thread, one for the client */

	entry->mask = malloc(len * sizeof(*words));
	if (!entry->mask) {
		pthread_mutex_unlock(&devlist_lock);
		goto err_free_entry;
	}

	entry->cyclic = cyclic;
	entry->metadata_enabled = metadata_enabled;
	entry->nb_words = len;
	entry->update_mask = true;
	entry->dev = dev;
	entry->buf = NULL;
	SLIST_INIT(&entry->thdlist_head);
	SLIST_INSERT_HEAD(&entry->thdlist_head, thd, dev_list_entry);
	thd->entry = entry;
	IIO_DEBUG("Added thread to client list\n");

	if (metadata_enabled) {
		ret = iiod_buffer_metadata_open(dev, samples_count, words, len,
				thd->sample_size,
				metadata_request, metadata_request_bytes,
				&entry->metadata_provider_context,
				&entry->metadata_extra_samples,
				&entry->burst_plan);
		if (ret < 0) {
			pthread_mutex_unlock(&devlist_lock);
			goto err_free_entry_mask;
		}
		ret = metadata_timeout_floor_acquire(
			(struct iio_context *)iio_device_get_context(dev));
		if (ret < 0) {
			pthread_mutex_unlock(&devlist_lock);
			goto err_free_entry_mask;
		}
		entry->metadata_timeout_floor_active = true;
	}

	pthread_mutex_init(&entry->thdlist_lock, NULL);
	pthread_cond_init(&entry->rw_ready_cond, NULL);
	pthread_mutex_init(&entry->ring_lock, NULL);
	pthread_cond_init(&entry->ring_ready_cond, NULL);
	pthread_mutex_init(&entry->timing.lock, NULL);
	entry->timing.next_snapshot_frame = IIOD_TIMING_SNAPSHOT_FRAMES;

	if (rw_cpu_affinity >= 0)
		ret = thread_pool_add_thread_on_cpu(main_thread_pool, rw_thd, entry,
			"rw_thd", rw_cpu_affinity);
	else
		ret = thread_pool_add_thread(main_thread_pool, rw_thd, entry,
			"rw_thd");
	if (ret) {
		pthread_mutex_unlock(&devlist_lock);
		pthread_mutex_destroy(&entry->timing.lock);
		pthread_cond_destroy(&entry->ring_ready_cond);
		pthread_mutex_destroy(&entry->ring_lock);
		pthread_cond_destroy(&entry->rw_ready_cond);
		pthread_mutex_destroy(&entry->thdlist_lock);
		goto err_free_entry_mask;
	}

	IIO_DEBUG("Adding new device thread to device list\n");
	iio_device_set_data(dev, entry);
	pthread_mutex_unlock(&devlist_lock);

	pthread_mutex_lock(&entry->thdlist_lock);
	/* Wait until the device is opened by the rw thread */
	while (thd->wait_for_open) {
		ret = thd_entry_event_wait(thd, &entry->thdlist_lock, pdata->fd_in);
		if (ret)
			break;
	}
	pthread_mutex_unlock(&entry->thdlist_lock);

	if (ret == 0)
		ret = (int) thd->err;
	if (ret < 0)
		remove_thd_entry(thd);
	else
		SLIST_INSERT_HEAD(&pdata->thdlist_head, thd, parser_list_entry);
	return ret;

err_free_entry_mask:
	if (entry->metadata_provider_context)
		iiod_buffer_metadata_close(entry->metadata_provider_context);
	if (entry->metadata_timeout_floor_active) {
		metadata_timeout_floor_release();
		entry->metadata_timeout_floor_active = false;
	}
	free(entry->mask);
err_free_entry:
	free(entry);
err_free_thd:
	close(thd->eventfd);
	free(thd);
err_free_words:
	free(words);
	return ret;
}

static int close_dev_helper(struct parser_pdata *pdata, struct iio_device *dev)
{
	bool wait_for_exclusive_close;
	struct DevEntry *entry;
	struct ThdEntry *t;

	if (!dev)
		return -ENODEV;

	t = parser_lookup_thd_entry(pdata, dev);
	if (!t)
		return -ENXIO;

	entry = t->entry;
	pthread_mutex_lock(&entry->thdlist_lock);
	wait_for_exclusive_close =
		(entry->cyclic || entry->metadata_enabled) &&
		SLIST_FIRST(&entry->thdlist_head) == t &&
		SLIST_NEXT(t, dev_list_entry) == NULL;
	if (wait_for_exclusive_close)
		entry->ref_count++;
	pthread_mutex_unlock(&entry->thdlist_lock);

	SLIST_REMOVE(&pdata->thdlist_head, t, ThdEntry, parser_list_entry);
	remove_thd_entry(t);

	/*
	 * Exclusive buffers use a separate socket from attribute operations and
	 * subsequent buffer opens.  Do not acknowledge CLOSE while the R/W worker
	 * can still own the kernel buffer: doing so lets the next socket race the
	 * asynchronous teardown and observe a spurious -EBUSY.
	 */
	if (wait_for_exclusive_close) {
		pthread_mutex_lock(&entry->thdlist_lock);
		while (!entry->closed)
			pthread_cond_wait(&entry->rw_ready_cond,
					&entry->thdlist_lock);
		pthread_mutex_unlock(&entry->thdlist_lock);
		dev_entry_put(entry);
	}

	return 0;
}

int open_dev(struct parser_pdata *pdata, struct iio_device *dev,
		size_t samples_count, const char *mask, bool cyclic)
{
	int ret = open_dev_helper(pdata, dev, samples_count, mask, cyclic, false,
			NULL, 0);
	print_value(pdata, ret);
	return ret;
}

int open_dev_with_metadata(struct parser_pdata *pdata,
		struct iio_device *dev, size_t samples_count, const char *mask,
		size_t request_bytes)
{
	void *request = NULL;
	ssize_t received;
	int ret;

	if (!request_bytes)
		ret = -EINVAL;
	else if (request_bytes > IIO_BUFFER_METADATA_REQUEST_MAX)
		ret = -E2BIG;
	else {
		request = malloc(request_bytes);
		if (!request)
			ret = -ENOMEM;
		else {
			received = read_all(pdata, request, request_bytes);
			if (received < 0)
				ret = (int)received;
			else if ((size_t)received != request_bytes)
				ret = -EIO;
			else
				ret = open_dev_helper(pdata, dev, samples_count, mask,
						false, true, request, request_bytes);
		}
	}
	free(request);
	print_value(pdata, ret);
	return ret;
}

int close_dev(struct parser_pdata *pdata, struct iio_device *dev)
{
	int ret = close_dev_helper(pdata, dev);
	print_value(pdata, ret);
	return ret;
}

ssize_t rw_dev(struct parser_pdata *pdata, struct iio_device *dev,
		unsigned int nb, bool is_write)
{
	ssize_t ret = rw_buffer(pdata, dev, nb, is_write, 0, 0);
	if (ret <= 0 || is_write)
		print_value(pdata, ret);
	return ret;
}

ssize_t rw_dev_with_metadata(struct parser_pdata *pdata,
		struct iio_device *dev, size_t nb, size_t metadata_capacity)
{
	ssize_t ret;
	if (nb > UINT_MAX || metadata_capacity > UINT_MAX)
		ret = -EOVERFLOW;
	else
		ret = rw_buffer(pdata, dev, (unsigned int)nb, false,
				(unsigned int)metadata_capacity, 0);
	if (ret <= 0)
		print_value(pdata, ret);
	return ret;
}

ssize_t rw_dev_with_metadata_async(struct parser_pdata *pdata,
		struct iio_device *dev, size_t nb, size_t metadata_capacity,
		size_t frames)
{
	ssize_t ret;

	if (!frames)
		ret = -EINVAL;
	else if (nb > UINT_MAX || metadata_capacity > UINT_MAX ||
			frames > UINT_MAX)
		ret = -EOVERFLOW;
	else
		ret = rw_buffer(pdata, dev, (unsigned int)nb, false,
			(unsigned int)metadata_capacity, (unsigned int)frames);
	if (ret <= 0)
		print_value(pdata, ret);
	return ret;
}

ssize_t read_buffer_metadata_status(struct parser_pdata *pdata,
		struct iio_device *dev, size_t status_capacity)
{
	uint8_t wire_status[SPF_DDR_RING_STATUS_BYTES];
	struct spf_ddr_ring_status status = {0};
	struct ThdEntry *thd;
	struct DevEntry *entry;
	ssize_t ret;

	if (!dev)
		ret = -ENODEV;
	else if (status_capacity < sizeof(wire_status))
		ret = -ENOSPC;
	else if (!(thd = parser_lookup_thd_entry(pdata, dev)))
		ret = -EBADF;
	else {
		entry = thd->entry;
		pthread_mutex_lock(&entry->ring_lock);
		if (!entry->ring.producer_started && !entry->ring.direct_extension) {
			ret = -ENODATA;
		} else {
			status.state = entry->ring.core.state;
			status.terminal_reason = entry->ring.core.terminal_reason;
			status.error_code = entry->ring.core.error_code;
			status.requested_capacity_iq_bytes =
				entry->ring.requested_iq_bytes;
			status.admitted_capacity_iq_bytes =
				entry->ring.admitted_iq_bytes;
			status.target_frames = entry->ring.core.target_frames;
			status.produced_frames = entry->ring.core.produced_frames;
			status.consumed_frames = entry->ring.core.consumed_frames;
			status.high_water_frames = entry->ring.core.high_water_frames;
			status.wrap_count = entry->ring.core.wrap_count;
			status.producer_position = entry->ring.core.producer_position;
			status.consumer_position = entry->ring.core.consumer_position;
			if (entry->ring.core.last_contiguous_valid) {
				status.valid_fields |=
					SPF_DDR_RING_STATUS_VALID_LAST_CONTIGUOUS;
				status.last_contiguous_sample_sequence =
					entry->ring.core.last_contiguous_sample_sequence;
			}
			if (entry->ring.core.first_unavailable_valid) {
				status.valid_fields |=
					SPF_DDR_RING_STATUS_VALID_FIRST_UNAVAILABLE;
				status.first_unavailable_sample_sequence =
					entry->ring.core.first_unavailable_sample_sequence;
			}
			ret = spf_ddr_ring_status_encode(wire_status,
				sizeof(wire_status), &status);
		}
		pthread_mutex_unlock(&entry->ring_lock);
		if (!ret) {
			print_value(pdata, sizeof(wire_status));
			ret = write_all(pdata, wire_status, sizeof(wire_status));
			if (ret > 0)
				ret = sizeof(wire_status);
		}
	}
	if (ret < 0)
		print_value(pdata, ret);
	return ret;
}

ssize_t read_dev_attr(struct parser_pdata *pdata, struct iio_device *dev,
		const char *attr, enum iio_attr_type type)
{
	/* We use a very large buffer here, as if attr is NULL all the
	 * attributes will be read, which may represents a few kilobytes worth
	 * of data. */
	char buf[0x10000];
	ssize_t ret = -EINVAL;

	if (!dev) {
		print_value(pdata, -ENODEV);
		return -ENODEV;
	}

	switch (type) {
		case IIO_ATTR_TYPE_DEVICE:
			ret = iio_device_attr_read(dev, attr, buf, sizeof(buf) - 1);
			break;
		case IIO_ATTR_TYPE_DEBUG:
			ret = iio_device_debug_attr_read(dev,
				attr, buf, sizeof(buf) - 1);
			break;
		case IIO_ATTR_TYPE_BUFFER:
			ret = iio_device_buffer_attr_read(dev,
							attr, buf, sizeof(buf) - 1);
			break;
		default:
			ret = -EINVAL;
			break;
	}
	print_value(pdata, ret);
	if (ret < 0)
		return ret;

	buf[ret] = '\n';
	return write_all(pdata, buf, ret + 1);
}

ssize_t write_dev_attr(struct parser_pdata *pdata, struct iio_device *dev,
		const char *attr, size_t len, enum iio_attr_type type)
{
	ssize_t ret = -ENOMEM;
	char *buf;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	buf = malloc(len);
	if (!buf)
		goto err_print_value;

	ret = read_all(pdata, buf, len);
	if (ret < 0)
		goto err_free_buffer;

	switch (type) {
		case IIO_ATTR_TYPE_DEVICE:
			ret = iio_device_attr_write_raw(dev, attr, buf, len);
			break;
		case IIO_ATTR_TYPE_DEBUG:
			ret = iio_device_debug_attr_write_raw(dev, attr, buf, len);
			break;
		case IIO_ATTR_TYPE_BUFFER:
			ret = iio_device_buffer_attr_write_raw(dev, attr, buf, len);
			break;
		default:
			ret = -EINVAL;
			break;
	}

err_free_buffer:
	free(buf);
err_print_value:
	print_value(pdata, ret);
	return ret;
}

ssize_t read_chn_attr(struct parser_pdata *pdata,
		struct iio_channel *chn, const char *attr)
{
	char buf[1024];
	ssize_t ret = -ENODEV;

	if (chn)
		ret = iio_channel_attr_read(chn, attr, buf, sizeof(buf) - 1);
	else if (pdata->dev)
		ret = -ENXIO;
	print_value(pdata, ret);
	if (ret < 0)
		return ret;

	buf[ret] = '\n';
	return write_all(pdata, buf, ret + 1);
}

ssize_t write_chn_attr(struct parser_pdata *pdata,
		struct iio_channel *chn, const char *attr, size_t len)
{
	ssize_t ret = -ENOMEM;
	char *buf = malloc(len);
	if (!buf)
		goto err_print_value;

	ret = read_all(pdata, buf, len);
	if (ret < 0)
		goto err_free_buffer;

	if (chn)
		ret = iio_channel_attr_write_raw(chn, attr, buf, len);
	else if (pdata->dev)
		ret = -ENXIO;
	else
		ret = -ENODEV;
err_free_buffer:
	free(buf);
err_print_value:
	print_value(pdata, ret);
	return ret;
}

ssize_t set_trigger(struct parser_pdata *pdata,
		struct iio_device *dev, const char *trigger)
{
	struct iio_device *trig = NULL;
	ssize_t ret = -ENOENT;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	if (trigger) {
		trig = iio_context_find_device(pdata->ctx, trigger);
		if (!trig)
			goto err_print_value;
	}

	ret = iio_device_set_trigger(dev, trig);
err_print_value:
	print_value(pdata, ret);
	return ret;
}

ssize_t get_trigger(struct parser_pdata *pdata, struct iio_device *dev)
{
	const struct iio_device *trigger;
	ssize_t ret;

	if (!dev) {
		print_value(pdata, -ENODEV);
		return -ENODEV;
	}

	ret = iio_device_get_trigger(dev, &trigger);
	if (!ret && trigger) {
		const char *name = iio_device_get_name(trigger);
		char buf[256];

		ret = strlen(name);
		print_value(pdata, ret);

		snprintf(buf, sizeof(buf), "%s\n", name);
		ret = write_all(pdata, buf, ret + 1);
	} else {
		print_value(pdata, ret);
	}
	return ret;
}

int set_timeout(struct parser_pdata *pdata, unsigned int timeout)
{
	unsigned int effective = timeout;
	int ret;

	pthread_mutex_lock(&metadata_timeout_lock);
	if (metadata_timeout_floor_users != 0U &&
			effective < IIOD_METADATA_LOCAL_IO_TIMEOUT_MIN_MS)
		effective = IIOD_METADATA_LOCAL_IO_TIMEOUT_MIN_MS;
	ret = iio_context_set_timeout(pdata->ctx, effective);
	pthread_mutex_unlock(&metadata_timeout_lock);
	print_value(pdata, ret);
	return ret;
}

int set_buffers_count(struct parser_pdata *pdata,
		struct iio_device *dev, long value)
{
	unsigned int i, nb = (unsigned int) value;
	struct timespec wait;
	int ret = -EINVAL;

	if (!dev) {
		ret = -ENODEV;
		goto err_print_value;
	}

	if (nb >= 1) {
		/*
		 * Avoid the same race condition described in open_dev_helper().
		 * We must be sure that the buffer has not been enabled in order
		 * to set the number of kernel buffers.
		 */
		for (i = 0; i < 500; i++) {
			ret = iio_device_set_kernel_buffers_count(dev, nb);
			if (ret != -EBUSY)
				break;

			wait.tv_sec = 0;
			wait.tv_nsec = (100 * 1000);
			do {
				ret = nanosleep(&wait, &wait);
			} while (ret == -1 && errno == EINTR);
		}
	}
err_print_value:
	print_value(pdata, ret);
	return ret;
}

ssize_t read_line(struct parser_pdata *pdata, char *buf, size_t len)
{
	size_t bytes_read = 0;
	ssize_t ret;
	bool found;

	if (pdata->is_usb)
	      return pdata->readfd(pdata, buf, len);

	if (pdata->fd_in_is_socket) {
		struct pollfd pfd[2];

		pfd[0].fd = pdata->fd_in;
		pfd[0].events = POLLIN | POLLRDHUP;
		pfd[0].revents = 0;
		pfd[1].fd = thread_pool_get_poll_fd(pdata->pool);
		pfd[1].events = POLLIN;
		pfd[1].revents = 0;

		do {
			size_t i, to_trunc;

			poll_nointr(pfd, 2);

			if (pfd[1].revents & POLLIN ||
					pfd[0].revents & POLLRDHUP)
				return 0;

			/* First read from the socket, without advancing the
			 * read offset */
			ret = recv(pdata->fd_in, buf, len,
					MSG_NOSIGNAL | MSG_PEEK);
			if (ret < 0)
				return -errno;

			/* Lookup for the trailing \n */
			for (i = 0; i < (size_t) ret && buf[i] != '\n'; i++);
			found = i < (size_t) ret;

			len -= ret;
			buf += ret;

			to_trunc = found ? i + 1 : (size_t) ret;

			/* Advance the read offset after the \n if found, or
			 * after the last character read otherwise */
			ret = recv(pdata->fd_in, NULL, to_trunc,
					MSG_NOSIGNAL | MSG_TRUNC);
			if (ret < 0)
				return -errno;

			bytes_read += to_trunc;
		} while (!found && len);
	} else {
		while (len) {
			ret = pdata->readfd(pdata, buf, 1);
			if (ret < 0)
			      return ret;

			bytes_read++;

			if (*buf == '\n')
			      break;

			len--;
			buf++;
		}

		found = !!len;
	}

	return found ? (ssize_t) bytes_read : -EIO;
}

void interpreter(struct iio_context *ctx, int fd_in, int fd_out, bool verbose,
		 bool is_socket, bool is_usb, bool use_aio,
		 struct thread_pool *pool, const void *xml_zstd,
		 size_t xml_zstd_len)
{
	yyscan_t scanner;
	struct parser_pdata pdata;
	unsigned int i;
	int ret;

	pdata.ctx = ctx;
	pdata.stop = false;
	pdata.fd_in = fd_in;
	pdata.fd_out = fd_out;
	pdata.verbose = verbose;
	pdata.pool = pool;

	pdata.xml_zstd = xml_zstd;
	pdata.xml_zstd_len = xml_zstd_len;

	pdata.fd_in_is_socket = is_socket;
	pdata.fd_out_is_socket = is_socket;
	pdata.is_usb = is_usb;

	SLIST_INIT(&pdata.thdlist_head);

	if (use_aio) {
		/* Note: if WITH_AIO is not defined, use_aio is always false.
		 * We ensure that in iiod.c. */
#if WITH_AIO
		char err_str[1024];

		pdata.aio_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (pdata.aio_eventfd < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_ERROR("Failed to create AIO eventfd: %s\n", err_str);
			return;
		}

		pdata.aio_ctx = 0;
		ret = io_setup(1, &pdata.aio_ctx);
		if (ret < 0) {
			iio_strerror(-ret, err_str, sizeof(err_str));
			IIO_ERROR("Failed to create AIO context: %s\n", err_str);
			close(pdata.aio_eventfd);
			return;
		}
		pthread_mutex_init(&pdata.aio_mutex, NULL);
		pdata.readfd = readfd_aio;
		pdata.writefd = writefd_aio;
#endif
	} else {
		pdata.readfd = readfd_io;
		pdata.writefd = writefd_io;
	}

	yylex_init_extra(&pdata, &scanner);

	do {
		if (verbose)
			output(&pdata, "iio-daemon > ");
		ret = yyparse(scanner);
	} while (!pdata.stop && ret >= 0);

	yylex_destroy(scanner);

	/* Close all opened devices */
	for (i = 0; i < iio_context_get_devices_count(ctx); i++)
		close_dev_helper(&pdata, iio_context_get_device(ctx, i));

#if WITH_AIO
	if (use_aio) {
		io_destroy(pdata.aio_ctx);
		close(pdata.aio_eventfd);
	}
#endif
}
