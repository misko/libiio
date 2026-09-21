/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-scheduler.h"
#ifdef SPF_HOP_TEST_NATIVE_POLICY
#include "spf-hop-adaptive-policy.h"
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define VISITS 40

static struct spf_hop_request_v2 request(uint64_t rate, uint32_t mode)
{
	struct spf_hop_request_v2 r = {0};
	unsigned i;
	r.geometry.required_features = SPF_HOP_REQUIRED_FEATURES_V1;
	r.geometry.flags = SPF_HOP_REQUEST_FLAGS_V1;
	r.geometry.session_id = UINT64_C(0xfedcba9876543210);
	r.geometry.sample_rate_hz = r.geometry.rf_bandwidth_hz = rate;
	r.geometry.if_offset_hz = -5000;
	r.geometry.dwell_samples = rate * 120 / 1000;
	r.geometry.transition_guard_samples = rate / 1000;
	r.geometry.dwell_count = 64;
	r.geometry.capture_span_samples = VISITS * (r.geometry.dwell_samples + r.geometry.transition_guard_samples + 2);
	for (i = 0; i < 8; ++i) {
		r.geometry.profiles[i].profile_id = (uint8_t)i;
		r.geometry.profiles[i].fastlock_slot = (uint8_t)(7 - i);
		r.geometry.profiles[i].lo_frequency_hz = 1000000000 + i * 1000000;
		r.geometry.profiles[i].center_frequency_hz = r.geometry.profiles[i].lo_frequency_hz - 5000;
		r.geometry.profiles[i].profile_crc32 = 100 + i;
	}
	r.policy = (struct spf_hop_policy_v2){9, mode, 3, 3, 3, 1, 2000, 3000, 160, 1000, 3};
	return r;
}

static void test_request(void)
{
	struct spf_hop_request_v2 r = request(2500000, SPF_HOP_ADAPTIVE), decoded, sentinel;
	struct spf_hop_request_v1 legacy;
	uint8_t wire[SPF_HOP_ADAPTIVE_REQUEST_BYTES], again[sizeof(wire)], bad[sizeof(wire)];
	size_t n;
	assert(!spf_hop_request_v2_encode(wire, sizeof(wire), &r));
	assert(wire[4] == 2 && wire[6] == 96 && wire[7] == 1 && wire[76] == 144);
	assert(!spf_hop_request_v2_decode(&decoded, wire, sizeof(wire)));
	assert(!spf_hop_request_v2_encode(again, sizeof(again), &decoded));
	assert(!memcmp(wire, again, sizeof(wire)));
	assert(spf_hop_request_v1_decode(&legacy, wire, sizeof(wire)) < 0);
	assert(spf_hop_request_v1_decode(&legacy, wire, SPF_HOP_REQUEST_BYTES) < 0);
	memset(&sentinel, 0xa5, sizeof(sentinel));
	for (n = 0; n < sizeof(wire); ++n) {
		decoded = sentinel;
		assert(spf_hop_request_v2_decode(&decoded, wire, n) < 0);
		assert(!memcmp(&decoded, &sentinel, sizeof(decoded)));
	}
	for (n = 336; n < sizeof(wire); ++n) {
		memcpy(bad, wire, sizeof(bad)); bad[n] = 1; decoded = sentinel;
		assert(spf_hop_request_v2_decode(&decoded, bad, sizeof(bad)) == -EBADMSG);
		assert(!memcmp(&decoded, &sentinel, sizeof(decoded)));
	}
	r.policy.mode = 0;
	memset(again, 0xa5, sizeof(again)); memcpy(bad, again, sizeof(bad));
	assert(spf_hop_request_v2_encode(again, sizeof(again), &r) == -EINVAL);
	assert(!memcmp(again, bad, sizeof(again)));
}

static void test_mask_request(void)
{
	struct spf_hop_request_v2 r=request(2500000,SPF_HOP_ADAPTIVE),decoded,sentinel;
	uint8_t legacy[SPF_HOP_ADAPTIVE_REQUEST_BYTES];
	uint8_t wire[SPF_HOP_ADAPTIVE_REQUEST_BYTES],again[sizeof(wire)],bad[sizeof(wire)];
	memset(&sentinel,0xa5,sizeof(sentinel));
	assert(!spf_hop_request_v2_encode(legacy,sizeof(legacy),&r));
	r.eligible_target_mask=0x0f;
	assert(!spf_hop_request_mask_v2_encode(wire,sizeof(wire),&r));
	assert(wire[4]==2 && wire[8]==SPF_HOP_ADAPTIVE_MASK_FEATURES && wire[336]==0x0f);
	assert(spf_hop_request_v2_decode(&decoded,wire,sizeof(wire))<0);
	assert(!spf_hop_request_mask_v2_decode(&decoded,wire,sizeof(wire)));
	assert(decoded.eligible_target_mask==0x0f);
	assert(!spf_hop_request_mask_v2_encode(again,sizeof(again),&decoded));
	assert(!memcmp(wire,again,sizeof(wire)));
	for (size_t n=337;n<sizeof(wire);++n) {
		memcpy(bad,wire,sizeof(bad)); bad[n]=1; decoded=sentinel;
		assert(spf_hop_request_mask_v2_decode(&decoded,bad,sizeof(bad))==-EBADMSG);
		assert(!memcmp(&decoded,&sentinel,sizeof(decoded)));
	}
	assert(!spf_hop_request_v2_decode(&decoded,legacy,sizeof(legacy)));
	assert(decoded.eligible_target_mask==0xff);
	assert(!spf_hop_request_v2_encode(again,sizeof(again),&decoded));
	assert(!memcmp(legacy,again,sizeof(legacy)));
	memcpy(bad,wire,sizeof(bad)); bad[336]=0; decoded=sentinel;
	assert(spf_hop_request_mask_v2_decode(&decoded,bad,sizeof(bad))==-EBADMSG);
	assert(!memcmp(&decoded,&sentinel,sizeof(decoded)));
	r.eligible_target_mask=0;
	assert(spf_hop_request_mask_v2_encode(wire,sizeof(wire),&r)==-EINVAL);
	r.eligible_target_mask=0xf0;
	assert(!spf_hop_request_mask_v2_encode(wire,sizeof(wire),&r));
	r.policy.mode=SPF_HOP_SHADOW;
	assert(spf_hop_request_mask_v2_encode(wire,sizeof(wire),&r)==-EINVAL);
}

struct fake {
	struct spf_hop_request_v2 request;
	uint64_t counter, committed_end;
	unsigned starts, restores, recalls, commits;
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	struct spf_hop_adaptive_policy *policy;
	leo_adaptive_observation_v1 observation;
	unsigned offered;
#endif
};

static int start(void *p, uint64_t lo)
{
	struct fake *f = p;
	unsigned first=0;
	uint32_t mask=f->request.eligible_target_mask ? f->request.eligible_target_mask : 0xff;
	while (!(mask&(1U<<first))) ++first;
	assert(lo == f->request.geometry.profiles[first].lo_frequency_hz);
	++f->starts;
	return 0;
}
static int counter(void *p, uint64_t *out)
{
	struct fake *f = p;
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	/* Synthetic evidence becomes available only after the sampled dwell ends.
	 * Queue/thread contention is a separate test, not simulated here. */
	if (f->commits > f->offered && f->counter >= f->committed_end) {
		assert(!spf_hop_adaptive_policy_offer(f->policy, &f->observation));
		f->offered = f->commits;
	}
#endif
	*out = f->counter;
	return 0;
}
static int recall(void *p, uint32_t slot, uint64_t lo, struct spf_hop_scheduler_transition_v1 *t)
{
	struct fake *f = p;
	t->transition_before = f->counter;
	t->transition_after = f->counter += 2;
	t->actual_lo_frequency_hz = lo;
	t->active_profile = slot;
	t->device_event_id = ++f->recalls;
	return 0;
}
static int restore_io(void *p, uint64_t lo, struct spf_hop_scheduler_restore_v1 *r)
{
	struct fake *f = p;
	++f->restores;
	r->transition_before = f->counter;
	r->transition_after = ++f->counter;
	r->actual_lo_frequency_hz = lo;
	r->active_profile = UINT32_MAX;
	return 0;
}
static int sleep_io(void *p, uint64_t ns)
{
	struct fake *f = p;
	uint64_t samples = ns * f->request.geometry.sample_rate_hz / 1000000000;
	f->counter += samples ? samples : 1;
	return 0;
}
static int choose(void *p, uint64_t visit, uint64_t now, struct spf_hop_choice_v2 *c)
{
	struct fake *f = p;
#ifndef SPF_HOP_TEST_NATIVE_POLICY
	static const unsigned sequence[] = {0, 2, 3, 0, 2, 3, 0, 2, 3, 1};
#endif
	assert(visit == f->commits);
	if (visit) assert(now >= f->committed_end);
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	return spf_hop_adaptive_policy_ports()->choose(f->policy, visit, now, c);
#else
	uint32_t eligible = f->request.eligible_target_mask ?
		f->request.eligible_target_mask : UINT32_C(0xff);
	uint32_t proposed = visit < 24 ? visit % 8 : sequence[(visit - 24) % 10];
	memset(c, 0, sizeof(*c));
	c->decision_counter = now;
	c->basis_visit = visit ? visit - 1 : UINT64_MAX;
	c->generation = f->request.policy.generation;
	c->mode = f->request.policy.mode;
	while (!(eligible & (UINT32_C(1) << proposed)))
		proposed = (proposed + 1) % SPF_HOP_PROFILE_COUNT;
	c->proposed_target = proposed;
	c->reason = visit < 24 ? SPF_HOP_CHOICE_WARMUP : SPF_HOP_CHOICE_WEIGHTED;
	c->active_mask = 13 & eligible;
	c->quiet_mask = 242 & eligible;
	return 0;
#endif
}
static int commit(void *p, const struct spf_hop_device_event_v2 *e, uint64_t first, uint64_t end)
{
	struct fake *f = p;
	assert(e->device.dwell_index == f->commits++);
	assert(first == e->device.transition_after + f->request.geometry.transition_guard_samples);
	assert(end - first == f->request.geometry.dwell_samples);
	f->committed_end = end;
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	assert(!spf_hop_adaptive_policy_ports()->commit(f->policy, e, first, end));
	f->observation = (leo_adaptive_observation_v1){
		.session = f->request.geometry.session_id, .generation = f->request.policy.generation,
		.visit = e->device.dwell_index, .valid_start = first + UINT64_C(0x10000000000),
		.valid_end = end + UINT64_C(0x10000000000),
		.rate_hz = (uint32_t)f->request.geometry.sample_rate_hz, .rx = 1,
		.target = e->device.to_profile,
		.outcome = (13 & (1U << e->device.to_profile)) ? LEO_ADAPTIVE_DETECTED : LEO_ADAPTIVE_NOT_DETECTED,
		.healthy = 1,
	};
#endif
	return 0;
}
static const struct spf_hop_scheduler_io_v1 io = {start, counter, recall, restore_io, sleep_io, NULL};
static const struct spf_hop_scheduler_policy_v2 policy = {choose, commit};

struct replay {
	struct spf_hop_device_event_v2 events[VISITS];
	size_t cursor;
	uint64_t end;
	unsigned restores, starts;
	int corrupt;
};
static int replay_start(void *p, const struct spf_hop_request_v2 *r)
{ ++((struct replay *)p)->starts; return r->policy.generation == 9 ? 0 : -EINVAL; }
static int replay_drain(void *p, struct spf_hop_device_event_v2 *out, size_t capacity,
	size_t *count, uint64_t *dropped)
{
	struct replay *r = p;
	size_t n = VISITS - r->cursor;
	if (n > capacity) n = capacity;
	memcpy(out, r->events + r->cursor, n * sizeof(*out));
	if (r->corrupt && n && r->cursor == 24)
		out[0].device.from_profile=(out[0].device.from_profile+1)%SPF_HOP_PROFILE_COUNT;
	r->cursor += n;
	*count = n; *dropped = 0;
	return 0;
}
static int replay_restore(void *p, uint16_t reason, struct spf_hop_restore_receipt_v1 *out)
{
	struct replay *r = p;
	(void)reason;
	++r->restores;
	memset(out, 0, sizeof(*out));
	out->transition_before = r->end;
	out->transition_after = r->end + 2;
	out->restored_lo_frequency_hz = 1000000000;
	out->restored_profile = SPF_HOP_PROFILE_NONE;
	out->flags = SPF_HOP_EVENT_FLAGS_V1;
	return 0;
}
static const struct spf_hop_device_ops_v2 replay_ops = {replay_start, replay_drain, replay_restore};

static void test_session(struct replay *replay, const struct spf_hop_request_v2 *request, int corrupt)
{
	struct spf_hop_session_v2 s;
	struct spf_hop_sidecar_v2 sidecar, decoded, sentinel;
	struct spf_hop_sidecar_v1 legacy;
	struct spf_hop_status_v1 status, status_out;
	uint8_t wire[SPF_HOP_ADAPTIVE_SIDECAR_MAX_BYTES], again[sizeof(wire)];
	uint64_t first = UINT64_C(0x10000000000) + replay->events[0].device.transition_before;
	uint64_t span = request->geometry.capture_span_samples / 5;
	unsigned batch;
	int ret;
	replay->cursor = 0; replay->corrupt = corrupt; replay->restores = 0;
	replay->starts = 0;
	replay->end = first + request->geometry.capture_span_samples;
	assert(!spf_hop_session_v2_init(&s, request, &replay_ops, replay));
	assert(!spf_hop_session_v2_arm(&s));
	spf_hop_session_v2_get_status(&s, &status);
	assert(status.state == SPF_HOP_STATE_ARMED && replay->starts == 0);
	assert(!spf_hop_session_v2_start(&s));
	assert(replay->starts == 1);
	for (batch = 0; batch < 5; ++batch) {
		ret = spf_hop_session_v2_on_block(&s, batch, first + batch * span,
			first + (batch + 1) * span, &sidecar);
		if (corrupt && batch == 3) { assert(ret < 0 && replay->restores == 1); return; }
		assert(!ret && sidecar.geometry.event_count == 8);
		assert(sidecar.choices[0].decision_counter >= first);
		assert(sidecar.choices[0].decision_counter <= sidecar.geometry.events[0].device.transition_before);
		ret = spf_hop_sidecar_v2_encode(wire, sizeof(wire), &sidecar);
		assert(ret == SPF_HOP_ADAPTIVE_SIDECAR_MAX_BYTES);
		assert(!spf_hop_sidecar_v2_decode(&decoded, wire, ret));
		assert(spf_hop_sidecar_v2_encode(again, sizeof(again), &decoded) == ret);
		assert(!memcmp(wire, again, ret));
		assert(spf_hop_sidecar_v1_decode(&legacy, wire, ret) < 0);
		memset(&sentinel, 0xa5, sizeof(sentinel)); decoded = sentinel;
		wire[64 + 136] = 1;
		assert(spf_hop_sidecar_v2_decode(&decoded, wire, ret) == -EBADMSG);
		assert(!memcmp(&decoded, &sentinel, sizeof(decoded)));
	}
	spf_hop_session_v2_get_status(&s, &status);
	assert(status.state == SPF_HOP_STATE_COMPLETED && status.visits_started == VISITS);
	assert(status.final_counter == replay->end && replay->restores == 1);
	assert(!spf_hop_status_v2_encode(wire, sizeof(wire), &status));
	assert(!spf_hop_status_v2_decode(&status_out, wire, SPF_HOP_STATUS_BYTES));
	assert(status_out.final_counter == status.final_counter);
	assert(spf_hop_status_v1_decode(&status_out, wire, SPF_HOP_STATUS_BYTES) < 0);
	assert(!spf_hop_session_v2_cancel(&s, SPF_HOP_REASON_CLIENT_CLOSE));
	assert(replay->restores == 1);
}

static void test_scheduler(uint64_t rate, uint32_t mode, uint32_t eligible_mask)
{
	struct fake f = {0};
	struct replay replay = {0};
	const struct spf_hop_device_ops_v2 *ops;
	struct spf_hop_restore_receipt_v1 receipt;
	struct spf_hop_request_v2 wrong;
	void *context;
	size_t total = 0;
	unsigned attempt, i, different = 0;
	f.request = request(rate, mode);
	f.request.eligible_target_mask=eligible_mask;
	f.counter = UINT64_C(0xffffffff) - 50;
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	assert(!spf_hop_adaptive_policy_create(&f.policy, &f.request));
#endif
	assert(!spf_hop_scheduler_v2_create(&f.request, &io, &f, &policy, &f, &context, &ops));
	wrong = f.request; ++wrong.policy.cooldown_ms;
	assert(ops->submit_plan(context, &wrong) == -EINVAL);
	assert(!ops->submit_plan(context, &f.request));
	for (attempt = 0; attempt < 1000000 && total < VISITS; ++attempt) {
		size_t n = 0;
		uint64_t dropped = 0;
		assert(!ops->drain_events(context, replay.events + total, VISITS - total, &n, &dropped));
		assert(!dropped);
		total += n;
		if (total < VISITS) sched_yield();
	}
	assert(total == VISITS);
	/* Join before reading fake IO state: the producer owns it while running. */
	assert(!ops->cancel_restore(context, SPF_HOP_REASON_CLIENT_CLOSE, &receipt));
	assert(f.starts == 1 && f.recalls == VISITS && f.commits == VISITS && f.restores == 1);
	for (i = 0; i < VISITS; ++i) {
		const struct spf_hop_device_event_v2 *e = &replay.events[i];
		assert(e->device.from_profile == (i ? replay.events[i - 1].device.to_profile : SPF_HOP_PROFILE_NONE));
		assert(e->device.to_profile == (mode == SPF_HOP_SHADOW ? i % 8 : e->choice.proposed_target));
		assert((eligible_mask ? eligible_mask : 0xff)&(1U<<e->device.to_profile));
		assert(!((e->choice.active_mask|e->choice.quiet_mask)&
			~(eligible_mask ? eligible_mask : 0xff)));
		if (e->choice.proposed_target != i % 8) ++different;
	}
	assert(different > 0);
	spf_hop_scheduler_v2_destroy(context);
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	spf_hop_adaptive_policy_destroy(f.policy);
#endif
	test_session(&replay, &f.request, 0);
	test_session(&replay, &f.request, 1);
}

int main(void)
{
	test_request();
	test_mask_request();
	{
		struct spf_hop_request_v2 r=request(10000000,SPF_HOP_ADAPTIVE), decoded;
		uint8_t wire[SPF_HOP_HOST_REQUEST_BYTES], again[sizeof(wire)];
		r.host.enabled=1; r.host.rx=0; r.host.decision_rate_hz=2500000;
		r.host.factor=4; r.host.delay=80; r.host.supported_start=40; r.host.supported_end=300000;
		memset(r.host.configuration_sha256,0x91,32);
		assert(spf_hop_request_v2_encode(wire,sizeof(wire),&r)<0);
		assert(!spf_hop_request_v3_encode(wire,sizeof(wire),&r));
		assert(!spf_hop_request_v3_decode(&decoded,wire,sizeof(wire)));
		assert(!spf_hop_request_v3_encode(again,sizeof(again),&decoded));
		assert(!memcmp(wire,again,sizeof(wire)));
		assert(spf_hop_request_v2_decode(&decoded,wire,sizeof(wire))<0);
		for (size_t n=0;n<sizeof(wire);++n) assert(spf_hop_request_v3_decode(&decoded,wire,n)<0);
		wire[336]=1;
		assert(spf_hop_request_v3_decode(&decoded,wire,sizeof(wire))<0);
		r.host.rx=1;
		assert(!spf_hop_request_v3_encode(wire,sizeof(wire),&r));
		r.host.rx=2;
		assert(spf_hop_request_v3_encode(wire,sizeof(wire),&r)<0);
		struct spf_hop_host_feedback_v1 f={.session=1,.generation=2,.stream_id=3,
			.valid_start=UINT64_C(0xffffffff),.valid_end=UINT64_C(0xffffffff)+1200000,
			.source_rate_hz=10000000,.decision_rate_hz=2500000,.rx=1,.outcome=1,.healthy=1,
			.screen_mask=63,.confirmation_mask=32,.supported_start=40,.supported_end=300000,
			.factor=4,.delay=80}, got;
		memset(f.configuration_sha256,0x92,32);
		assert(!spf_hop_host_feedback_v1_encode(wire,sizeof(wire),&f));
		assert(!spf_hop_host_feedback_v1_decode(&got,wire,SPF_HOP_HOST_FEEDBACK_BYTES));
		assert(got.valid_end==f.valid_end && got.rx==1 && got.outcome==1);
		for (size_t n=0;n<SPF_HOP_HOST_FEEDBACK_BYTES;++n)
			assert(spf_hop_host_feedback_v1_decode(&got,wire,n)<0);
		wire[152]=1;
		assert(spf_hop_host_feedback_v1_decode(&got,wire,SPF_HOP_HOST_FEEDBACK_BYTES)<0);
		f.healthy=0;
		assert(spf_hop_host_feedback_v1_encode(wire,sizeof(wire),&f)<0);
		f.outcome=0; f.screen_mask=0; f.confirmation_mask=0;
		assert(!spf_hop_host_feedback_v1_encode(wire,sizeof(wire),&f));
	}
	{
		const uint32_t rates[]={15000000,20000000};
		const uint32_t factors[]={6,8}, delays[]={100,128}, starts[]={34,32};
		for (size_t i=0;i<2;++i) {
			struct spf_hop_request_v2 r=request(rates[i],SPF_HOP_ADAPTIVE), decoded;
			uint8_t wire[SPF_HOP_HOST_REQUEST_BYTES], again[sizeof(wire)];
			struct spf_hop_host_feedback_v1 f={.session=1,.generation=2,.stream_id=3,
				.valid_start=UINT64_C(0xffffffff),
				.valid_end=UINT64_C(0xffffffff)+rates[i]*120/1000,
				.source_rate_hz=rates[i],.decision_rate_hz=2500000,
				.outcome=1,.healthy=1,.screen_mask=63,.confirmation_mask=32,
				.supported_start=starts[i],.supported_end=300000,
				.factor=factors[i],.delay=delays[i]}, got;
			r.host.enabled=1; r.host.decision_rate_hz=2500000;
			r.host.factor=factors[i]; r.host.delay=delays[i];
			r.host.supported_start=starts[i]; r.host.supported_end=300000;
			memset(r.host.configuration_sha256,0x93,32);
			assert(!spf_hop_request_v4_encode(wire,sizeof(wire),&r));
			assert(!spf_hop_request_v4_decode(&decoded,wire,sizeof(wire)));
			assert(!spf_hop_request_v4_encode(again,sizeof(again),&decoded));
			assert(!memcmp(wire,again,sizeof(wire)));
			assert(spf_hop_request_v3_decode(&decoded,wire,sizeof(wire))<0);
			memset(f.configuration_sha256,0x94,32);
			assert(!spf_hop_host_feedback_v2_encode(wire,sizeof(wire),&f));
			assert(!spf_hop_host_feedback_v2_decode(&got,wire,SPF_HOP_HOST_FEEDBACK_BYTES));
			assert(got.source_rate_hz==rates[i] && got.valid_end==f.valid_end);
			assert(spf_hop_host_feedback_v1_decode(&got,wire,SPF_HOP_HOST_FEEDBACK_BYTES)<0);
		}
	}
	test_scheduler(2500000, SPF_HOP_ADAPTIVE,0);
	test_scheduler(5000000, SPF_HOP_ADAPTIVE,0);
	test_scheduler(2500000, SPF_HOP_SHADOW,0);
	test_scheduler(5000000, SPF_HOP_SHADOW,0);
	test_scheduler(2500000, SPF_HOP_ADAPTIVE,0x0f);
	test_scheduler(2500000, SPF_HOP_ADAPTIVE,0xf0);
#ifdef SPF_HOP_TEST_NATIVE_POLICY
	puts("native policy + feedback + scheduler + session: both rates/modes PASS (synthetic evidence, no RF)");
#else
	puts("adaptive request/session/scheduler: both rates and modes PASS (synthetic counters, no RF)");
#endif
	return 0;
}
