/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-adaptive-policy.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define FEEDBACK_CAPACITY 32U
_Static_assert(sizeof(unsigned) == 4, "feedback requires lock-free 32-bit indices");

struct committed_visit { uint64_t start, end; uint32_t target; };
struct spf_hop_adaptive_policy {
	struct spf_hop_request_v2 request;
	leo_adaptive_scan *scan;
	atomic_uint write, read, fault;
	atomic_uint visible_committed;
	leo_adaptive_observation_v1 feedback[FEEDBACK_CAPACITY];
	struct committed_visit *visits;
	uint64_t source_epoch_offset;
	uint32_t committed;
	int have_source_epoch;
	uint64_t next_host_visit;
};

int spf_hop_adaptive_policy_validate_pinned(const struct spf_hop_request_v2 *r)
{
	const struct spf_hop_policy_v2 *p;
	uint8_t wire[SPF_HOP_HOST_REQUEST_BYTES];
	int ret = spf_hop_adaptive_configuration(wire, sizeof(wire), r);
	if (ret) return ret;
	p = &r->policy;
	return p->warmup_visits == 3 && p->missed_dwells == 3 &&
		p->active_weight == 3 && p->quiet_weight == 1 && p->cooldown_ms == 2000 &&
		p->maximum_revisit_ms == 3000 && p->hop_budget_ms == 160 &&
		p->maximum_result_age_ms == 1000 && p->unhealthy_limit == 3 ? 0 : -ENOTSUP;
}

void spf_hop_adaptive_policy_destroy(struct spf_hop_adaptive_policy *p)
{
	if (!p) return;
	leo_adaptive_destroy(p->scan);
	free(p->visits);
	free(p);
}

int spf_hop_adaptive_policy_create(struct spf_hop_adaptive_policy **out,
	const struct spf_hop_request_v2 *request)
{
	struct spf_hop_adaptive_policy *p;
	const struct spf_hop_policy_v2 *c;
	leo_adaptive_config_v1 config = {0};
	uint8_t wire[SPF_HOP_HOST_REQUEST_BYTES];
	int ret;
	if (!out || !request) return -EINVAL;
	ret = spf_hop_adaptive_configuration(wire, sizeof(wire), request);
	if (ret) return ret;
	p = calloc(1, sizeof(*p));
	if (!p) return -ENOMEM;
	atomic_init(&p->write, 0); atomic_init(&p->read, 0); atomic_init(&p->fault, 0);
	atomic_init(&p->visible_committed, 0);
	if (!atomic_is_lock_free(&p->write) || !atomic_is_lock_free(&p->read) ||
		!atomic_is_lock_free(&p->fault)) { free(p); return -ENOTSUP; }
	p->request = *request;
	p->visits = calloc((size_t)request->geometry.dwell_count, sizeof(*p->visits));
	if (!p->visits) { free(p); return -ENOMEM; }
	c = &request->policy;
	config.session = request->geometry.session_id;
	config.generation = c->generation;
	config.rate_hz = (uint32_t)request->geometry.sample_rate_hz;
	config.target_count = SPF_HOP_PROFILE_COUNT;
	config.maximum_visits = (uint32_t)request->geometry.dwell_count;
	config.warmup_visits = c->warmup_visits;
	config.missed_dwells = c->missed_dwells;
	config.active_weight = c->active_weight;
	config.quiet_weight = c->quiet_weight;
	config.cooldown_ms = c->cooldown_ms;
	config.maximum_revisit_ms = c->maximum_revisit_ms;
	config.hop_budget_ms = c->hop_budget_ms;
	config.maximum_result_age_ms = c->maximum_result_age_ms;
	config.unhealthy_limit = c->unhealthy_limit;
	if (request->host.enabled) {
		leo_adaptive_config_v2 single = {config, request->host.rx, 0};
		ret = leo_adaptive_create_v2(&p->scan, &single);
	} else ret = leo_adaptive_create(&p->scan, &config);
	if (ret) { spf_hop_adaptive_policy_destroy(p); return ret; }
	*out = p;
	return 0;
}

void spf_hop_adaptive_policy_fault(struct spf_hop_adaptive_policy *p)
{ if (p) atomic_store_explicit(&p->fault, 1, memory_order_release); }

int spf_hop_adaptive_policy_offer(struct spf_hop_adaptive_policy *p,
	const leo_adaptive_observation_v1 *o)
{
	unsigned w, r;
	if (!p) return -EINVAL;
	if (!o || o->session != p->request.geometry.session_id ||
		o->generation != p->request.policy.generation ||
		o->rx != (p->request.host.enabled ? p->request.host.rx : 1) ||
		o->rate_hz != p->request.geometry.sample_rate_hz ||
		o->visit >= p->request.geometry.dwell_count || o->target >= SPF_HOP_PROFILE_COUNT ||
		o->outcome > LEO_ADAPTIVE_NOT_DETECTED || o->healthy > 1 ||
		(!o->healthy && o->outcome != LEO_ADAPTIVE_UNKNOWN) ||
		o->valid_end <= o->valid_start ||
		o->valid_end - o->valid_start != p->request.geometry.dwell_samples) {
		spf_hop_adaptive_policy_fault(p);
		return -EINVAL;
	}
	w = atomic_load_explicit(&p->write, memory_order_relaxed);
	r = atomic_load_explicit(&p->read, memory_order_acquire);
	if (w - r >= FEEDBACK_CAPACITY) {
		spf_hop_adaptive_policy_fault(p);
		return -ENOBUFS;
	}
	p->feedback[w % FEEDBACK_CAPACITY] = *o;
	atomic_store_explicit(&p->write, w + 1, memory_order_release);
	return 0;
}

static int bind_observation(struct spf_hop_adaptive_policy *p,
	leo_adaptive_observation_v1 *o)
{
	const struct committed_visit *v;
	uint64_t offset;
	if (o->visit >= p->committed) return -EINVAL;
	v = &p->visits[o->visit];
	if (o->target != v->target || o->valid_start < v->start) return -EINVAL;
	/* Local counter starts from the public low-32 register; IQ provides its
	 * full epoch. Bind once to the same visit, then require the SAME epoch
	 * offset for every later result. Never independently modulo-map each job. */
	offset = o->valid_start - v->start;
	if ((offset & UINT64_C(0xffffffff)) || o->valid_end < v->end ||
		o->valid_end - v->end != offset ||
		(p->have_source_epoch && offset != p->source_epoch_offset)) return -EINVAL;
	p->source_epoch_offset = offset; p->have_source_epoch = 1;
	o->valid_start = v->start; o->valid_end = v->end;
	return 0;
}

int spf_hop_adaptive_policy_offer_host(struct spf_hop_adaptive_policy *p,
	const struct spf_hop_host_feedback_v1 *f, uint64_t stream_id, uint64_t now)
{
	uint8_t wire[SPF_HOP_HOST_FEEDBACK_BYTES];
	const struct committed_visit *v;
	uint64_t offset;
	int ret;
	if (!p || !p->request.host.enabled) return -ENOTSUP;
	if (!f || spf_hop_host_feedback_v1_encode(wire,sizeof(wire),f)) return -EINVAL;
	if (f->session!=p->request.geometry.session_id || f->generation!=p->request.policy.generation ||
		f->stream_id!=stream_id || f->rx!=p->request.host.rx ||
		memcmp(f->configuration_sha256,p->request.host.configuration_sha256,32)) return -ESTALE;
	if (f->visit<p->next_host_visit) return -EALREADY;
	if (f->visit!=p->next_host_visit ||
		f->visit>=atomic_load_explicit(&p->visible_committed,memory_order_acquire)) return -ERANGE;
	v=&p->visits[f->visit];
	if (f->target!=v->target || f->valid_start<v->start || f->valid_end<v->end) return -EINVAL;
	offset=f->valid_start-v->start;
	if ((offset & UINT64_C(0xffffffff)) || f->valid_end-v->end!=offset) return -EINVAL;
	if (now<f->valid_end || now-f->valid_end>p->request.geometry.sample_rate_hz) return -ESTALE;
	leo_adaptive_observation_v1 o={f->session,f->generation,f->visit,f->valid_start,f->valid_end,
		f->source_rate_hz,f->rx,f->target,f->outcome,f->healthy};
	ret=spf_hop_adaptive_policy_offer(p,&o);
	if (!ret) ++p->next_host_visit;
	return ret;
}

static int choose(void *opaque, uint64_t visit, uint64_t now, struct spf_hop_choice_v2 *out)
{
	struct spf_hop_adaptive_policy *p = opaque;
	leo_adaptive_choice_v1 choice;
	unsigned n;
	int ret;
	if (!p || !out || visit != p->committed) return -EINVAL;
	if (atomic_load_explicit(&p->fault, memory_order_acquire)) leo_adaptive_fallback(p->scan);
	for (n = 0; n < SPF_HOP_PROFILE_COUNT; ++n) {
		unsigned r = atomic_load_explicit(&p->read, memory_order_relaxed);
		unsigned w = atomic_load_explicit(&p->write, memory_order_acquire);
		leo_adaptive_observation_v1 observation;
		if (r == w) break;
		observation = p->feedback[r % FEEDBACK_CAPACITY];
		atomic_store_explicit(&p->read, r + 1, memory_order_release);
		ret = bind_observation(p, &observation);
		/* Age at the decision boundary: a queued result cannot freshen an old
		 * detection merely because its producer copied it earlier. */
		if (!ret) ret = leo_adaptive_observe(p->scan, &observation, now);
		if (ret && ret != -EALREADY) {
			spf_hop_adaptive_policy_fault(p);
			leo_adaptive_fallback(p->scan);
		}
	}
	ret = leo_adaptive_choose(p->scan, now, &choice);
	if (ret) return ret;
	*out = (struct spf_hop_choice_v2){choice.decision_counter, choice.basis_visit,
		choice.cooldown_remaining_samples, p->request.policy.generation, choice.target,
		choice.reason, choice.active_mask, choice.quiet_mask, choice.consecutive_misses,
		p->request.policy.mode};
	return 0;
}

static int commit(void *opaque, const struct spf_hop_device_event_v2 *e,
	uint64_t start, uint64_t end)
{
	struct spf_hop_adaptive_policy *p = opaque;
	int ret;
	if (!p || !e || e->device.dwell_index != p->committed ||
		p->committed >= p->request.geometry.dwell_count ||
		e->choice.generation != p->request.policy.generation ||
		e->choice.mode != p->request.policy.mode) return -EINVAL;
	ret = leo_adaptive_commit_actual(p->scan, e->device.to_profile, start, end);
	if (ret) return ret;
	p->visits[p->committed++] = (struct committed_visit){start, end, e->device.to_profile};
	atomic_store_explicit(&p->visible_committed,p->committed,memory_order_release);
	return 0;
}

const struct spf_hop_scheduler_policy_v2 *spf_hop_adaptive_policy_ports(void)
{
	static const struct spf_hop_scheduler_policy_v2 ports = {choose, commit};
	return &ports;
}
