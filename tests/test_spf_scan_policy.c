/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scan-policy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct spf_scan_policy_config config(void)
{
	struct spf_scan_policy_config c = {0};
	unsigned i;
	c.session = 1; c.generation = 2; c.seed = 123456789;
	c.source_rate_hz = 15000000; c.targets = 8;
	c.duration_ms = 300000; c.dwell_ms = 120; c.transition_budget_ms = 10;
	c.maximum_revisit_ms = 3000; c.feedback_age_ms = 1000;
	c.application_delay_ms = 1000; c.decay_ms = 3000; c.maximum_boost = 3;
	c.analysis_digest[0] = 77;
	for (i = 0; i < c.targets; i++) c.baseline[i] = 1;
	return c;
}
static uint64_t dt(const struct spf_scan_policy_config *c, unsigned ms)
{ return (uint64_t)c->source_rate_hz * ms / 1000; }

static struct spf_scan_feedback observation(const struct spf_scan_policy_config *c,
	const struct spf_scan_choice *choice, uint64_t start, uint64_t sequence, unsigned outcome)
{
	struct spf_scan_feedback f = {c->session, c->generation, sequence,
		choice->visit, start, start + dt(c, c->dwell_ms), choice->target, outcome, {0}};
	memcpy(f.analysis_digest, c->analysis_digest, 32);
	return f;
}

static void test_admission(void)
{
	struct spf_scan_policy_config c = config(), bad;
	struct spf_scan_policy *p = NULL;
	unsigned rates[] = {520833, 2500000, 5000000, 7500000, 8000000,
		12345679, 10000000, 15000000, 20000000, 30000000, 61440000}, i;
	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		c.source_rate_hz = rates[i];
		assert(!spf_scan_policy_validate(&c));
	}
	bad = c; bad.source_rate_hz = 61440001;
	assert(spf_scan_policy_validate(&bad) == -EOPNOTSUPP);
	bad = c; bad.source_rate_hz = 520832;
	assert(spf_scan_policy_validate(&bad) == -EOPNOTSUPP);
	bad = c; bad.maximum_revisit_ms = 1039;
	assert(spf_scan_policy_validate(&bad) == -ERANGE);
	bad = c; bad.application_delay_ms = 129;
	assert(spf_scan_policy_validate(&bad) == -ERANGE);
	bad = c; bad.baseline[7] = 0;
	assert(spf_scan_policy_validate(&bad) == -EINVAL);
	bad = c; bad.targets = 9;
	assert(spf_scan_policy_validate(&bad) == -EINVAL);
	bad = c; memset(bad.analysis_digest, 0, 32);
	assert(spf_scan_policy_validate(&bad) == -EINVAL);
	assert(spf_scan_policy_create(&p, &c, UINT64_MAX - 10) == -EOVERFLOW);
	assert(p == NULL);
}

static void test_feedback_and_terminal(void)
{
	struct spf_scan_policy_config c = config();
	struct spf_scan_policy *p = NULL;
	struct spf_scan_choice choice;
	struct spf_scan_feedback f, bad;
	struct spf_scan_ack ack;
	uint64_t epoch = (UINT64_C(1) << 54) + 12345;
	uint64_t start = epoch + dt(&c, 10), end = start + dt(&c, c.dwell_ms);
	assert(!spf_scan_policy_create(&p, &c, epoch));
	assert(!spf_scan_policy_select(p, epoch, &choice));
	assert(spf_scan_policy_select(p, epoch, &choice) == -EBUSY);
	assert(!spf_scan_policy_commit(p, start));
	f = observation(&c, &choice, start, 10, SPF_SCAN_ACTIVE);
	assert(spf_scan_policy_feedback(p, &f, end) == SPF_SCAN_REJECTED);
	assert(!spf_scan_policy_finish_visit(p, 0, true));
	bad = f; bad.session++;
	assert(spf_scan_policy_feedback(p, &bad, end) == SPF_SCAN_WRONG_SESSION);
	bad = f; bad.valid_start++;
	assert(spf_scan_policy_feedback(p, &bad, end) == SPF_SCAN_REJECTED);
	bad = f; bad.analysis_digest[0]++;
	assert(spf_scan_policy_feedback(p, &bad, end) == SPF_SCAN_REJECTED);
	assert(spf_scan_policy_feedback(p, &f, end) == SPF_SCAN_ACCEPTED);
	assert(spf_scan_policy_feedback(p, &f, end) == SPF_SCAN_DUPLICATE);
	bad = f; bad.sequence--;
	assert(spf_scan_policy_feedback(p, &bad, end) == SPF_SCAN_SUPERSEDED);
	assert(spf_scan_policy_take_ack(p, &ack) == -EAGAIN);
	assert(!spf_scan_policy_select(p, end, &choice));
	assert(!spf_scan_policy_take_ack(p, &ack));
	assert(ack.sequence == 10 && ack.source_visit == 0 && ack.first_visit == 1);
	assert(ack.result == SPF_SCAN_APPLIED && ack.application_counter == end);
	assert(ack.old_boost == SPF_SCAN_WEIGHT_ONE && ack.new_boost == 3 * SPF_SCAN_WEIGHT_ONE);
	assert(!spf_scan_policy_commit(p, end + dt(&c, 10)));
	assert(!spf_scan_policy_finish_visit(p, 1, true));
	f = observation(&c, &choice, end + dt(&c, 10), 12, SPF_SCAN_UNKNOWN);
	assert(spf_scan_policy_feedback(p, &f, f.valid_end) == SPF_SCAN_ACCEPTED);
	spf_scan_policy_stop(p, f.valid_end);
	assert(!spf_scan_policy_take_ack(p, &ack));
	assert(ack.result == SPF_SCAN_CANCELLED && ack.first_visit == UINT64_MAX);
	assert(spf_scan_policy_take_ack(p, &ack) == -EAGAIN);
	assert(spf_scan_policy_select(p, f.valid_end, &choice) == -ESHUTDOWN);
	spf_scan_policy_destroy(p);
}

static void test_commit_after_transition_budget_is_valid(void)
{
	struct spf_scan_policy_config c = config();
	struct spf_scan_policy *p = NULL;
	struct spf_scan_choice choice;
	uint64_t start = dt(&c, 25);

	assert(!spf_scan_policy_create(&p, &c, 0));
	assert(!spf_scan_policy_select(p, 0, &choice));
	assert(start > choice.selection_counter + dt(&c, c.transition_budget_ms));
	assert(!spf_scan_policy_commit(p, start));
	assert(!spf_scan_policy_finish_visit(p, choice.visit, true));
	spf_scan_policy_stop(p, start + dt(&c, c.dwell_ms));
	spf_scan_policy_destroy(p);
}

static void test_v3_active_base_and_quiet_probe_durations(void)
{
	struct spf_scan_policy_config c = config();
	struct spf_scan_policy *p = NULL;
	struct spf_scan_choice choice;
	struct spf_scan_feedback f;
	uint64_t now = 0, start;

	c.protocol_version = 3;
	c.source_rate_hz = 2500000;
	c.targets = 1;
	c.dwell_ms = 360;
	memset(c.baseline, 0, sizeof(c.baseline));
	c.baseline[0] = 1;
	assert(!spf_scan_policy_create(&p, &c, 0));
	assert(!spf_scan_policy_select(p, now, &choice));
	assert(choice.dwell_ms == 120);
	start = now + dt(&c, 10);
	assert(!spf_scan_policy_commit(p, start));
	assert(!spf_scan_policy_finish_visit(p, choice.visit, true));
	f = observation(&c, &choice, start, 1, SPF_SCAN_ACTIVE);
	f.valid_end = start + dt(&c, choice.dwell_ms);
	now = f.valid_end;
	assert(spf_scan_policy_feedback(p, &f, now) == SPF_SCAN_ACCEPTED);
	assert(!spf_scan_policy_select(p, now, &choice));
	assert(choice.dwell_ms == 360);
	start = now + dt(&c, 10);
	assert(!spf_scan_policy_commit(p, start));
	assert(!spf_scan_policy_finish_visit(p, choice.visit, true));
	f = observation(&c, &choice, start, 2, SPF_SCAN_QUIET);
	f.valid_end = start + dt(&c, choice.dwell_ms);
	now = f.valid_end;
	assert(spf_scan_policy_feedback(p, &f, now) == SPF_SCAN_ACCEPTED);
	assert(!spf_scan_policy_select(p, now, &choice));
	assert(choice.dwell_ms == 120);
	spf_scan_policy_destroy(p);
}

static void test_mailbox_reservation(void)
{
	struct spf_scan_policy_config c = config();
	struct spf_scan_policy *p = NULL;
	struct spf_scan_choice choice;
	struct spf_scan_feedback f;
	struct spf_scan_ack ack;
	unsigned i;
	assert(!spf_scan_policy_create(&p, &c, 0));
	assert(!spf_scan_policy_select(p, 0, &choice));
	assert(!spf_scan_policy_commit(p, dt(&c, 10)));
	assert(!spf_scan_policy_finish_visit(p, 0, true));
	f = observation(&c, &choice, dt(&c, 10), 1, SPF_SCAN_ACTIVE);
	for (i = 1; i <= SPF_SCAN_ACK_CAPACITY; i++) {
		f.sequence = i;
		assert(spf_scan_policy_feedback(p, &f, f.valid_end) == SPF_SCAN_ACCEPTED);
	}
	f.sequence++;
	assert(spf_scan_policy_feedback(p, &f, f.valid_end) == SPF_SCAN_MAILBOX_FULL);
	assert(!spf_scan_policy_select(p, f.valid_end, &choice));
	for (i = 1; i <= SPF_SCAN_ACK_CAPACITY; i++) {
		assert(!spf_scan_policy_take_ack(p, &ack));
		assert(ack.sequence == i);
		assert(ack.result == (i < SPF_SCAN_ACK_CAPACITY ? SPF_SCAN_SUPERSEDED : SPF_SCAN_APPLIED));
	}
	assert(spf_scan_policy_take_ack(p, &ack) == -EAGAIN);
	/* A refused sequence was not consumed and may be retried. */
	assert(spf_scan_policy_feedback(p, &f, f.valid_end) == SPF_SCAN_ACCEPTED);
	spf_scan_policy_stop(p, f.valid_end);
	assert(!spf_scan_policy_take_ack(p, &ack));
	assert(ack.sequence == f.sequence && ack.result == SPF_SCAN_CANCELLED);
	spf_scan_policy_destroy(p);
}

static void test_order_expiry_and_invalid_capture(void)
{
	struct spf_scan_policy_config c = config();
	struct spf_scan_policy *p = NULL;
	struct spf_scan_choice choice;
	struct spf_scan_feedback f;
	struct spf_scan_ack ack;
	uint64_t now = 0;
	unsigned i, first_target = UINT32_MAX;
	assert(!spf_scan_policy_create(&p, &c, 0));
	/* Out-of-order arrival on different channels is valid within the window. */
	for (i = 0; i < 8; i++) {
		assert(!spf_scan_policy_select(p, now, &choice));
		assert(!spf_scan_policy_commit(p, now + dt(&c, 10)));
		assert(!spf_scan_policy_finish_visit(p, i, true));
		f = observation(&c, &choice, now + dt(&c, 10), 100 - i, SPF_SCAN_ACTIVE);
		now = f.valid_end;
		if (!i) {
			first_target = choice.target;
			assert(spf_scan_policy_feedback(p, &f, now) == SPF_SCAN_ACCEPTED);
		} else if (first_target != choice.target) {
			assert(spf_scan_policy_feedback(p, &f, now) == SPF_SCAN_ACCEPTED);
			break;
		}
	}
	assert(i < 8);
	while (!spf_scan_policy_take_ack(p, &ack)) { }
	spf_scan_policy_stop(p, now);
	spf_scan_policy_destroy(p);
	assert(!spf_scan_policy_create(&p, &c, 0));
	assert(!spf_scan_policy_select(p, 0, &choice));
	assert(!spf_scan_policy_commit(p, dt(&c, 10)));
	assert(!spf_scan_policy_finish_visit(p, 0, false));
	f = observation(&c, &choice, dt(&c, 10), 1, SPF_SCAN_ACTIVE);
	assert(spf_scan_policy_feedback(p, &f, f.valid_end) == SPF_SCAN_REJECTED);
	assert(spf_scan_policy_finish_visit(p, 0, true) == -EALREADY);
	spf_scan_policy_destroy(p);
	assert(!spf_scan_policy_create(&p, &c, 0));
	assert(!spf_scan_policy_select(p, 0, &choice));
	assert(!spf_scan_policy_commit(p, dt(&c, 10)));
	assert(!spf_scan_policy_finish_visit(p, 0, true));
	f = observation(&c, &choice, dt(&c, 10), 1, SPF_SCAN_ACTIVE);
	assert(spf_scan_policy_feedback(p, &f, f.valid_end + dt(&c, 1001)) == SPF_SCAN_EXPIRED);
	assert(spf_scan_policy_feedback(p, &f, f.valid_end + dt(&c, 999)) == SPF_SCAN_ACCEPTED);
	assert(!spf_scan_policy_select(p, f.valid_end + dt(&c, 1001), &choice));
	assert(!spf_scan_policy_take_ack(p, &ack));
	assert(ack.result == SPF_SCAN_EXPIRED);
	spf_scan_policy_destroy(p);
}

static void test_weighted_replay_and_deadlines(void)
{
	struct spf_scan_policy_config c = config();
	struct spf_scan_policy *p, *replay;
	struct spf_scan_choice choice, same;
	struct spf_scan_feedback f;
	struct spf_scan_ack ack, same_ack;
	uint64_t now, last[8], seq;
	unsigned active, i, counts[8], total;
	int ret;
	c.dwell_ms = 20;
	/* All 256 activity masks, full 300 s, two identical independent instances. */
	for (active = 0; active < 256; active++) {
		now = UINT64_C(9007199254740997); seq = 0; total = 0;
		memset(counts, 0, sizeof(counts));
		for (i = 0; i < 8; i++) last[i] = now;
		assert(!spf_scan_policy_create(&p, &c, now));
		assert(!spf_scan_policy_create(&replay, &c, now));
		while (!(ret = spf_scan_policy_select(p, now, &choice))) {
			assert(!spf_scan_policy_select(replay, now, &same));
			assert(choice.visit == same.visit && choice.target == same.target);
			assert(choice.eligible_mask == same.eligible_mask && choice.effective_weight == same.effective_weight);
			now += dt(&c, 10);
			assert(now - last[choice.target] <= dt(&c, c.maximum_revisit_ms));
			last[choice.target] = now;
			assert(!spf_scan_policy_commit(p, now));
			assert(!spf_scan_policy_commit(replay, now));
			assert(!spf_scan_policy_finish_visit(p, choice.visit, true));
			assert(!spf_scan_policy_finish_visit(replay, choice.visit, true));
			f = observation(&c, &choice, now, ++seq,
				active & (1U << choice.target) ? SPF_SCAN_ACTIVE : SPF_SCAN_QUIET);
			now = f.valid_end;
			assert(spf_scan_policy_feedback(p, &f, now) == SPF_SCAN_ACCEPTED);
			assert(spf_scan_policy_feedback(replay, &f, now) == SPF_SCAN_ACCEPTED);
			while (!spf_scan_policy_take_ack(p, &ack)) {
				assert(!spf_scan_policy_take_ack(replay, &same_ack));
				assert(ack.sequence == same_ack.sequence && ack.new_boost == same_ack.new_boost);
				assert(ack.result == SPF_SCAN_APPLIED);
				assert(ack.application_counter - ack.received_counter <= dt(&c, c.application_delay_ms));
			}
			++counts[choice.target]; ++total;
		}
		assert(ret == -ENODATA && total == 10000);
		for (i = 0; i < 8; i++) assert(counts[i] > 100);
		if (active == 0 || active == 255)
			for (i = 0; i < 8; i++) assert(counts[i] > 1050 && counts[i] < 1450);
		if (active == 1) {
			assert(counts[0] > 2300 && counts[0] < 4000);
			for (i = 1; i < 8; i++) assert(counts[0] > 2 * counts[i]);
		}
		spf_scan_policy_stop(p, now); spf_scan_policy_stop(replay, now);
		spf_scan_policy_destroy(p); spf_scan_policy_destroy(replay);
	}
}

int main(void)
{
	test_admission();
	test_feedback_and_terminal();
	test_commit_after_transition_budget_is_valid();
	test_v3_active_base_and_quiet_probe_durations();
	test_mailbox_reservation();
	test_order_expiry_and_invalid_capture();
	test_weighted_replay_and_deadlines();
	puts("scan policy: admission, source binding, bounded receipts, 256 activity masks and replay PASS");
	return 0;
}
