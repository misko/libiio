/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-visit-queue.h"
#include "spf-scan-rate.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct block {
	uintptr_t token;
	const void *data;
	unsigned refs;
	bool owned;
};
struct visit {
	uint64_t id, reserved_at, start, end, next, bytes;
	uint32_t samples;
	unsigned remaining_blocks, slice_count, block[SPF_VISIT_QUEUE_MAX_BLOCKS];
	struct spf_visit_slice slices[SPF_VISIT_QUEUE_MAX_BLOCKS];
	enum spf_visit_result result;
	bool bound, closed, sending;
};
struct spf_visit_queue {
	struct spf_visit_queue_config config;
	struct block blocks[SPF_VISIT_QUEUE_MAX_BLOCKS];
	struct visit visits[SPF_VISIT_QUEUE_MAX_VISITS];
	struct spf_visit_queue_stats stats;
	unsigned head;
	uint64_t last_source_end, last_reserved_id, last_valid_end;
	bool have_source, have_id, cancelled;
	int (*release)(void *, uintptr_t);
	void *context;
};
static struct visit *at(struct spf_visit_queue *q, unsigned index)
{ return &q->visits[(q->head + index) % SPF_VISIT_QUEUE_MAX_VISITS]; }
static const struct visit *at_const(const struct spf_visit_queue *q,
				    unsigned index)
{ return &q->visits[(q->head + index) % SPF_VISIT_QUEUE_MAX_VISITS]; }

int spf_visit_queue_create(struct spf_visit_queue **out,
	const struct spf_visit_queue_config *c, int (*release)(void *, uintptr_t), void *ctx)
{
	struct spf_visit_queue *q;
	if (!out || !c || !release || c->block_count < 4 || c->block_count > 64 ||
		!c->headroom_blocks || c->headroom_blocks >= c->block_count ||
		!c->maximum_visits || c->maximum_visits > SPF_VISIT_QUEUE_MAX_VISITS ||
		!c->block_samples || c->block_samples > 1000000 ||
		!spf_scan_rate_valid(c->source_rate_hz) ||
		(c->bytes_per_sample != 4 && c->bytes_per_sample != 8) ||
		!c->maximum_visit_ms || c->maximum_visit_ms > 360 ||
		!c->maximum_bytes || c->maximum_bytes > UINT64_C(200000000) ||
		!c->maximum_age_ticks || c->maximum_age_ticks > (uint64_t)c->source_rate_hz * 10 ||
		!c->drain_bytes_per_second || c->drain_bytes_per_second > UINT64_C(1000000000))
		return -EINVAL;
	q = calloc(1, sizeof(*q));
	if (!q) return -ENOMEM;
	q->config = *c; q->release = release; q->context = ctx;
	*out = q;
	return 0;
}

int spf_visit_queue_reap(struct spf_visit_queue *q)
{
	unsigned i;
	int first_error = 0;
	if (!q) return -EINVAL;
	for (i = 0; i < q->config.block_count; i++) {
		struct block *b = &q->blocks[i];
		int ret;
		if (!b->owned || b->refs) continue;
		ret = q->release(q->context, b->token);
		if (ret) { if (!first_error) first_error = ret; continue; }
		memset(b, 0, sizeof(*b));
		--q->stats.leased_blocks;
	}
	return first_error;
}

static void discard(struct spf_visit_queue *q, struct visit *v, enum spf_visit_result result)
{
	unsigned i;
	for (i = 0; i < v->slice_count; i++) --q->blocks[v->block[i]].refs;
	v->slice_count = 0;
	q->stats.reserved_blocks -= v->remaining_blocks; v->remaining_blocks = 0;
	q->stats.reserved_bytes -= v->bytes; v->bytes = 0;
	v->result = result;
	v->closed = true;
}

int spf_visit_queue_reserve(struct spf_visit_queue *q, uint64_t id,
	uint32_t samples, uint64_t now, enum spf_visit_result *result)
{
	uint64_t bytes = (uint64_t)samples * q->config.bytes_per_sample,
		ticks_to_drain, oldest_age = 0;
	unsigned blocks;
	struct visit *v;
	if (!q || !result || !samples || samples >
	    (uint64_t)q->config.source_rate_hz * q->config.maximum_visit_ms / 1000)
		return -EINVAL;
	if (q->cancelled) return -ESHUTDOWN;
	if (q->have_id && id <= q->last_reserved_id) return -ERANGE;
	if (q->stats.visits && !at(q, q->stats.visits - 1)->bound) return -EBUSY;
	if (q->have_source && now < q->last_source_end) return -ERANGE;
	if (q->stats.visits) {
		if (now < at(q, 0)->reserved_at) return -ERANGE;
		oldest_age = now - at(q, 0)->reserved_at;
	}
	/* Conservative count at any phase of the DMA frame boundary. */
	blocks = (samples + q->config.block_samples - 1) / q->config.block_samples + 1;
	*result = SPF_VISIT_SKIP_CAPACITY;
	if (q->stats.visits == q->config.maximum_visits ||
		blocks + q->stats.leased_blocks + q->stats.reserved_blocks >
		q->config.block_count - q->config.headroom_blocks ||
		bytes > q->config.maximum_bytes ||
		q->stats.reserved_bytes > q->config.maximum_bytes - bytes)
		return 0;
	/* Include capture time and the entire outstanding send, even if the two
 * overlap in practice. Products fit uint64 under create's advertised bounds. */
	ticks_to_drain = ((q->stats.reserved_bytes + bytes) * q->config.source_rate_hz +
		q->config.drain_bytes_per_second - 1) / q->config.drain_bytes_per_second;
	*result = SPF_VISIT_SKIP_AGE;
	if (oldest_age > q->config.maximum_age_ticks ||
		ticks_to_drain + samples > q->config.maximum_age_ticks - oldest_age)
		return 0;
	v = at(q, q->stats.visits++);
	memset(v, 0, sizeof(*v));
	v->id = id; v->reserved_at = now; v->samples = samples;
	v->remaining_blocks = blocks; v->bytes = bytes;
	v->result = *result = SPF_VISIT_ADMITTED;
	q->stats.reserved_blocks += blocks;
	q->stats.reserved_bytes += bytes;
	q->last_reserved_id = id; q->have_id = true;
	if (q->stats.reserved_bytes > q->stats.high_water_bytes)
		q->stats.high_water_bytes = q->stats.reserved_bytes;
	return 0;
}

int spf_visit_queue_bind(struct spf_visit_queue *q, uint64_t id, uint64_t start)
{
	struct visit *v;
	if (!q || !q->stats.visits || q->cancelled) return -EINVAL;
	v = at(q, q->stats.visits - 1);
	if (v->id != id || v->bound || v->result != SPF_VISIT_ADMITTED) return -EINVAL;
	if (start < v->reserved_at || start < q->last_valid_end ||
		start > UINT64_MAX - v->samples) return -ERANGE;
	v->start = v->next = start; v->end = start + v->samples; v->bound = true;
	q->last_valid_end = v->end;
	return 0;
}

int spf_visit_queue_feed(struct spf_visit_queue *q, uintptr_t token,
	const void *data, uint64_t first, uint32_t samples)
{
	unsigned i, slot = SPF_VISIT_QUEUE_MAX_BLOCKS;
	uint64_t end;
	if (!q || !data || !token || samples != q->config.block_samples ||
		first > UINT64_MAX - samples) return -EINVAL;
	if (q->cancelled) return -ESHUTDOWN;
	if (q->have_source && first < q->last_source_end) return -ERANGE;
	if (q->stats.visits && !at(q, q->stats.visits - 1)->bound) return -EAGAIN;
	for (i = 0; i < q->config.block_count; i++) {
		if (q->blocks[i].owned && q->blocks[i].token == token) return -EEXIST;
		if (!q->blocks[i].owned) slot = i;
	}
	if (slot == SPF_VISIT_QUEUE_MAX_BLOCKS) return -ENOSPC;
	end = first + samples;
	q->blocks[slot] = (struct block){token, data, 0, true};
	++q->stats.leased_blocks;
	if (q->stats.leased_blocks > q->stats.high_water_blocks)
		q->stats.high_water_blocks = q->stats.leased_blocks;
	if (q->have_source) q->stats.missing_samples += first - q->last_source_end;
	q->last_source_end = end; q->have_source = true;
	for (i = 0; i < q->stats.visits; i++) {
		struct visit *v = at(q, i);
		uint64_t a, b;
		unsigned s;
		if (v->result != SPF_VISIT_ADMITTED) continue;
		if (first > v->next && v->next < v->end) {
			discard(q, v, SPF_VISIT_INVALID_GAP);
			continue;
		}
		a = first > v->start ? first : v->start;
		b = end < v->end ? end : v->end;
		if (a >= b) continue;
		/* Reservation accounts for every potential slice; violations are
 * explicit corruption, never a partially valid capture. */
		if (a != v->next || !v->remaining_blocks || v->slice_count >= 64) {
			discard(q, v, SPF_VISIT_INVALID_GAP);
			continue;
		}
		s = v->slice_count++;
		v->block[s] = slot;
		v->slices[s] = (struct spf_visit_slice){token, data,
			(size_t)(a - first) * q->config.bytes_per_sample,
			(size_t)(b - a) * q->config.bytes_per_sample};
		++q->blocks[slot].refs;
		--v->remaining_blocks; --q->stats.reserved_blocks;
		v->next = b;
		if (b == v->end) {
			q->stats.reserved_blocks -= v->remaining_blocks; v->remaining_blocks = 0;
			if (v->closed) v->result = SPF_VISIT_COMPLETE;
		}
	}
	/* Lease consumption succeeds independently of release IO. A failed return
 * stays recorded; the owner must call reap and stop on its persistent error. */
	return 0;
}

int spf_visit_queue_close_window(struct spf_visit_queue *q, uint64_t id, uint64_t invalid)
{
	unsigned i;
	if (!q) return -EINVAL;
	for (i = 0; i < q->stats.visits; i++) {
		struct visit *v = at(q, i);
		if (v->id != id) continue;
		if (!v->bound) return -EINVAL;
		/* A source gap may terminally close the window before its nominal
		 * retune boundary. The scheduler still owns that boundary and must be
		 * able to acknowledge it without turning an explicit INVALID_GAP into
		 * a fatal session error. This is also safe while the terminal view is
		 * pinned for transport: there is no state left to mutate here. */
		if (v->closed) return 0;
		if (v->sending) return -EINVAL;
		v->closed = true;
		if (invalid < v->end) discard(q, v, SPF_VISIT_INVALID_GAP);
		else if (v->next == v->end) v->result = SPF_VISIT_COMPLETE;
		return 0;
	}
	return -ENOENT;
}

int spf_visit_queue_take(struct spf_visit_queue *q, struct spf_visit_view *view)
{
	struct visit *v;
	if (!q || !view) return -EINVAL;
	if (!q->stats.visits) return -EAGAIN;
	v = at(q, 0);
	if (v->sending) return -EBUSY;
	if (v->result == SPF_VISIT_ADMITTED) return -EAGAIN;
	memset(view, 0, sizeof(*view));
	view->id = v->id; view->start = v->start; view->end = v->end;
	view->result = v->result; view->slice_count = v->slice_count;
	memcpy(view->slices, v->slices, v->slice_count * sizeof(*v->slices));
	v->sending = true;
	return 0;
}

int spf_visit_queue_complete_send(struct spf_visit_queue *q, uint64_t id)
{
	struct visit *v;
	if (!q || !q->stats.visits) return -EINVAL;
	v = at(q, 0);
	if (!v->sending || v->id != id) return -EINVAL;
	discard(q, v, v->result);
	memset(v, 0, sizeof(*v));
	q->head = (q->head + 1) % SPF_VISIT_QUEUE_MAX_VISITS;
	--q->stats.visits;
	return 0;
}

int spf_visit_queue_expire(struct spf_visit_queue *q, uint64_t now)
{
	unsigned i;
	int ret = 0;
	if (!q) return -EINVAL;
	/* Validate all clocks before making any state changes. */
	for (i = 0; i < q->stats.visits; i++)
		if (now < at(q, i)->reserved_at) return -ERANGE;
	for (i = 0; i < q->stats.visits; i++) {
		struct visit *v = at(q, i);
		if (now - v->reserved_at <= q->config.maximum_age_ticks) continue;
		if (v->sending) ret = -ETIMEDOUT;
		else if (v->bytes) discard(q, v, SPF_VISIT_SKIP_AGE);
	}
	return ret;
}

int spf_visit_queue_cancel(struct spf_visit_queue *q)
{
	unsigned i;
	int ret = 0;
	if (!q) return -EINVAL;
	q->cancelled = true;
	for (i = 0; i < q->stats.visits; i++) {
		struct visit *v = at(q, i);
		if (v->sending) ret = -EBUSY;
		else discard(q, v, SPF_VISIT_CANCELLED);
	}
	return ret;
}

void spf_visit_queue_stats(const struct spf_visit_queue *q, struct spf_visit_queue_stats *stats)
{ if (q && stats) *stats = q->stats; }

bool spf_visit_queue_capture_complete(const struct spf_visit_queue *q)
{
	unsigned i;

	if (!q)
		return false;
	for (i = 0; i < q->stats.visits; i++)
		if (at_const(q, i)->result == SPF_VISIT_ADMITTED)
			return false;
	return true;
}

int spf_visit_queue_destroy(struct spf_visit_queue *q)
{
	if (!q) return -EINVAL;
	if (q->stats.visits || q->stats.leased_blocks) return -EBUSY;
	free(q);
	return 0;
}
