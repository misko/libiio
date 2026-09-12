/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-adaptive-protocol.h"

#include <errno.h>
#include <string.h>

static uint64_t get(const uint8_t *p, unsigned n)
{
	uint64_t v = 0;
	unsigned i;
	for (i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static void put(uint8_t *p, uint64_t v, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static int valid_policy(const struct spf_hop_request_v2 *r)
{
	const struct spf_hop_policy_v2 *p = &r->policy;
	const struct spf_hop_request_v1 *g = &r->geometry;
	if (!p->generation || (p->mode != SPF_HOP_SHADOW && p->mode != SPF_HOP_ADAPTIVE) ||
		(g->sample_rate_hz != 2500000 && g->sample_rate_hz != 5000000) ||
		g->dwell_samples != g->sample_rate_hz * 120 / 1000 ||
		g->dwell_count > 2500 || !p->warmup_visits || p->warmup_visits > 16 ||
		g->dwell_count < p->warmup_visits * SPF_HOP_PROFILE_COUNT ||
		!p->missed_dwells || p->missed_dwells > 32 || !p->quiet_weight ||
		p->quiet_weight > p->active_weight || p->active_weight > 16 ||
		!p->cooldown_ms || p->cooldown_ms > 30000 || p->hop_budget_ms < 120 ||
		p->hop_budget_ms > 1000 || p->maximum_revisit_ms > 30000 ||
		p->maximum_revisit_ms < p->hop_budget_ms * SPF_HOP_PROFILE_COUNT ||
		!p->maximum_result_age_ms || p->maximum_result_age_ms > 10000 ||
		!p->unhealthy_limit || p->unhealthy_limit > 32)
		return -EINVAL;
	return 0;
}

static void policy_encode(uint8_t *p, const struct spf_hop_policy_v2 *c)
{
	put(p, c->generation, 8);
	put(p + 8, c->mode, 4);
	put(p + 12, c->warmup_visits, 4);
	put(p + 16, c->missed_dwells, 4);
	put(p + 20, c->active_weight, 4);
	put(p + 24, c->quiet_weight, 4);
	put(p + 28, c->cooldown_ms, 4);
	put(p + 32, c->maximum_revisit_ms, 4);
	put(p + 36, c->hop_budget_ms, 4);
	put(p + 40, c->maximum_result_age_ms, 4);
	put(p + 44, c->unhealthy_limit, 4);
}

static void policy_decode(struct spf_hop_policy_v2 *c, const uint8_t *p)
{
	c->generation = get(p, 8);
	c->mode = (uint32_t)get(p + 8, 4);
	c->warmup_visits = (uint32_t)get(p + 12, 4);
	c->missed_dwells = (uint32_t)get(p + 16, 4);
	c->active_weight = (uint32_t)get(p + 20, 4);
	c->quiet_weight = (uint32_t)get(p + 24, 4);
	c->cooldown_ms = (uint32_t)get(p + 28, 4);
	c->maximum_revisit_ms = (uint32_t)get(p + 32, 4);
	c->hop_budget_ms = (uint32_t)get(p + 36, 4);
	c->maximum_result_age_ms = (uint32_t)get(p + 40, 4);
	c->unhealthy_limit = (uint32_t)get(p + 44, 4);
}

int spf_hop_request_v2_encode(void *wire, size_t size, const struct spf_hop_request_v2 *r)
{
	uint8_t p[SPF_HOP_ADAPTIVE_REQUEST_BYTES] = {0};
	int ret;
	if (!wire || !r) return -EINVAL;
	if (size < sizeof(p)) return -ENOSPC;
	if (valid_policy(r)) return -EINVAL;
	ret = spf_hop_request_v1_encode(p, SPF_HOP_REQUEST_BYTES, &r->geometry);
	if (ret) return ret;
	put(p + 4, SPF_HOP_ADAPTIVE_VERSION, 2);
	put(p + 6, sizeof(p), 2);
	put(p + 8, SPF_HOP_ADAPTIVE_FEATURES, 4);
	put(p + 76, SPF_HOP_ADAPTIVE_EVENT_BYTES, 2);
	policy_encode(p + SPF_HOP_REQUEST_BYTES, &r->policy);
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_hop_request_v2_decode(struct spf_hop_request_v2 *r, const void *wire, size_t size)
{
	const uint8_t *p = wire;
	uint8_t geometry[SPF_HOP_REQUEST_BYTES];
	struct spf_hop_request_v2 out = {0};
	int ret;
	if (!r || !wire) return -EINVAL;
	if (size != SPF_HOP_ADAPTIVE_REQUEST_BYTES) return -EMSGSIZE;
	if (get(p, 4) != SPF_HOP_REQUEST_MAGIC || get(p + 4, 2) != SPF_HOP_ADAPTIVE_VERSION ||
		get(p + 6, 2) != size) return -EPROTONOSUPPORT;
	if (get(p + 8, 4) != SPF_HOP_ADAPTIVE_FEATURES ||
		get(p + 76, 2) != SPF_HOP_ADAPTIVE_EVENT_BYTES ||
		get(p + 336, 8) || get(p + 344, 8)) return -EBADMSG;
	/* Reuse ONLY numeric geometry validation in a private scratch record.
	 * No adaptive input is emitted or retained under a V1 identity. */
	memcpy(geometry, p, sizeof(geometry));
	put(geometry + 4, SPF_HOP_PROTOCOL_VERSION, 2);
	put(geometry + 6, SPF_HOP_REQUEST_BYTES, 2);
	put(geometry + 8, SPF_HOP_REQUIRED_FEATURES_V1, 4);
	put(geometry + 76, SPF_HOP_EVENT_BYTES, 2);
	ret = spf_hop_request_v1_decode(&out.geometry, geometry, sizeof(geometry));
	if (ret) return ret;
	policy_decode(&out.policy, p + SPF_HOP_REQUEST_BYTES);
	if (valid_policy(&out)) return -EBADMSG;
	*r = out;
	return 0;
}

int spf_hop_choice_v2_validate(const struct spf_hop_choice_v2 *c, const struct spf_hop_event_v1 *e)
{
	if (!c || !e || !c->generation || c->proposed_target >= SPF_HOP_PROFILE_COUNT ||
		c->reason > SPF_HOP_CHOICE_FAULT_FALLBACK ||
		(c->active_mask | c->quiet_mask) > 255 || (c->active_mask & c->quiet_mask) ||
		c->consecutive_misses > 32 || c->cooldown_remaining_samples > UINT64_C(150000000) ||
		(c->basis_visit != UINT64_MAX && c->basis_visit >= e->device.dwell_index) ||
		c->decision_counter > e->device.transition_before ||
		(c->mode != SPF_HOP_SHADOW && c->mode != SPF_HOP_ADAPTIVE) ||
		(c->mode == SPF_HOP_ADAPTIVE && c->proposed_target != e->device.to_profile) ||
		(c->mode == SPF_HOP_SHADOW && e->device.to_profile != e->device.dwell_index % 8))
		return -EINVAL;
	return 0;
}

static void choice_encode(uint8_t *p, const struct spf_hop_choice_v2 *c)
{
	put(p, c->decision_counter, 8);
	put(p + 8, c->basis_visit, 8);
	put(p + 16, c->cooldown_remaining_samples, 8);
	put(p + 24, c->generation, 8);
	put(p + 32, c->proposed_target, 4);
	put(p + 36, c->reason, 4);
	put(p + 40, c->active_mask, 4);
	put(p + 44, c->quiet_mask, 4);
	put(p + 48, c->consecutive_misses, 4);
	put(p + 52, c->mode, 4);
}

static void choice_decode(struct spf_hop_choice_v2 *c, const uint8_t *p)
{
	c->decision_counter = get(p, 8);
	c->basis_visit = get(p + 8, 8);
	c->cooldown_remaining_samples = get(p + 16, 8);
	c->generation = get(p + 24, 8);
	c->proposed_target = (uint32_t)get(p + 32, 4);
	c->reason = (uint32_t)get(p + 36, 4);
	c->active_mask = (uint32_t)get(p + 40, 4);
	c->quiet_mask = (uint32_t)get(p + 44, 4);
	c->consecutive_misses = (uint32_t)get(p + 48, 4);
	c->mode = (uint32_t)get(p + 52, 4);
}

int spf_hop_sidecar_v2_encode(void *wire, size_t size, const struct spf_hop_sidecar_v2 *s)
{
	uint8_t p[SPF_HOP_ADAPTIVE_SIDECAR_MAX_BYTES] = {0}, old[SPF_HOP_SIDECAR_MAX_BYTES];
	size_t bytes;
	unsigned i;
	int ret;
	if (!wire || !s || s->geometry.event_count > SPF_HOP_EVENT_CAPACITY) return -EINVAL;
	bytes = SPF_HOP_SIDECAR_HEADER_BYTES + s->geometry.event_count * SPF_HOP_ADAPTIVE_EVENT_BYTES;
	if (size < bytes) return -ENOSPC;
	ret = spf_hop_sidecar_v1_encode(old, sizeof(old), &s->geometry);
	if (ret < 0) return ret;
	memcpy(p, old, SPF_HOP_SIDECAR_HEADER_BYTES);
	put(p + 4, SPF_HOP_ADAPTIVE_VERSION, 2);
	put(p + 8, bytes, 4);
	put(p + 12, SPF_HOP_ADAPTIVE_FEATURES, 4);
	for (i = 0; i < s->geometry.event_count; ++i) {
		uint8_t *e = p + SPF_HOP_SIDECAR_HEADER_BYTES + i * SPF_HOP_ADAPTIVE_EVENT_BYTES;
		if (spf_hop_choice_v2_validate(&s->choices[i], &s->geometry.events[i])) return -EINVAL;
		memcpy(e, old + SPF_HOP_SIDECAR_HEADER_BYTES + i * SPF_HOP_EVENT_BYTES, SPF_HOP_EVENT_BYTES);
		choice_encode(e + SPF_HOP_EVENT_BYTES, &s->choices[i]);
	}
	memcpy(wire, p, bytes);
	return (int)bytes;
}

int spf_hop_sidecar_v2_decode(struct spf_hop_sidecar_v2 *s, const void *wire, size_t size)
{
	const uint8_t *p = wire;
	struct spf_hop_sidecar_v2 out = {0};
	uint8_t old[SPF_HOP_SIDECAR_MAX_BYTES];
	size_t count, bytes;
	unsigned i;
	int ret;
	if (!s || !wire) return -EINVAL;
	if (size < SPF_HOP_SIDECAR_HEADER_BYTES) return -EMSGSIZE;
	if (get(p, 4) != SPF_HOP_SIDECAR_MAGIC || get(p + 4, 2) != SPF_HOP_ADAPTIVE_VERSION ||
		get(p + 6, 2) != SPF_HOP_SIDECAR_HEADER_BYTES) return -EPROTONOSUPPORT;
	count = (size_t)get(p + 20, 2);
	if (count > SPF_HOP_EVENT_CAPACITY || get(p + 8, 4) != size ||
		size != SPF_HOP_SIDECAR_HEADER_BYTES + count * SPF_HOP_ADAPTIVE_EVENT_BYTES ||
		get(p + 12, 4) != SPF_HOP_ADAPTIVE_FEATURES) return -EBADMSG;
	bytes = SPF_HOP_SIDECAR_HEADER_BYTES + count * SPF_HOP_EVENT_BYTES;
	memcpy(old, p, SPF_HOP_SIDECAR_HEADER_BYTES);
	put(old + 4, SPF_HOP_PROTOCOL_VERSION, 2);
	put(old + 8, bytes, 4);
	put(old + 12, SPF_HOP_REQUIRED_FEATURES_V1, 4);
	for (i = 0; i < count; ++i) {
		const uint8_t *e = p + SPF_HOP_SIDECAR_HEADER_BYTES + i * SPF_HOP_ADAPTIVE_EVENT_BYTES;
		if (get(e + 136, 8)) return -EBADMSG;
		memcpy(old + SPF_HOP_SIDECAR_HEADER_BYTES + i * SPF_HOP_EVENT_BYTES, e, SPF_HOP_EVENT_BYTES);
		choice_decode(&out.choices[i], e + SPF_HOP_EVENT_BYTES);
	}
	ret = spf_hop_sidecar_v1_decode(&out.geometry, old, bytes);
	if (ret) return ret;
	for (i = 0; i < count; ++i)
		if (spf_hop_choice_v2_validate(&out.choices[i], &out.geometry.events[i])) return -EBADMSG;
	*s = out;
	return 0;
}

int spf_hop_status_v2_encode(void *wire, size_t size, const struct spf_hop_status_v1 *s)
{
	uint8_t p[SPF_HOP_STATUS_BYTES];
	int ret;
	if (!wire || !s) return -EINVAL;
	if (size < sizeof(p)) return -ENOSPC;
	ret = spf_hop_status_v1_encode(p, sizeof(p), s);
	if (ret) return ret;
	put(p + 4, SPF_HOP_ADAPTIVE_VERSION, 2);
	put(p + 8, SPF_HOP_ADAPTIVE_FEATURES, 4);
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_hop_status_v2_decode(struct spf_hop_status_v1 *s, const void *wire, size_t size)
{
	const uint8_t *p = wire;
	uint8_t old[SPF_HOP_STATUS_BYTES];
	struct spf_hop_status_v1 out;
	int ret;
	if (!s || !wire) return -EINVAL;
	if (size != sizeof(old)) return -EMSGSIZE;
	if (get(p, 4) != SPF_HOP_STATUS_MAGIC || get(p + 4, 2) != SPF_HOP_ADAPTIVE_VERSION ||
		get(p + 6, 2) != size) return -EPROTONOSUPPORT;
	if (get(p + 8, 4) != SPF_HOP_ADAPTIVE_FEATURES) return -EBADMSG;
	memcpy(old, p, sizeof(old));
	put(old + 4, SPF_HOP_PROTOCOL_VERSION, 2);
	put(old + 8, SPF_HOP_REQUIRED_FEATURES_V1, 4);
	ret = spf_hop_status_v1_decode(&out, old, sizeof(old));
	if (ret) return ret;
	*s = out;
	return 0;
}
