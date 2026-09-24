/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scan-policy.h"
#include "spf-scan-rate.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct scan_visit {
	uint64_t start, end;
	uint32_t target;
	bool finished, intact;
};
struct pending_feedback {
	struct spf_scan_feedback value;
	uint64_t received;
	bool occupied;
};
struct spf_scan_policy {
	struct spf_scan_policy_config config;
	struct scan_visit *visits;
	uint64_t start, end, dwell, transition, revisit, age, delay, decay;
	uint64_t rng, last_now, visit_count, max_visits;
	uint64_t last_start[SPF_SCAN_TARGETS], boost_time[SPF_SCAN_TARGETS];
	uint32_t boost[SPF_SCAN_TARGETS];
	bool active[SPF_SCAN_TARGETS];
	uint64_t channel_sequence[SPF_SCAN_TARGETS], highest_sequence, seen;
	struct pending_feedback pending[SPF_SCAN_TARGETS];
	struct spf_scan_ack acks[SPF_SCAN_ACK_CAPACITY];
	unsigned ack_head, ack_count, pending_count;
	struct spf_scan_choice selection;
	bool selected, stopped;
};

static uint64_t ticks(const struct spf_scan_policy_config *c, uint32_t ms)
{
	return spf_scan_ticks(c->source_rate_hz, ms);
}

int spf_scan_policy_validate(const struct spf_scan_policy_config *c)
{
	unsigned i;
	uint8_t digest = 0;
	uint64_t slot;
	if (!c || !c->session || !c->generation || !c->seed ||
		!c->targets || c->targets > SPF_SCAN_TARGETS ||
		!c->duration_ms || c->duration_ms > 300000 ||
		(c->protocol_version == 3 ?
		 (c->dwell_ms != 120 && c->dwell_ms != 240 && c->dwell_ms != 360) :
		 (c->dwell_ms < 20 || c->dwell_ms > 240)) ||
		!c->transition_budget_ms || c->transition_budget_ms > 100 ||
		!c->maximum_revisit_ms || c->maximum_revisit_ms > 10000 ||
		!c->feedback_age_ms || c->feedback_age_ms > 10000 ||
		!c->application_delay_ms || c->application_delay_ms > 10000 ||
		!c->decay_ms || c->decay_ms > 60000 ||
		c->maximum_boost < 1 || c->maximum_boost > 16)
		return -EINVAL;
	if (c->protocol_version > 3 ||
	    (c->protocol_version == 3 && c->source_rate_hz != 2500000 &&
	     c->source_rate_hz != 10000000) ||
	    (c->protocol_version != 3 && !spf_scan_rate_valid(c->source_rate_hz)))
		return -EOPNOTSUPP;
	for (i = 0; i < sizeof(c->analysis_digest); i++)
		digest |= c->analysis_digest[i];
	if (!digest)
		return -EINVAL;
	for (i = 0; i < SPF_SCAN_TARGETS; i++)
		if ((i < c->targets && (!c->baseline[i] || c->baseline[i] > 1024)) ||
			(i >= c->targets && c->baseline[i]))
			return -EINVAL;
	slot = (uint64_t)c->dwell_ms + c->transition_budget_ms;
	if (slot * c->targets > c->maximum_revisit_ms ||
		slot > c->application_delay_ms || slot > c->duration_ms)
		return -ERANGE;
	return 0;
}

int spf_scan_policy_create(struct spf_scan_policy **out,
	const struct spf_scan_policy_config *c, uint64_t start)
{
	struct spf_scan_policy *p;
	unsigned i;
	int ret = spf_scan_policy_validate(c);
	if (!out)
		return -EINVAL;
	if (ret)
		return ret;
	if (start > UINT64_MAX - ticks(c, c->duration_ms) - ticks(c, c->maximum_revisit_ms))
		return -EOVERFLOW;
	p = calloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;
	p->config = *c;
	p->start = p->last_now = start;
	p->end = start + ticks(c, c->duration_ms);
	p->dwell = ticks(c, c->dwell_ms);
	p->transition = ticks(c, c->transition_budget_ms);
	p->revisit = ticks(c, c->maximum_revisit_ms);
	p->age = ticks(c, c->feedback_age_ms);
	p->delay = ticks(c, c->application_delay_ms);
	p->decay = ticks(c, c->decay_ms);
	p->rng = c->seed;
	p->max_visits = c->duration_ms /
		(c->protocol_version == 3 ? 120U : c->dwell_ms) + 1;
	if (p->max_visits > SPF_SCAN_MAX_VISITS) {
		free(p);
		return -E2BIG;
	}
	p->visits = calloc((size_t)p->max_visits, sizeof(*p->visits));
	if (!p->visits) {
		free(p);
		return -ENOMEM;
	}
	for (i = 0; i < c->targets; i++) {
		p->last_start[i] = p->boost_time[i] = start;
		p->boost[i] = SPF_SCAN_WEIGHT_ONE;
	}
	*out = p;
	return 0;
}

void spf_scan_policy_destroy(struct spf_scan_policy *p)
{
	if (p) {
		free(p->visits);
		free(p);
	}
}

static uint32_t decayed(const struct spf_scan_policy *p, unsigned target, uint64_t now)
{
	uint64_t elapsed = now - p->boost_time[target];
	if (elapsed >= p->decay)
		return SPF_SCAN_WEIGHT_ONE;
	return SPF_SCAN_WEIGHT_ONE + (uint32_t)((uint64_t)
		(p->boost[target] - SPF_SCAN_WEIGHT_ONE) * (p->decay - elapsed) / p->decay);
}

static bool active_at(const struct spf_scan_policy *p, unsigned target, uint64_t now)
{
	return p->active[target] && now - p->boost_time[target] < p->decay;
}

static void terminal_ack(struct spf_scan_policy *p, unsigned target,
	uint32_t result, uint64_t now, uint32_t before, uint32_t after)
{
	struct pending_feedback *f = &p->pending[target];
	struct spf_scan_ack *a = &p->acks[(p->ack_head + p->ack_count) % SPF_SCAN_ACK_CAPACITY];
	*a = (struct spf_scan_ack){f->value.sequence, f->value.visit,
		result == SPF_SCAN_APPLIED ? p->visit_count : UINT64_MAX,
		f->received, now, target, result, before, after};
	++p->ack_count;
	--p->pending_count;
	f->occupied = false;
}

/* Selecting candidate must leave a feasible EDF ordering for every other
 * target at the declared worst-case dwell/transition budget. This prevents a
 * lottery from painting itself into an impossible pair of imminent deadlines. */
static bool feasible(const struct spf_scan_policy *p, unsigned candidate, uint64_t now)
{
	uint64_t deadlines[SPF_SCAN_TARGETS], t = now + p->transition;
	unsigned n = 0, i, j;
	if (t > p->last_start[candidate] + p->revisit)
		return false;
	for (i = 0; i < p->config.targets; i++) {
		uint64_t d;
		if (i == candidate)
			continue;
		d = p->last_start[i] + p->revisit;
		j = n++;
		while (j && deadlines[j - 1] > d) {
			deadlines[j] = deadlines[j - 1];
			--j;
		}
		deadlines[j] = d;
	}
	for (i = 0; i < n; i++) {
		t += p->dwell + p->transition;
		if (t > deadlines[i])
			return false;
	}
	return true;
}

static uint64_t random64(struct spf_scan_policy *p)
{
	uint64_t x = p->rng;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	p->rng = x;
	return x * UINT64_C(2685821657736338717);
}

int spf_scan_policy_select(struct spf_scan_policy *p, uint64_t now,
	struct spf_scan_choice *choice)
{
	uint64_t weights[SPF_SCAN_TARGETS] = {0}, total = 0, draw, threshold;
	uint32_t mask = 0;
	unsigned i, chosen = 0;
	if (!p || !choice)
		return -EINVAL;
	if (p->stopped)
		return -ESHUTDOWN;
	if (p->selected)
		return -EBUSY;
	if (now < p->last_now || (p->visit_count && now < p->visits[p->visit_count - 1].end))
		return -ERANGE;
	if (now >= p->end || p->end - now <
	    (p->config.protocol_version == 3 ? ticks(&p->config, 120U) : p->dwell) +
	    p->transition)
		return -ENODATA;
	if (p->visit_count >= p->max_visits)
		return -EOVERFLOW;
	/* Verify timing/fairness before consuming feedback. */
	for (i = 0; i < p->config.targets; i++)
		if (feasible(p, i, now))
			mask |= 1U << i;
	if (!mask)
		return -ETIME;
	for (i = 0; i < p->config.targets; i++) {
		struct pending_feedback *f = &p->pending[i];
		uint32_t before = decayed(p, i, now), after = before;
		if (f->occupied) {
			if (now - f->received > p->delay || now - f->value.valid_end > p->age) {
				terminal_ack(p, i, SPF_SCAN_EXPIRED, now, before, before);
			} else {
				if (f->value.outcome == SPF_SCAN_ACTIVE)
					after = p->config.maximum_boost * SPF_SCAN_WEIGHT_ONE;
				else if (f->value.outcome == SPF_SCAN_QUIET)
					after = SPF_SCAN_WEIGHT_ONE + (before - SPF_SCAN_WEIGHT_ONE) / 2;
				if (p->config.protocol_version == 3 &&
				    f->value.outcome != SPF_SCAN_UNKNOWN)
					p->active[i] = f->value.outcome == SPF_SCAN_ACTIVE;
				/* UNKNOWN does not refresh a decaying boost. */
				if (f->value.outcome != SPF_SCAN_UNKNOWN) {
					p->boost[i] = after;
					p->boost_time[i] = now;
				}
				terminal_ack(p, i, SPF_SCAN_APPLIED, now, before, after);
			}
		}
		if (mask & (1U << i)) {
			weights[i] = (uint64_t)p->config.baseline[i] * decayed(p, i, now);
			total += weights[i];
		}
	}
	/* Rejection sampling avoids modulo bias; the seed makes replay exact. */
	threshold = (UINT64_C(0) - total) % total;
	do { draw = random64(p); } while (draw < threshold);
	draw %= total;
	for (i = 0; i < p->config.targets; i++) {
		if (draw < weights[i]) { chosen = i; break; }
		draw -= weights[i];
	}
	p->selection = (struct spf_scan_choice){
		.visit = p->visit_count, .selection_counter = now, .target = chosen,
		.eligible_mask = mask, .effective_weight = (uint32_t)weights[chosen],
		.dwell_ms = p->config.protocol_version == 3 && !active_at(p, chosen, now) ?
			120U : p->config.dwell_ms,
		.deadline_forced = (mask & (mask - 1U)) == 0 && p->config.targets > 1,
	};
	if (p->end - now < ticks(&p->config, p->selection.dwell_ms) + p->transition)
		return -ENODATA;
	p->last_now = now;
	p->selected = true;
	*choice = p->selection;
	return 0;
}

int spf_scan_policy_commit(struct spf_scan_policy *p, uint64_t start)
{
	struct scan_visit *v;
	if (!p || !p->selected || p->stopped)
		return -EINVAL;
	/* transition is the required settling minimum, not an expiry.  A PHY
	 * Fast-Lock ioctl may begin after the snapshot used for selection; callers
	 * then commit at its measured completion plus transition. */
	if (start < p->selection.selection_counter || start > p->end ||
		p->end - start < ticks(&p->config, p->selection.dwell_ms))
		return -ETIME;
	v = &p->visits[p->visit_count++];
	*v = (struct scan_visit){start,
		start + ticks(&p->config, p->selection.dwell_ms),
		p->selection.target, false, false};
	p->last_start[v->target] = start;
	p->selected = false;
	p->last_now = start;
	return 0;
}

int spf_scan_policy_finish_visit(struct spf_scan_policy *p, uint64_t visit, bool intact)
{
	if (!p || visit >= p->visit_count)
		return -EINVAL;
	if (p->visits[visit].finished)
		return -EALREADY;
	p->visits[visit].finished = true;
	p->visits[visit].intact = intact;
	return 0;
}

enum spf_scan_feedback_result spf_scan_policy_feedback(struct spf_scan_policy *p,
	const struct spf_scan_feedback *f, uint64_t now)
{
	const struct scan_visit *v;
	uint64_t distance;
	unsigned t;
	if (!p || !f || p->stopped)
		return SPF_SCAN_REJECTED;
	if (f->session != p->config.session || f->generation != p->config.generation)
		return SPF_SCAN_WRONG_SESSION;
	if (!f->sequence || f->target >= p->config.targets || f->outcome > SPF_SCAN_QUIET ||
		f->visit >= p->visit_count || memcmp(f->analysis_digest, p->config.analysis_digest, 32))
		return SPF_SCAN_REJECTED;
	v = &p->visits[f->visit];
	if (!v->finished || !v->intact || f->target != v->target ||
		f->valid_start != v->start || f->valid_end != v->end || now < v->end ||
		now < p->last_now)
		return SPF_SCAN_REJECTED;
	if (now - v->end > p->age)
		return SPF_SCAN_EXPIRED;
	if (f->sequence <= p->highest_sequence) {
		distance = p->highest_sequence - f->sequence;
		if (distance >= 64)
			return SPF_SCAN_EXPIRED;
		if (p->seen & (UINT64_C(1) << distance))
			return SPF_SCAN_DUPLICATE;
	}
	t = f->target;
	if (f->sequence <= p->channel_sequence[t])
		return SPF_SCAN_SUPERSEDED;
	/* Reserve a terminal receipt even when an older pending item is replaced. */
	if (p->ack_count + p->pending_count >= SPF_SCAN_ACK_CAPACITY)
		return SPF_SCAN_MAILBOX_FULL;
	if (p->pending[t].occupied) {
		uint32_t weight = decayed(p, t, now);
		terminal_ack(p, t, SPF_SCAN_SUPERSEDED, now, weight, weight);
	}
	if (f->sequence > p->highest_sequence) {
		distance = f->sequence - p->highest_sequence;
		p->seen = distance >= 64 ? 0 : p->seen << distance;
		p->highest_sequence = f->sequence;
	}
	p->seen |= UINT64_C(1) << (p->highest_sequence - f->sequence);
	p->channel_sequence[t] = f->sequence;
	p->pending[t] = (struct pending_feedback){*f, now, true};
	++p->pending_count;
	p->last_now = now;
	return SPF_SCAN_ACCEPTED;
}

int spf_scan_policy_take_ack(struct spf_scan_policy *p, struct spf_scan_ack *ack)
{
	if (!p || !ack)
		return -EINVAL;
	if (!p->ack_count)
		return -EAGAIN;
	*ack = p->acks[p->ack_head];
	p->ack_head = (p->ack_head + 1) % SPF_SCAN_ACK_CAPACITY;
	--p->ack_count;
	return 0;
}

void spf_scan_policy_stop(struct spf_scan_policy *p, uint64_t now)
{
	unsigned i;
	if (!p || p->stopped)
		return;
	if (now < p->last_now)
		now = p->last_now;
	for (i = 0; i < p->config.targets; i++)
		if (p->pending[i].occupied) {
			uint32_t weight = decayed(p, i, now);
			terminal_ack(p, i, SPF_SCAN_CANCELLED, now, weight, weight);
		}
	p->stopped = true;
}
