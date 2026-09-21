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

static int valid_policy(const struct spf_hop_request_v2 *r, int host)
{
	const struct spf_hop_policy_v2 *p = &r->policy;
	const struct spf_hop_request_v1 *g = &r->geometry;
	if (!p->generation || (p->mode != SPF_HOP_SHADOW && p->mode != SPF_HOP_ADAPTIVE) ||
		(host==1 ? g->sample_rate_hz != 10000000 : host==2 ?
		 (g->sample_rate_hz != 15000000 && g->sample_rate_hz != 20000000) :
		 (g->sample_rate_hz != 2500000 && g->sample_rate_hz != 5000000)) ||
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

static uint32_t eligible_target_mask(const struct spf_hop_request_v2 *r)
{
	return r->eligible_target_mask ? r->eligible_target_mask : UINT32_C(0xff);
}

int spf_hop_request_v2_encode(void *wire, size_t size, const struct spf_hop_request_v2 *r)
{
	uint8_t p[SPF_HOP_ADAPTIVE_REQUEST_BYTES] = {0};
	int ret;
	if (!wire || !r) return -EINVAL;
	if (size < sizeof(p)) return -ENOSPC;
	if (valid_policy(r, 0) || eligible_target_mask(r) != UINT32_C(0xff)) return -EINVAL;
	const unsigned char empty_host[sizeof(r->host)] = {0};
	if (memcmp(&r->host, empty_host, sizeof(r->host))) return -EINVAL;
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
	out.eligible_target_mask = UINT32_C(0xff);
	if (valid_policy(&out, 0)) return -EBADMSG;
	*r = out;
	return 0;
}

int spf_hop_request_mask_v2_encode(void *wire, size_t size,
	const struct spf_hop_request_v2 *r)
{
	uint8_t p[SPF_HOP_ADAPTIVE_REQUEST_BYTES] = {0};
	const unsigned char empty_host[sizeof(r->host)] = {0};
	uint32_t mask;
	int ret;
	if (!wire || !r) return -EINVAL;
	if (size < sizeof(p)) return -ENOSPC;
	mask = r->eligible_target_mask;
	if (valid_policy(r, 0) || !mask ||
		r->policy.mode != SPF_HOP_ADAPTIVE ||
		memcmp(&r->host, empty_host, sizeof(r->host))) return -EINVAL;
	ret = spf_hop_request_v1_encode(p, SPF_HOP_REQUEST_BYTES, &r->geometry);
	if (ret) return ret;
	put(p + 4, SPF_HOP_ADAPTIVE_VERSION, 2);
	put(p + 6, sizeof(p), 2);
	put(p + 8, SPF_HOP_ADAPTIVE_MASK_FEATURES, 4);
	put(p + 76, SPF_HOP_ADAPTIVE_EVENT_BYTES, 2);
	policy_encode(p + SPF_HOP_REQUEST_BYTES, &r->policy);
	put(p + 336, mask, 1);
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_hop_request_mask_v2_decode(struct spf_hop_request_v2 *r,
	const void *wire, size_t size)
{
	const uint8_t *p = wire;
	uint8_t geometry[SPF_HOP_REQUEST_BYTES];
	struct spf_hop_request_v2 out = {0};
	int ret;
	if (!r || !wire) return -EINVAL;
	if (size != SPF_HOP_ADAPTIVE_REQUEST_BYTES) return -EMSGSIZE;
	if (get(p, 4) != SPF_HOP_REQUEST_MAGIC ||
		get(p + 4, 2) != SPF_HOP_ADAPTIVE_VERSION || get(p + 6, 2) != size)
		return -EPROTONOSUPPORT;
	if (get(p + 8, 4) != SPF_HOP_ADAPTIVE_MASK_FEATURES ||
		get(p + 76, 2) != SPF_HOP_ADAPTIVE_EVENT_BYTES || !p[336] ||
		get(p + 337, 7) || get(p + 344, 8)) return -EBADMSG;
	memcpy(geometry, p, sizeof(geometry));
	put(geometry + 4, SPF_HOP_PROTOCOL_VERSION, 2);
	put(geometry + 6, SPF_HOP_REQUEST_BYTES, 2);
	put(geometry + 8, SPF_HOP_REQUIRED_FEATURES_V1, 4);
	put(geometry + 76, SPF_HOP_EVENT_BYTES, 2);
	ret = spf_hop_request_v1_decode(&out.geometry, geometry, sizeof(geometry));
	if (ret) return ret;
	policy_decode(&out.policy, p + SPF_HOP_REQUEST_BYTES);
	out.eligible_target_mask = p[336];
	if (valid_policy(&out, 0) || out.policy.mode != SPF_HOP_ADAPTIVE)
		return -EBADMSG;
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

static int digest_present(const uint8_t *p)
{
	unsigned any=0;
	for (unsigned i=0; i<32; ++i) any |= p[i];
	return any != 0;
}

int spf_hop_sidecar_v3_encode(void *wire, size_t size, const struct spf_hop_sidecar_v2 *s)
{
	int ret=spf_hop_sidecar_v2_encode(wire,size,s);
	if (ret>=0) { put((uint8_t *)wire+4,3,2); put((uint8_t *)wire+12,0x7f,4); }
	return ret;
}

int spf_hop_sidecar_v3_decode(struct spf_hop_sidecar_v2 *s, const void *wire, size_t size)
{
	uint8_t copy[SPF_HOP_ADAPTIVE_SIDECAR_MAX_BYTES];
	if (!s || !wire) return -EINVAL;
	if (size<SPF_HOP_SIDECAR_HEADER_BYTES || size>sizeof(copy)) return -EMSGSIZE;
	if (get((const uint8_t *)wire+4,2)!=3 || get((const uint8_t *)wire+12,4)!=0x7f)
		return -EPROTONOSUPPORT;
	memcpy(copy,wire,size); put(copy+4,2,2); put(copy+12,SPF_HOP_ADAPTIVE_FEATURES,4);
	return spf_hop_sidecar_v2_decode(s,copy,size);
}

int spf_hop_status_v3_encode(void *wire, size_t size, const struct spf_hop_status_v1 *s)
{
	int ret=spf_hop_status_v2_encode(wire,size,s);
	if (!ret) { put((uint8_t *)wire+4,3,2); put((uint8_t *)wire+8,0x7f,4); }
	return ret;
}

int spf_hop_request_v3_encode(void *wire, size_t size, const struct spf_hop_request_v2 *r)
{
	uint8_t p[SPF_HOP_HOST_REQUEST_BYTES]={0};
	int ret;
	if (!wire || !r) return -EINVAL;
	if (size<sizeof(p)) return -ENOSPC;
	if (valid_policy(r,1) || eligible_target_mask(r)!=UINT32_C(0xff) ||
		r->host.enabled!=1 || r->host.rx>1 ||
		r->host.decision_rate_hz!=2500000 || r->host.factor!=4 || r->host.phase ||
		r->host.delay!=80 || r->host.supported_start!=40 || r->host.supported_end!=300000 ||
		!digest_present(r->host.configuration_sha256)) return -EINVAL;
	ret=spf_hop_request_v1_encode(p,SPF_HOP_REQUEST_BYTES,&r->geometry);
	if (ret) return ret;
	put(p+4,3,2); put(p+6,sizeof(p),2); put(p+8,0x7f,4);
	put(p+76,SPF_HOP_ADAPTIVE_EVENT_BYTES,2);
	policy_encode(p+SPF_HOP_REQUEST_BYTES,&r->policy);
	put(p+352,r->host.enabled,4); put(p+356,r->host.rx,4);
	put(p+360,r->host.decision_rate_hz,4); put(p+364,r->host.factor,4);
	put(p+368,r->host.phase,4); put(p+372,r->host.delay,4);
	put(p+376,r->host.supported_start,4); put(p+380,r->host.supported_end,4);
	memcpy(p+384,r->host.configuration_sha256,32);
	memcpy(wire,p,sizeof(p));
	return 0;
}

int spf_hop_request_v3_decode(struct spf_hop_request_v2 *r, const void *wire, size_t size)
{
	const uint8_t *p=wire;
	uint8_t geometry[SPF_HOP_REQUEST_BYTES], validated[SPF_HOP_HOST_REQUEST_BYTES];
	struct spf_hop_request_v2 out={0};
	int ret;
	if (!r || !wire) return -EINVAL;
	if (size!=SPF_HOP_HOST_REQUEST_BYTES) return -EMSGSIZE;
	if (get(p,4)!=SPF_HOP_REQUEST_MAGIC || get(p+4,2)!=3 || get(p+6,2)!=size)
		return -EPROTONOSUPPORT;
	if (get(p+8,4)!=0x7f || get(p+76,2)!=SPF_HOP_ADAPTIVE_EVENT_BYTES ||
		get(p+336,8) || get(p+344,8)) return -EBADMSG;
	memcpy(geometry,p,sizeof(geometry));
	put(geometry+4,1,2); put(geometry+6,sizeof(geometry),2);
	put(geometry+8,SPF_HOP_REQUIRED_FEATURES_V1,4); put(geometry+76,SPF_HOP_EVENT_BYTES,2);
	ret=spf_hop_request_v1_decode(&out.geometry,geometry,sizeof(geometry));
	if (ret) return ret;
	policy_decode(&out.policy,p+288);
	out.eligible_target_mask=UINT32_C(0xff);
	out.host.enabled=get(p+352,4); out.host.rx=get(p+356,4);
	out.host.decision_rate_hz=get(p+360,4); out.host.factor=get(p+364,4);
	out.host.phase=get(p+368,4); out.host.delay=get(p+372,4);
	out.host.supported_start=get(p+376,4); out.host.supported_end=get(p+380,4);
	memcpy(out.host.configuration_sha256,p+384,32);
	ret=spf_hop_request_v3_encode(validated,sizeof(validated),&out);
	if (ret) return ret;
	*r=out;
	return 0;
}

static int multirate_host_geometry(const struct spf_hop_request_v2 *r)
{
	if (!r) return -EINVAL;
	if (r->geometry.sample_rate_hz==15000000)
		return r->host.factor==6 && r->host.delay==100 && r->host.supported_start==34 ? 0 : -EINVAL;
	if (r->geometry.sample_rate_hz==20000000)
		return r->host.factor==8 && r->host.delay==128 && r->host.supported_start==32 ? 0 : -EINVAL;
	return -EINVAL;
}

int spf_hop_request_v4_encode(void *wire, size_t size, const struct spf_hop_request_v2 *r)
{
	uint8_t p[SPF_HOP_HOST_REQUEST_BYTES]={0};
	int ret;
	if (size<sizeof(p)) return -ENOSPC;
	if (!r || multirate_host_geometry(r) || valid_policy(r,2) ||
		eligible_target_mask(r)!=UINT32_C(0xff) ||
		r->host.enabled!=1 || r->host.rx!=0 || r->host.decision_rate_hz!=2500000 ||
		r->host.phase || r->host.supported_end!=300000 ||
		!digest_present(r->host.configuration_sha256)) return -EINVAL;
	ret=spf_hop_request_v1_encode(p,SPF_HOP_REQUEST_BYTES,&r->geometry);
	if (ret) return ret;
	put(p+4,4,2); put(p+6,sizeof(p),2); put(p+8,0xff,4);
	put(p+76,SPF_HOP_ADAPTIVE_EVENT_BYTES,2);
	policy_encode(p+SPF_HOP_REQUEST_BYTES,&r->policy);
	put(p+352,2,4); put(p+356,r->host.rx,4);
	put(p+360,r->host.decision_rate_hz,4); put(p+364,r->host.factor,4);
	put(p+368,r->host.phase,4); put(p+372,r->host.delay,4);
	put(p+376,r->host.supported_start,4); put(p+380,r->host.supported_end,4);
	memcpy(p+384,r->host.configuration_sha256,32);
	memcpy(wire,p,sizeof(p));
	return 0;
}

int spf_hop_request_v4_decode(struct spf_hop_request_v2 *r, const void *wire, size_t size)
{
	const uint8_t *p=wire;
	uint8_t geometry[SPF_HOP_REQUEST_BYTES], validated[SPF_HOP_HOST_REQUEST_BYTES];
	struct spf_hop_request_v2 out={0};
	int ret;
	if (!r || !wire) return -EINVAL;
	if (size!=SPF_HOP_HOST_REQUEST_BYTES) return -EMSGSIZE;
	if (get(p,4)!=SPF_HOP_REQUEST_MAGIC || get(p+4,2)!=4 || get(p+6,2)!=size ||
		get(p+8,4)!=0xff || get(p+352,4)!=2) return -EPROTONOSUPPORT;
	if (get(p+76,2)!=SPF_HOP_ADAPTIVE_EVENT_BYTES || get(p+336,8) || get(p+344,8))
		return -EBADMSG;
	memcpy(geometry,p,sizeof(geometry));
	put(geometry+4,1,2); put(geometry+6,sizeof(geometry),2);
	put(geometry+8,SPF_HOP_REQUIRED_FEATURES_V1,4); put(geometry+76,SPF_HOP_EVENT_BYTES,2);
	ret=spf_hop_request_v1_decode(&out.geometry,geometry,sizeof(geometry));
	if (ret) return ret;
	policy_decode(&out.policy,p+288);
	out.eligible_target_mask=UINT32_C(0xff);
	out.host.enabled=1; out.host.rx=get(p+356,4);
	out.host.decision_rate_hz=get(p+360,4); out.host.factor=get(p+364,4);
	out.host.phase=get(p+368,4); out.host.delay=get(p+372,4);
	out.host.supported_start=get(p+376,4); out.host.supported_end=get(p+380,4);
	memcpy(out.host.configuration_sha256,p+384,32);
	ret=spf_hop_request_v4_encode(validated,sizeof(validated),&out);
	if (ret || memcmp(validated,p,size)) return -EBADMSG;
	*r=out;
	return 0;
}

int spf_hop_adaptive_configuration(void *out, size_t size, const struct spf_hop_request_v2 *r)
{
	uint8_t bytes[SPF_HOP_HOST_REQUEST_BYTES]={0};
	int ret;
	if (!out || !r) return -EINVAL;
	if (size<sizeof(bytes)) return -ENOSPC;
	ret=r->host.enabled ?
		(r->geometry.sample_rate_hz==10000000 ?
		 spf_hop_request_v3_encode(bytes,sizeof(bytes),r) :
		 spf_hop_request_v4_encode(bytes,sizeof(bytes),r)) :
		(eligible_target_mask(r) == UINT32_C(0xff) ?
		 spf_hop_request_v2_encode(bytes,sizeof(bytes),r) :
		 spf_hop_request_mask_v2_encode(bytes,sizeof(bytes),r));
	if (!ret) memcpy(out,bytes,sizeof(bytes));
	return ret;
}

static int feedback_valid(const struct spf_hop_host_feedback_v1 *f)
{
	int geometry=f && f->source_rate_hz==10000000 && f->factor==4 && f->delay==80 &&
		f->supported_start==40 && f->valid_end-f->valid_start==1200000;
	geometry |= f && f->source_rate_hz==15000000 && f->factor==6 && f->delay==100 &&
		f->supported_start==34 && f->valid_end-f->valid_start==1800000;
	geometry |= f && f->source_rate_hz==20000000 && f->factor==8 && f->delay==128 &&
		f->supported_start==32 && f->valid_end-f->valid_start==2400000;
	return f && f->session && f->generation && f->stream_id && f->visit<2500 &&
		f->event_sequence==f->visit && f->valid_end>f->valid_start &&
		geometry &&
		f->decision_rate_hz==2500000 && f->rx<=1 && f->target<8 && f->outcome<=2 &&
		f->healthy<=1 && (f->healthy || !f->outcome) && f->screen_mask<=63 &&
		(!f->healthy || f->screen_mask==63) && f->confirmation_mask<=32 &&
		!(f->confirmation_mask & (f->confirmation_mask-1)) &&
		(f->outcome!=1 || f->confirmation_mask) &&
		f->supported_end==300000 && !f->phase &&
		digest_present(f->configuration_sha256);
}

int spf_hop_host_feedback_v1_encode(void *wire, size_t size,
	const struct spf_hop_host_feedback_v1 *f)
{
	uint8_t p[SPF_HOP_HOST_FEEDBACK_BYTES]={0};
	if (!wire || !feedback_valid(f)) return -EINVAL;
	if (f->source_rate_hz!=10000000) return -EINVAL;
	if (size<sizeof(p)) return -ENOSPC;
	memcpy(p,"HFB1",4); put(p+4,1,2); put(p+6,sizeof(p),2);
	put(p+8,f->session,8); put(p+16,f->generation,8); put(p+24,f->stream_id,8);
	put(p+32,f->visit,8); put(p+40,f->event_sequence,8);
	put(p+48,f->valid_start,8); put(p+56,f->valid_end,8);
	put(p+64,f->source_rate_hz,4); put(p+68,f->decision_rate_hz,4);
	put(p+72,f->rx,4); put(p+76,f->target,4); put(p+80,f->outcome,4); put(p+84,f->healthy,4);
	put(p+88,f->screen_mask,4); put(p+92,f->confirmation_mask,4);
	put(p+96,f->supported_start,4); put(p+100,f->supported_end,4);
	put(p+104,f->factor,4); put(p+108,f->phase,4); put(p+112,f->delay,4);
	memcpy(p+120,f->configuration_sha256,32);
	memcpy(wire,p,sizeof(p));
	return 0;
}

int spf_hop_host_feedback_v2_encode(void *wire, size_t size,
	const struct spf_hop_host_feedback_v1 *f)
{
	uint8_t p[SPF_HOP_HOST_FEEDBACK_BYTES];
	if (!wire || !feedback_valid(f) || f->source_rate_hz==10000000) return -EINVAL;
	if (size<sizeof(p)) return -ENOSPC;
	memset(p,0,sizeof(p)); memcpy(p,"HFB2",4); put(p+4,2,2); put(p+6,sizeof(p),2);
	put(p+8,f->session,8); put(p+16,f->generation,8); put(p+24,f->stream_id,8);
	put(p+32,f->visit,8); put(p+40,f->event_sequence,8);
	put(p+48,f->valid_start,8); put(p+56,f->valid_end,8);
	put(p+64,f->source_rate_hz,4); put(p+68,f->decision_rate_hz,4);
	put(p+72,f->rx,4); put(p+76,f->target,4); put(p+80,f->outcome,4); put(p+84,f->healthy,4);
	put(p+88,f->screen_mask,4); put(p+92,f->confirmation_mask,4);
	put(p+96,f->supported_start,4); put(p+100,f->supported_end,4);
	put(p+104,f->factor,4); put(p+108,f->phase,4); put(p+112,f->delay,4);
	memcpy(p+120,f->configuration_sha256,32); memcpy(wire,p,sizeof(p));
	return 0;
}

int spf_hop_host_feedback_encode(void *wire, size_t size,
	const struct spf_hop_host_feedback_v1 *f)
{
	return f && f->source_rate_hz==10000000 ?
		spf_hop_host_feedback_v1_encode(wire,size,f) :
		spf_hop_host_feedback_v2_encode(wire,size,f);
}

int spf_hop_host_feedback_v1_decode(struct spf_hop_host_feedback_v1 *f,
	const void *wire, size_t size)
{
	const uint8_t *p=wire;
	struct spf_hop_host_feedback_v1 out={0};
	if (!f || !wire) return -EINVAL;
	if (size!=SPF_HOP_HOST_FEEDBACK_BYTES) return -EMSGSIZE;
	if (memcmp(p,"HFB1",4) || get(p+4,2)!=1 || get(p+6,2)!=size) return -EPROTONOSUPPORT;
	if (get(p+116,4) || get(p+152,8)) return -EBADMSG;
	out.session=get(p+8,8); out.generation=get(p+16,8); out.stream_id=get(p+24,8);
	out.visit=get(p+32,8); out.event_sequence=get(p+40,8);
	out.valid_start=get(p+48,8); out.valid_end=get(p+56,8);
	out.source_rate_hz=get(p+64,4); out.decision_rate_hz=get(p+68,4);
	out.rx=get(p+72,4); out.target=get(p+76,4); out.outcome=get(p+80,4); out.healthy=get(p+84,4);
	out.screen_mask=get(p+88,4); out.confirmation_mask=get(p+92,4);
	out.supported_start=get(p+96,4); out.supported_end=get(p+100,4);
	out.factor=get(p+104,4); out.phase=get(p+108,4); out.delay=get(p+112,4);
	memcpy(out.configuration_sha256,p+120,32);
	if (!feedback_valid(&out)) return -EBADMSG;
	*f=out;
	return 0;
}

int spf_hop_host_feedback_v2_decode(struct spf_hop_host_feedback_v1 *f,
	const void *wire, size_t size)
{
	const uint8_t *p=wire;
	struct spf_hop_host_feedback_v1 out={0};
	uint8_t validated[SPF_HOP_HOST_FEEDBACK_BYTES];
	if (!f || !wire) return -EINVAL;
	if (size!=SPF_HOP_HOST_FEEDBACK_BYTES) return -EMSGSIZE;
	if (memcmp(p,"HFB2",4) || get(p+4,2)!=2 || get(p+6,2)!=size) return -EPROTONOSUPPORT;
	if (get(p+116,4) || get(p+152,8)) return -EBADMSG;
	out.session=get(p+8,8); out.generation=get(p+16,8); out.stream_id=get(p+24,8);
	out.visit=get(p+32,8); out.event_sequence=get(p+40,8);
	out.valid_start=get(p+48,8); out.valid_end=get(p+56,8);
	out.source_rate_hz=get(p+64,4); out.decision_rate_hz=get(p+68,4);
	out.rx=get(p+72,4); out.target=get(p+76,4); out.outcome=get(p+80,4); out.healthy=get(p+84,4);
	out.screen_mask=get(p+88,4); out.confirmation_mask=get(p+92,4);
	out.supported_start=get(p+96,4); out.supported_end=get(p+100,4);
	out.factor=get(p+104,4); out.phase=get(p+108,4); out.delay=get(p+112,4);
	memcpy(out.configuration_sha256,p+120,32);
	if (spf_hop_host_feedback_v2_encode(validated,sizeof(validated),&out) ||
		memcmp(validated,p,size)) return -EBADMSG;
	*f=out;
	return 0;
}
