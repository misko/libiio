/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-adaptive-policy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static struct spf_hop_request_v2 request(unsigned rate, unsigned mode)
{
	struct spf_hop_request_v2 r = {0};
	unsigned i;
	r.geometry.required_features = SPF_HOP_REQUIRED_FEATURES_V1;
	r.geometry.flags = SPF_HOP_REQUEST_FLAGS_V1;
	r.geometry.session_id = 71;
	r.geometry.sample_rate_hz = r.geometry.rf_bandwidth_hz = rate;
	r.geometry.dwell_samples = rate * 120ULL / 1000;
	r.geometry.transition_guard_samples = rate / 1000;
	r.geometry.dwell_count = 2500;
	r.geometry.capture_span_samples = rate * 300ULL;
	for (i = 0; i < 8; ++i) {
		r.geometry.profiles[i].profile_id = r.geometry.profiles[i].fastlock_slot = (uint8_t)i;
		r.geometry.profiles[i].center_frequency_hz = r.geometry.profiles[i].lo_frequency_hz = 1000000000 + i * 1000000;
		r.geometry.profiles[i].profile_crc32 = 100 + i;
	}
	r.policy = (struct spf_hop_policy_v2){9, mode, 3, 3, 3, 1, 2000, 3000, 160, 1000, 3};
	return r;
}

static leo_adaptive_observation_v1 visit(struct spf_hop_adaptive_policy *p,
	const struct spf_hop_request_v2 *r, uint64_t index, uint64_t *now,
	struct spf_hop_choice_v2 *choice)
{
	const struct spf_hop_scheduler_policy_v2 *ports = spf_hop_adaptive_policy_ports();
	struct spf_hop_device_event_v2 e = {0};
	leo_adaptive_observation_v1 o = {0};
	uint64_t start, end;
	assert(!ports->choose(p, index, *now, choice));
	e.choice = *choice;
	e.device.dwell_index = index;
	e.device.to_profile = r->policy.mode == SPF_HOP_SHADOW ? index % 8 : choice->proposed_target;
	e.device.transition_before = *now;
	e.device.transition_after = *now + 2;
	start = e.device.transition_after + r->geometry.transition_guard_samples;
	end = start + r->geometry.dwell_samples;
	assert(!ports->commit(p, &e, start, end));
	*now = end;
	o.session = 71; o.generation = 9; o.visit = index;
	/* Source IQ and local register counter differ by a fixed full epoch. */
	o.valid_start = start + UINT64_C(0x1000000000000);
	o.valid_end = end + UINT64_C(0x1000000000000);
	o.rate_hz = (uint32_t)r->geometry.sample_rate_hz;
	o.rx = 1; o.target = e.device.to_profile; o.healthy = 1;
	return o;
}

static void all_masks(unsigned rate, unsigned mode)
{
	struct spf_hop_request_v2 r = request(rate, mode);
	unsigned mask;
	for (mask = 0; mask < 256; ++mask) {
		struct spf_hop_adaptive_policy *p;
		uint64_t now = UINT64_C(0xfffffff0);
		unsigned index, counts[8] = {0}, sum = 0;
		assert(!spf_hop_adaptive_policy_create(&p, &r));
		for (index = 0; index < 448; ++index) {
			struct spf_hop_choice_v2 c;
			leo_adaptive_observation_v1 o = visit(p, &r, index, &now, &c);
			if (index < 24) assert(c.reason == SPF_HOP_CHOICE_WARMUP && o.target == index % 8);
			if (index >= 48) {
				assert(c.active_mask == mask && c.quiet_mask == (mask ^ 255));
				assert(c.reason != SPF_HOP_CHOICE_FAULT_FALLBACK);
				++counts[o.target];
			}
			o.outcome = mask & (1U << o.target) ? LEO_ADAPTIVE_DETECTED : LEO_ADAPTIVE_NOT_DETECTED;
			assert(!spf_hop_adaptive_policy_offer(p, &o));
		}
		for (index = 0; index < 8; ++index) sum += mask && (mask & (1U << index)) ? 3 : 1;
		for (index = 0; index < 8; ++index) {
			unsigned expected = mode == SPF_HOP_SHADOW ? 50 :
				400 * (mask && (mask & (1U << index)) ? 3 : 1) / sum;
			assert(counts[index] + 3 >= expected && counts[index] <= expected + 3);
		}
		spf_hop_adaptive_policy_destroy(p);
	}
}

static void fault_cases(void)
{
	unsigned kind;
	for (kind = 0; kind < 5; ++kind) {
		struct spf_hop_request_v2 r = request(5000000, SPF_HOP_ADAPTIVE);
		struct spf_hop_adaptive_policy *p;
		struct spf_hop_choice_v2 c;
		uint64_t now = 100;
		leo_adaptive_observation_v1 o;
		unsigned i;
		assert(!spf_hop_adaptive_policy_create(&p, &r));
		o = visit(p, &r, 0, &now, &c);
		o.outcome = LEO_ADAPTIVE_DETECTED;
		assert(!spf_hop_adaptive_policy_offer(p, &o));
		o = visit(p, &r, 1, &now, &c);
		if (kind == 0) {
			for (i = 0; i < 32; ++i) assert(!spf_hop_adaptive_policy_offer(p, &o));
			assert(spf_hop_adaptive_policy_offer(p, &o) == -ENOBUFS);
		} else if (kind == 1) {
			++o.generation;
			assert(spf_hop_adaptive_policy_offer(p, &o) == -EINVAL);
		} else if (kind == 2) {
			o.valid_start += UINT64_C(0x100000000); o.valid_end += UINT64_C(0x100000000);
			assert(!spf_hop_adaptive_policy_offer(p, &o));
		} else if (kind == 3) {
			o.target = (o.target + 1) % 8;
			assert(!spf_hop_adaptive_policy_offer(p, &o));
		} else spf_hop_adaptive_policy_fault(p);
		for (i = 2; i < 40; ++i) {
			o = visit(p, &r, i, &now, &c);
			assert(c.reason == SPF_HOP_CHOICE_FAULT_FALLBACK && o.target == i % 8);
			o.outcome = LEO_ADAPTIVE_DETECTED;
			/* Full-queue case drains in bounded batches, then catches up. */
			if (i >= 8) assert(!spf_hop_adaptive_policy_offer(p, &o));
		}
		spf_hop_adaptive_policy_destroy(p);
	}
}

struct handoff {
	struct spf_hop_adaptive_policy *policy;
	leo_adaptive_observation_v1 observation;
	atomic_uint ready, done;
};

static void wait_for(atomic_uint *value, unsigned target)
{
	unsigned attempt;
	for (attempt = 0; attempt < 1000000; ++attempt) {
		if (atomic_load_explicit(value, memory_order_acquire) == target) return;
		sched_yield();
	}
	assert(!"bounded synthetic handoff timed out");
}

static void *producer(void *opaque)
{
	struct handoff *h = opaque;
	unsigned index;
	for (index = 1; index <= 2000; ++index) {
		wait_for(&h->ready, index);
		assert(!spf_hop_adaptive_policy_offer(h->policy, &h->observation));
		atomic_store_explicit(&h->done, index, memory_order_release);
	}
	return NULL;
}

static void threaded_feedback(unsigned rate)
{
	struct spf_hop_request_v2 r = request(rate, SPF_HOP_ADAPTIVE);
	struct handoff h;
	pthread_t thread;
	uint64_t now = UINT64_C(0xffffff00);
	unsigned index;
	memset(&h, 0, sizeof(h));
	atomic_init(&h.ready, 0); atomic_init(&h.done, 0);
	assert(!spf_hop_adaptive_policy_create(&h.policy, &r));
	assert(!pthread_create(&thread, NULL, producer, &h));
	for (index = 0; index < 2000; ++index) {
		struct spf_hop_choice_v2 c;
		h.observation = visit(h.policy, &r, index, &now, &c);
		if (index >= 48) assert(c.active_mask == 13 && c.quiet_mask == 242);
		h.observation.outcome = (13 & (1U << h.observation.target)) ?
			LEO_ADAPTIVE_DETECTED : LEO_ADAPTIVE_NOT_DETECTED;
		atomic_store_explicit(&h.ready, index + 1, memory_order_release);
		wait_for(&h.done, index + 1);
	}
	assert(!pthread_join(thread, NULL));
	spf_hop_adaptive_policy_destroy(h.policy);
}

static void host_feedback(void)
{
	for (unsigned rx=0;rx<2;++rx) {
		struct spf_hop_request_v2 r=request(10000000,SPF_HOP_ADAPTIVE);
		struct spf_hop_adaptive_policy *p;
		struct spf_hop_choice_v2 c;
		uint64_t now=UINT64_C(0xfffffff0);
		r.host.enabled=1; r.host.rx=rx; r.host.decision_rate_hz=2500000;
		r.host.factor=4; r.host.delay=80; r.host.supported_start=40; r.host.supported_end=300000;
		memset(r.host.configuration_sha256,0x17,32);
		assert(!spf_hop_adaptive_policy_create(&p,&r));
		for (unsigned index=0;index<80;++index) {
			leo_adaptive_observation_v1 o=visit(p,&r,index,&now,&c);
			struct spf_hop_host_feedback_v1 f={.session=o.session,.generation=o.generation,
				.stream_id=94,.visit=index,.event_sequence=index,.valid_start=o.valid_start,
				.valid_end=o.valid_end,.source_rate_hz=10000000,.decision_rate_hz=2500000,
				.rx=rx,.target=o.target,.outcome=o.target==0 ? 1 : 2,.healthy=1,
				.screen_mask=63,.confirmation_mask=1,.supported_start=40,.supported_end=300000,
				.factor=4,.delay=80}, bad;
			memcpy(f.configuration_sha256,r.host.configuration_sha256,32);
			assert(c.reason!=SPF_HOP_CHOICE_FAULT_FALLBACK);
			if (index>=48) assert(c.active_mask==1 && c.quiet_mask==254);
			if (!index) {
				bad=f; bad.rx=1-rx;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end)==-ESTALE);
				bad=f; ++bad.generation;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end)==-ESTALE);
				bad=f; ++bad.session;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end)==-ESTALE);
				bad=f; bad.configuration_sha256[0]^=1;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end)==-ESTALE);
				bad=f; ++bad.visit; ++bad.event_sequence;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end)==-ERANGE);
				bad=f; bad.target=(bad.target+1)%8;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end)==-EINVAL);
				bad=f; bad.valid_start+=4; bad.valid_end+=4;
				assert(spf_hop_adaptive_policy_offer_host(p,&bad,94,o.valid_end+4)==-EINVAL);
				assert(spf_hop_adaptive_policy_offer_host(p,&f,95,o.valid_end)==-ESTALE);
				assert(spf_hop_adaptive_policy_offer_host(p,&f,94,o.valid_end-1)==-ESTALE);
				assert(spf_hop_adaptive_policy_offer_host(p,&f,94,o.valid_end+10000001)==-ESTALE);
			}
			assert(!spf_hop_adaptive_policy_offer_host(p,&f,94,o.valid_end));
			assert(spf_hop_adaptive_policy_offer_host(p,&f,94,o.valid_end)==-EALREADY);
		}
		spf_hop_adaptive_policy_destroy(p);
	}
}

static void eligible_edges(uint32_t mask)
{
	struct spf_hop_request_v2 r=request(2500000,SPF_HOP_ADAPTIVE);
	struct spf_hop_adaptive_policy *p;
	struct spf_hop_choice_v2 c;
	uint64_t now=UINT64_C(0xfffffff0);
	unsigned saw[5]={0}, index;
	r.eligible_target_mask=mask;
	assert(!spf_hop_adaptive_policy_validate_pinned(&r));
	assert(!spf_hop_adaptive_policy_create(&p,&r));
	for (index=0;index<120;++index) {
		if (index==40) now+=r.geometry.sample_rate_hz*4;
		leo_adaptive_observation_v1 o=visit(p,&r,index,&now,&c);
		assert(mask & (1U<<c.proposed_target));
		assert(mask & (1U<<o.target));
		assert(!((c.active_mask|c.quiet_mask)&~mask));
		assert(c.reason<=SPF_HOP_CHOICE_FAULT_FALLBACK);
		saw[c.reason]=1;
		/* One active edge creates weighted service plus overdue exploration;
		 * every other eligible edge becomes quiet after three misses. */
		o.outcome=o.target==(mask==0x0f ? 0U : 4U) ?
			LEO_ADAPTIVE_DETECTED : LEO_ADAPTIVE_NOT_DETECTED;
		assert(!spf_hop_adaptive_policy_offer(p,&o));
	}
	assert(saw[SPF_HOP_CHOICE_WARMUP]);
	assert(saw[SPF_HOP_CHOICE_WEIGHTED]);
	assert(saw[SPF_HOP_CHOICE_EXPLORATION]);
	spf_hop_adaptive_policy_fault(p);
	{
		leo_adaptive_observation_v1 o=visit(p,&r,120,&now,&c);
		assert(c.reason==SPF_HOP_CHOICE_FAULT_FALLBACK);
		assert(mask&(1U<<o.target));
	}
	spf_hop_adaptive_policy_destroy(p);

	/* A no-detection session deterministically reaches the no-active path. */
	memset(saw,0,sizeof(saw));
	assert(!spf_hop_adaptive_policy_create(&p,&r));
	now=UINT64_C(0xfffffff0);
	for (index=0;index<40;++index) {
		leo_adaptive_observation_v1 o=visit(p,&r,index,&now,&c);
		assert(mask&(1U<<o.target));
		saw[c.reason]=1;
		o.outcome=LEO_ADAPTIVE_NOT_DETECTED;
		assert(!spf_hop_adaptive_policy_offer(p,&o));
	}
	assert(saw[SPF_HOP_CHOICE_NONE_ACTIVE]);
	spf_hop_adaptive_policy_destroy(p);
}

static void wide_host_policy_creation(void)
{
	const unsigned rates[] = {15000000, 20000000};

	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
		struct spf_hop_request_v2 r = request(rates[i], SPF_HOP_ADAPTIVE);
		struct spf_hop_adaptive_policy *p = NULL;

		r.host.enabled = 1;
		r.host.rx = 0;
		r.host.decision_rate_hz = 2500000;
		r.host.factor = rates[i] / r.host.decision_rate_hz;
		r.host.delay = rates[i] == 15000000 ? 100 : 128;
		r.host.supported_start = rates[i] == 15000000 ? 34 : 32;
		r.host.supported_end = 300000;
		memset(r.host.configuration_sha256, 0x17,
			sizeof(r.host.configuration_sha256));
		assert(!spf_hop_adaptive_policy_create(&p, &r));
		assert(p);
		spf_hop_adaptive_policy_destroy(p);
	}
}

static void host_feedback_can_advance_one_unavailable_visit(void)
{
	struct spf_hop_request_v2 r=request(15000000,SPF_HOP_ADAPTIVE);
	struct spf_hop_adaptive_policy *p;
	struct spf_hop_choice_v2 c;
	leo_adaptive_observation_v1 observations[3];
	uint64_t now=UINT64_C(0xfffffff0);
	struct spf_hop_host_feedback_v1 f={0};
	r.host.enabled=1; r.host.rx=0; r.host.decision_rate_hz=2500000;
	r.host.factor=6; r.host.delay=100; r.host.supported_start=34; r.host.supported_end=300000;
	memset(r.host.configuration_sha256,0x17,32);
	assert(!spf_hop_adaptive_policy_create(&p,&r));
	for (unsigned index=0;index<3;++index)
		observations[index]=visit(p,&r,index,&now,&c);
	f=(struct spf_hop_host_feedback_v1){.session=71,.generation=9,.stream_id=94,
		.visit=0,.event_sequence=0,.valid_start=observations[0].valid_start,
		.valid_end=observations[0].valid_end,.source_rate_hz=15000000,
		.decision_rate_hz=2500000,.rx=0,.target=observations[0].target,
		.outcome=LEO_ADAPTIVE_NOT_DETECTED,.healthy=1,.screen_mask=63,
		.supported_start=34,.supported_end=300000,.factor=6,.delay=100};
	memcpy(f.configuration_sha256,r.host.configuration_sha256,32);
	assert(!spf_hop_adaptive_policy_offer_host(p,&f,94,observations[0].valid_end));
	/* Visit 1 has no retained IQ. Visit 2 must advance it as unavailable. */
	f.visit=f.event_sequence=2; f.valid_start=observations[2].valid_start;
	f.valid_end=observations[2].valid_end; f.target=observations[2].target;
	assert(!spf_hop_adaptive_policy_offer_host(p,&f,94,observations[2].valid_end));
	assert(spf_hop_adaptive_policy_offer_host(p,&f,94,observations[2].valid_end)==-EALREADY);
	spf_hop_adaptive_policy_destroy(p);
}

int main(void)
{
	all_masks(2500000, SPF_HOP_ADAPTIVE);
	all_masks(5000000, SPF_HOP_ADAPTIVE);
	all_masks(2500000, SPF_HOP_SHADOW);
	all_masks(5000000, SPF_HOP_SHADOW);
	fault_cases();
	threaded_feedback(2500000);
	threaded_feedback(5000000);
	host_feedback();
	eligible_edges(0x0f);
	eligible_edges(0xf0);
	host_feedback_can_advance_one_unavailable_visit();
	wide_host_policy_creation();
	puts("native feedback/policy: 458752 mask decisions + 4000 threaded decisions + fault cases PASS; synthetic, no RF");
	return 0;
}
