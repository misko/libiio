/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_VISIT_QUEUE_H
#define SPF_VISIT_QUEUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPF_VISIT_QUEUE_MAX_BLOCKS 64U
#define SPF_VISIT_QUEUE_MAX_VISITS 64U

enum spf_visit_result {
	SPF_VISIT_ADMITTED, SPF_VISIT_COMPLETE, SPF_VISIT_SKIP_CAPACITY,
	SPF_VISIT_SKIP_AGE, SPF_VISIT_INVALID_GAP, SPF_VISIT_CANCELLED
};
struct spf_visit_queue;
struct spf_visit_queue_config {
	unsigned block_count, headroom_blocks;
	unsigned maximum_visits;
	uint32_t block_samples, source_rate_hz;
	uint64_t maximum_bytes, maximum_age_ticks, drain_bytes_per_second;
	uint32_t bytes_per_sample;
};
struct spf_visit_slice {
	uintptr_t token;
	const void *data;
	size_t offset, bytes;
};
struct spf_visit_view {
	uint64_t id, start, end;
	enum spf_visit_result result;
	unsigned slice_count;
	struct spf_visit_slice slices[SPF_VISIT_QUEUE_MAX_BLOCKS];
};
struct spf_visit_queue_stats {
	uint64_t reserved_bytes, high_water_bytes, missing_samples;
	unsigned leased_blocks, reserved_blocks, high_water_blocks, visits;
};

/* The acquisition owner serializes this API with the session mutex. The
 * release callback returns 0 only after the DMA lease is actually returned.
 * A failed return stays owned and is retried; destroy refuses owned leases. */
int spf_visit_queue_create(struct spf_visit_queue **out,
	const struct spf_visit_queue_config *config,
	int (*release)(void *context, uintptr_t token), void *context);
int spf_visit_queue_destroy(struct spf_visit_queue *queue);
/* Reserve BEFORE recall. SKIP has no IQ slot; the caller must persist its
 * outcome in the session execution ledger. Only one unbound visit is allowed. */
int spf_visit_queue_reserve(struct spf_visit_queue *queue, uint64_t id,
	uint32_t samples, uint64_t now, enum spf_visit_result *result);
int spf_visit_queue_bind(struct spf_visit_queue *queue, uint64_t id,
	uint64_t valid_start);
/* feed consumes the lease only on success. Every nonzero return leaves it
 * with the caller. Missing source intervals invalidate intersected visits. */
int spf_visit_queue_feed(struct spf_visit_queue *queue, uintptr_t token,
	const void *data, uint64_t first, uint32_t samples);
/* Actual following transition (or terminal counter) attests no early retune. */
int spf_visit_queue_close_window(struct spf_visit_queue *queue, uint64_t id,
	uint64_t first_invalid_counter);
int spf_visit_queue_take(struct spf_visit_queue *queue, struct spf_visit_view *view);
/* A view pins all its slices until the consumer completes or aborts send. */
int spf_visit_queue_complete_send(struct spf_visit_queue *queue, uint64_t id);
int spf_visit_queue_expire(struct spf_visit_queue *queue, uint64_t now);
int spf_visit_queue_cancel(struct spf_visit_queue *queue);
int spf_visit_queue_reap(struct spf_visit_queue *queue);
void spf_visit_queue_stats(const struct spf_visit_queue *queue,
	struct spf_visit_queue_stats *stats);
/* True once every closed capture window has either complete IQ or an explicit
 * terminal outcome. Transport may still hold or queue those complete views. */
bool spf_visit_queue_capture_complete(const struct spf_visit_queue *queue);
#endif
