/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-visit-queue.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
	bool owned[128];
	unsigned released[128];
	uintptr_t fail;
	uint32_t data[128][100];
};
static int release(void *opaque, uintptr_t token)
{
	struct fixture *f = opaque;
	assert(token < 128 && f->owned[token]);
	if (f->fail == token) { f->fail = 0; return -EIO; }
	f->owned[token] = false; ++f->released[token];
	return 0;
}
static struct spf_visit_queue_config config(void)
{
	struct spf_visit_queue_config c = {8, 2, 100, 10000000, 200000000,
		10000000, 60000000};
	return c;
}
static struct spf_visit_queue *create(struct fixture *f, struct spf_visit_queue_config c)
{
	struct spf_visit_queue *q = NULL;
	assert(!spf_visit_queue_create(&q, &c, release, f));
	return q;
}
static void feed(struct fixture *f, struct spf_visit_queue *q, uintptr_t token, uint64_t first)
{
	unsigned i;
	assert(!f->owned[token]); f->owned[token] = true;
	for (i = 0; i < 100; i++) f->data[token][i] = (uint32_t)(first + i);
	assert(!spf_visit_queue_feed(q, token, f->data[token], first, 100));
}
static void admit(struct spf_visit_queue *q, uint64_t id, uint32_t count,
	uint64_t now, uint64_t start)
{
	enum spf_visit_result result;
	assert(!spf_visit_queue_reserve(q, id, count, now, &result));
	assert(result == SPF_VISIT_ADMITTED);
	assert(!spf_visit_queue_bind(q, id, start));
}
static void check_samples(const struct spf_visit_view *v)
{
	uint64_t next = v->start;
	unsigned i;
	assert(v->result == SPF_VISIT_COMPLETE);
	for (i = 0; i < v->slice_count; i++) {
		const struct spf_visit_slice *s = &v->slices[i];
		const uint32_t *values = s->data;
		size_t j;
		assert(s->offset % 4 == 0 && s->bytes % 4 == 0);
		for (j = 0; j < s->bytes / 4; j++)
			assert(values[s->offset / 4 + j] == (uint32_t)next++);
	}
	assert(next == v->end);
}

static void test_shared_slices_and_failed_release(void)
{
	struct fixture f = {0};
	struct spf_visit_queue *q = create(&f, config());
	struct spf_visit_view v;
	struct spf_visit_queue_stats stats;
	uint64_t base = (UINT64_C(1) << 54) + UINT64_C(0xffffff00);
	admit(q, 1, 120, base, base + 25);
	admit(q, 2, 120, base + 145, base + 160);
	feed(&f, q, 1, base); feed(&f, q, 2, base + 100);
	assert(spf_visit_queue_take(q, &v) == -EAGAIN);
	assert(!spf_visit_queue_close_window(q, 1, base + 145));
	assert(!spf_visit_queue_take(q, &v)); check_samples(&v);
	assert(v.slice_count == 2 && v.slices[1].token == 2);
	feed(&f, q, 3, base + 200);
	assert(!spf_visit_queue_close_window(q, 2, base + 280));
	assert(spf_visit_queue_take(q, &v) == -EBUSY);
	assert(!spf_visit_queue_complete_send(q, 1));
	f.fail = 1;
	assert(spf_visit_queue_reap(q) == -EIO);
	assert(f.owned[1] && f.owned[2] && f.owned[3]);
	assert(!spf_visit_queue_reap(q));
	assert(!f.owned[1] && f.owned[2]);
	assert(!spf_visit_queue_take(q, &v)); check_samples(&v);
	assert(v.slices[0].token == 2);
	assert(spf_visit_queue_destroy(q) == -EBUSY);
	assert(!spf_visit_queue_complete_send(q, 2));
	assert(!spf_visit_queue_reap(q));
	assert(f.released[1] == 1 && f.released[2] == 1 && f.released[3] == 1);
	spf_visit_queue_stats(q, &stats);
	assert(!stats.visits && !stats.leased_blocks && !stats.reserved_blocks && !stats.reserved_bytes);
	assert(!spf_visit_queue_destroy(q));
}

static void test_gap_and_early_retune(void)
{
	struct fixture f = {0};
	struct spf_visit_queue *q = create(&f, config());
	struct spf_visit_view v;
	struct spf_visit_queue_stats stats;
	admit(q, 1, 120, 0, 25); admit(q, 2, 120, 145, 160);
	feed(&f, q, 1, 0); feed(&f, q, 2, 200);
	assert(!spf_visit_queue_take(q, &v));
	assert(v.id == 1 && v.result == SPF_VISIT_INVALID_GAP && !v.slice_count);
	assert(!spf_visit_queue_complete_send(q, 1));
	assert(!spf_visit_queue_take(q, &v));
	assert(v.id == 2 && v.result == SPF_VISIT_INVALID_GAP && !v.slice_count);
	assert(!spf_visit_queue_complete_send(q, 2));
	assert(!spf_visit_queue_reap(q));
	spf_visit_queue_stats(q, &stats); assert(stats.missing_samples == 100);
	admit(q, 3, 120, 300, 310);
	feed(&f, q, 3, 300); feed(&f, q, 4, 400);
	assert(!spf_visit_queue_close_window(q, 3, 429));
	assert(!spf_visit_queue_take(q, &v));
	assert(v.result == SPF_VISIT_INVALID_GAP && !v.slice_count);
	assert(!spf_visit_queue_complete_send(q, 3));
	assert(!spf_visit_queue_reap(q)); assert(!spf_visit_queue_destroy(q));
}

static void test_admission_age_and_cancel_pinned_send(void)
{
	struct fixture f = {0};
	struct spf_visit_queue_config c = config();
	struct spf_visit_queue *q;
	struct spf_visit_view v;
	enum spf_visit_result result;
	c.maximum_bytes = 480;
	q = create(&f, c);
	admit(q, 1, 120, 0, 25);
	assert(!spf_visit_queue_reserve(q, 2, 120, 145, &result));
	assert(result == SPF_VISIT_SKIP_CAPACITY);
	feed(&f, q, 1, 0); feed(&f, q, 2, 100);
	assert(!spf_visit_queue_close_window(q, 1, 145));
	assert(!spf_visit_queue_take(q, &v));
	assert(spf_visit_queue_expire(q, c.maximum_age_ticks + 1) == -ETIMEDOUT);
	assert(spf_visit_queue_cancel(q) == -EBUSY);
	assert(!spf_visit_queue_reap(q));
	assert(f.owned[1] && f.owned[2]); check_samples(&v);
	assert(!spf_visit_queue_complete_send(q, 1));
	assert(!spf_visit_queue_reap(q)); assert(!spf_visit_queue_destroy(q));
	c = config(); c.maximum_age_ticks = 120;
	q = create(&f, c);
	assert(!spf_visit_queue_reserve(q, 1, 120, 0, &result));
	assert(result == SPF_VISIT_SKIP_AGE);
	assert(!spf_visit_queue_destroy(q));
	c = config(); q = create(&f, c);
	admit(q, 1, 120, 0, 25);
	assert(!spf_visit_queue_expire(q, c.maximum_age_ticks + 1));
	assert(!spf_visit_queue_take(q, &v)); assert(v.result == SPF_VISIT_SKIP_AGE);
	assert(!spf_visit_queue_complete_send(q, 1));
	assert(!spf_visit_queue_destroy(q));
}

static void test_every_boundary(void)
{
	unsigned offset, count;
	for (offset = 0; offset < 100; offset++) {
		for (count = 1; count <= 240; count++) {
			struct fixture f = {0};
			struct spf_visit_queue *q = create(&f, config());
			struct spf_visit_view v;
			unsigned source = 0, token = 1;
			admit(q, 1, count, 0, offset);
			/* Both event-before-IQ and IQ-before-event are legal. */
			if (count & 1) assert(!spf_visit_queue_close_window(q, 1, offset + count));
			while (source < offset + count) {
				feed(&f, q, token++, source); source += 100;
			}
			if (!(count & 1)) assert(!spf_visit_queue_close_window(q, 1, offset + count));
			assert(!spf_visit_queue_take(q, &v)); check_samples(&v);
			assert(!spf_visit_queue_complete_send(q, 1));
			assert(!spf_visit_queue_reap(q)); assert(!spf_visit_queue_destroy(q));
		}
	}
}

int main(void)
{
	test_shared_slices_and_failed_release();
	test_gap_and_early_retune();
	test_admission_age_and_cancel_pinned_send();
	test_every_boundary();
	puts("visit queue: 24000 boundary cases, shared leases, exact gaps, age, cancel and release retry PASS");
	return 0;
}
