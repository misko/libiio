/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scan-protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define MAGIC_CAPS UINT32_C(0x50435053)
#define MAGIC_SETUP UINT32_C(0x51535053)
#define MAGIC_VISIT UINT32_C(0x52565053)
#define MAGIC_FEEDBACK UINT32_C(0x42465053)
#define MAGIC_ACK UINT32_C(0x41465053)
#define MAGIC_TERMINAL UINT32_C(0x54465053)

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t *p, uint64_t value)
{
	put32(p, (uint32_t)value);
	put32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t crc32(const uint8_t *p, size_t bytes)
{
	uint32_t crc = ~UINT32_C(0);
	size_t i;

	for (i = 0; i < bytes; i++) {
		unsigned bit;

		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
				(UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
	}
	return ~crc;
}

static void header(uint8_t *p, uint32_t magic, uint16_t bytes, uint32_t flags)
{
	put32(p, magic);
	put16(p + 4, SPF_SCAN_PROTOCOL_VERSION);
	put16(p + 6, bytes);
	put32(p + 8, SPF_SCAN_PROTOCOL_FEATURES);
	put32(p + 12, flags);
}

static int check(const uint8_t *p, size_t actual, size_t expected,
			 uint32_t magic, uint32_t flags)
{
	if (!p)
		return -EINVAL;
	if (actual != expected)
		return -EMSGSIZE;
	if (get32(p) != magic || (uint16_t)(p[4] | p[5] << 8) !=
	    SPF_SCAN_PROTOCOL_VERSION || (uint16_t)(p[6] | p[7] << 8) != expected)
		return -EPROTONOSUPPORT;
	if (get32(p + 8) != SPF_SCAN_PROTOCOL_FEATURES || get32(p + 12) != flags ||
	    get32(p + expected - 4) != crc32(p, expected - 4))
		return -EBADMSG;
	return 0;
}

static bool all_zero(const uint8_t *p, size_t bytes)
{
	size_t i;

	for (i = 0; i < bytes; i++)
		if (p[i])
			return false;
	return true;
}

static bool digest_present(const uint8_t digest[32])
{
	return !all_zero(digest, 32);
}

int spf_scan_time_query_encode(void *wire, size_t bytes,
		const struct spf_scan_time_query *q)
{
	uint8_t *p = wire;
	if (!p || !q || !q->request || !q->session || !q->generation)
		return -EINVAL;
	if (bytes < SPF_SCAN_TIME_QUERY_BYTES) return -ENOSPC;
	memset(p, 0, SPF_SCAN_TIME_QUERY_BYTES);
	header(p, UINT32_C(0x51545053), SPF_SCAN_TIME_QUERY_BYTES, 1);
	put64(p + 16, q->request); put64(p + 24, q->session);
	put64(p + 32, q->generation);
	put32(p + 44, crc32(p, 44));
	return 0;
}

int spf_scan_time_query_decode(struct spf_scan_time_query *q,
		const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	int ret = check(p, bytes, SPF_SCAN_TIME_QUERY_BYTES, UINT32_C(0x51545053), 1);
	if (ret) return ret;
	if (!q || get32(p + 40) || !get64(p + 16) || !get64(p + 24) || !get64(p + 32))
		return -EINVAL;
	*q = (struct spf_scan_time_query){get64(p + 16), get64(p + 24), get64(p + 32)};
	return 0;
}

int spf_scan_time_encode(void *wire, size_t bytes, const struct spf_scan_time *t)
{
	uint8_t *p = wire;
	if (!p || !t || !t->identity.request || !t->identity.session ||
	    !t->identity.generation || all_zero(t->boot_id, 16) || !t->epoch ||
	    !t->sample_rate_hz || t->monotonic_after_ns < t->monotonic_before_ns)
		return -EINVAL;
	if (bytes < SPF_SCAN_TIME_BYTES) return -ENOSPC;
	memset(p, 0, SPF_SCAN_TIME_BYTES);
	header(p, UINT32_C(0x41545053), SPF_SCAN_TIME_BYTES, 1);
	put64(p + 16, t->identity.request); put64(p + 24, t->identity.session);
	put64(p + 32, t->identity.generation); memcpy(p + 40, t->boot_id, 16);
	put64(p + 56, t->epoch); put64(p + 64, t->counter);
	put64(p + 72, t->monotonic_before_ns); put64(p + 80, t->monotonic_after_ns);
	put32(p + 88, t->sample_rate_hz); put32(p + 92, 64);
	put64(p + 96, t->maximum_snapshot_age_ns);
	put32(p + 124, crc32(p, 124));
	return 0;
}

int spf_scan_time_decode(struct spf_scan_time *t, const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	uint8_t check_wire[SPF_SCAN_TIME_BYTES];
	int ret = check(p, bytes, SPF_SCAN_TIME_BYTES, UINT32_C(0x41545053), 1);
	if (ret) return ret;
	if (!t || get32(p + 92) != 64 || !all_zero(p + 104, 20)) return -EINVAL;
	memset(t, 0, sizeof(*t));
	t->identity = (struct spf_scan_time_query){get64(p + 16), get64(p + 24), get64(p + 32)};
	memcpy(t->boot_id, p + 40, 16); t->epoch = get64(p + 56);
	t->counter = get64(p + 64); t->monotonic_before_ns = get64(p + 72);
	t->monotonic_after_ns = get64(p + 80); t->sample_rate_hz = get32(p + 88);
	t->maximum_snapshot_age_ns = get64(p + 96);
	return spf_scan_time_encode(check_wire, sizeof(check_wire), t);
}

static uint32_t rate_flag(uint32_t rate)
{
	switch (rate) {
	case 2500000: return SPF_SCAN_RATE_2P5M;
	case 10000000: return SPF_SCAN_RATE_10M;
	case 15000000: return SPF_SCAN_RATE_15M;
	case 20000000: return SPF_SCAN_RATE_20M;
	case 30000000: return SPF_SCAN_RATE_30M;
	default: return 0;
	}
}

void spf_scan_caps_default(struct spf_scan_caps *caps)
{
	if (!caps)
		return;
	*caps = (struct spf_scan_caps) {
		.rate_mask = SPF_SCAN_RATE_MASK_FIXED,
		.rx_mask = SPF_SCAN_RX1_RX2,
		.formats = SPF_SCAN_FORMAT_CI16,
		.maximum_targets = SPF_SCAN_TARGETS,
		.maximum_fastlock_profiles = SPF_SCAN_TARGETS,
		.minimum_dwell_ms = 20,
		.maximum_dwell_ms = 240,
		.maximum_duration_ms = 300000,
		.maximum_queue_bytes = UINT64_C(200000000),
		.maximum_queue_age_ms = 10000,
		.feedback_capacity = SPF_SCAN_ACK_CAPACITY,
		.maximum_feedback_age_ms = 10000,
		.maximum_application_delay_ms = 10000,
		.maximum_analog_bandwidth_hz = 56000000,
		.source_counter_bits = 64,
	};
}

static int caps_validate(const struct spf_scan_caps *caps)
{
	if (!caps || caps->rate_mask != SPF_SCAN_RATE_MASK_FIXED ||
	    caps->rx_mask != SPF_SCAN_RX1_RX2 || caps->formats != SPF_SCAN_FORMAT_CI16 ||
	    caps->maximum_targets != SPF_SCAN_TARGETS ||
	    caps->maximum_fastlock_profiles != SPF_SCAN_TARGETS ||
	    caps->minimum_dwell_ms != 20 || caps->maximum_dwell_ms != 240 ||
	    caps->maximum_duration_ms != 300000 ||
	    caps->maximum_queue_bytes != UINT64_C(200000000) ||
	    caps->maximum_queue_age_ms != 10000 ||
	    caps->feedback_capacity != SPF_SCAN_ACK_CAPACITY ||
	    caps->maximum_feedback_age_ms != 10000 ||
	    caps->maximum_application_delay_ms != 10000 ||
	    caps->maximum_analog_bandwidth_hz != 56000000 ||
	    caps->source_counter_bits != 64)
		return -EINVAL;
	return 0;
}

int spf_scan_setup_policy(const struct spf_scan_setup *setup,
			  struct spf_scan_policy_config *policy)
{
	unsigned i;

	if (!setup || !policy)
		return -EINVAL;
	memset(policy, 0, sizeof(*policy));
	policy->session = setup->session;
	policy->generation = setup->generation;
	policy->seed = setup->seed;
	policy->source_rate_hz = setup->source_rate_hz;
	policy->targets = setup->target_count;
	policy->duration_ms = setup->duration_ms;
	policy->dwell_ms = setup->dwell_ms;
	policy->transition_budget_ms = setup->transition_budget_ms;
	policy->maximum_revisit_ms = setup->maximum_revisit_ms;
	policy->feedback_age_ms = setup->feedback_age_ms;
	policy->application_delay_ms = setup->application_delay_ms;
	policy->decay_ms = setup->decay_ms;
	policy->maximum_boost = setup->maximum_boost;
	memcpy(policy->analysis_digest, setup->analysis_digest,
	       sizeof(policy->analysis_digest));
	for (i = 0; i < setup->target_count; i++)
		policy->baseline[i] = setup->targets[i].baseline_weight;
	return 0;
}

int spf_scan_setup_validate(const struct spf_scan_setup *setup)
{
	struct spf_scan_policy_config policy;
	uint32_t profiles = 0;
	unsigned i, j;

	if (!setup || !rate_flag(setup->source_rate_hz) ||
	    setup->analog_bandwidth_hz < 200000 ||
	    setup->analog_bandwidth_hz > setup->source_rate_hz ||
	    !setup->maximum_queue_bytes ||
	    setup->maximum_queue_bytes > UINT64_C(200000000) ||
	    !setup->maximum_queue_age_ms || setup->maximum_queue_age_ms > 10000 ||
	    !setup->maximum_queue_visits || setup->maximum_queue_visits > 64 ||
	    !setup->target_count || setup->target_count > SPF_SCAN_TARGETS ||
	    (setup->rx_mask != SPF_SCAN_RX1 &&
	     setup->rx_mask != SPF_SCAN_RX1_RX2) ||
	    setup->format != SPF_SCAN_FORMAT_CI16 ||
	    setup->flags != SPF_SCAN_SETUP_FLAGS ||
	    !digest_present(setup->analysis_digest))
		return -EINVAL;
	for (i = 0; i < setup->target_count; i++) {
		const struct spf_scan_target *target = &setup->targets[i];

		if (target->profile >= SPF_SCAN_TARGETS ||
		    profiles & (1U << target->profile) ||
		    target->frequency_hz < UINT64_C(70000000) ||
		    target->frequency_hz > UINT64_C(6000000000) ||
		    !target->baseline_weight || target->baseline_weight > 1024)
			return -EINVAL;
		profiles |= 1U << target->profile;
		for (j = 0; j < i; j++)
			if (setup->targets[j].channel == target->channel)
				return -EINVAL;
	}
	for (; i < SPF_SCAN_TARGETS; i++) {
		const struct spf_scan_target zero = { 0 };

		if (memcmp(&setup->targets[i], &zero, sizeof(zero)))
			return -EINVAL;
	}
	spf_scan_setup_policy(setup, &policy);
	return spf_scan_policy_validate(&policy);
}

int spf_scan_caps_encode(void *wire, size_t bytes,
			 const struct spf_scan_caps *caps)
{
	uint8_t p[SPF_SCAN_CAPS_BYTES] = { 0 };

	if (!wire || caps_validate(caps))
		return -EINVAL;
	if (bytes < sizeof(p))
		return -ENOSPC;
	header(p, MAGIC_CAPS, sizeof(p), 0);
	put32(p + 16, caps->rate_mask);
	put32(p + 20, caps->rx_mask);
	put32(p + 24, caps->formats);
	put32(p + 28, caps->maximum_targets);
	put32(p + 32, caps->maximum_fastlock_profiles);
	put32(p + 36, caps->minimum_dwell_ms);
	put32(p + 40, caps->maximum_dwell_ms);
	put32(p + 44, caps->maximum_duration_ms);
	put64(p + 48, caps->maximum_queue_bytes);
	put32(p + 56, caps->maximum_queue_age_ms);
	put32(p + 60, caps->feedback_capacity);
	put32(p + 64, caps->maximum_feedback_age_ms);
	put32(p + 68, caps->maximum_application_delay_ms);
	put32(p + 72, caps->maximum_analog_bandwidth_hz);
	put32(p + 76, caps->source_counter_bits);
	put32(p + 92, crc32(p, 92));
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_scan_caps_decode(struct spf_scan_caps *caps,
			 const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	struct spf_scan_caps out;
	int ret = check(p, bytes, SPF_SCAN_CAPS_BYTES, MAGIC_CAPS, 0);

	if (!caps)
		return -EINVAL;
	if (ret)
		return ret;
	if (!all_zero(p + 80, 12))
		return -EBADMSG;
	out = (struct spf_scan_caps) {
		.rate_mask = get32(p + 16), .rx_mask = get32(p + 20),
		.formats = get32(p + 24), .maximum_targets = get32(p + 28),
		.maximum_fastlock_profiles = get32(p + 32),
		.minimum_dwell_ms = get32(p + 36), .maximum_dwell_ms = get32(p + 40),
		.maximum_duration_ms = get32(p + 44), .maximum_queue_bytes = get64(p + 48),
		.maximum_queue_age_ms = get32(p + 56), .feedback_capacity = get32(p + 60),
		.maximum_feedback_age_ms = get32(p + 64),
		.maximum_application_delay_ms = get32(p + 68),
		.maximum_analog_bandwidth_hz = get32(p + 72),
		.source_counter_bits = get32(p + 76),
	};
	if (caps_validate(&out))
		return -EBADMSG;
	*caps = out;
	return 0;
}

int spf_scan_setup_encode(void *wire, size_t bytes,
			  const struct spf_scan_setup *setup)
{
	uint8_t p[SPF_SCAN_SETUP_BYTES] = { 0 };
	unsigned i;

	if (!wire || spf_scan_setup_validate(setup))
		return -EINVAL;
	if (bytes < sizeof(p))
		return -ENOSPC;
	header(p, MAGIC_SETUP, sizeof(p), SPF_SCAN_SETUP_FLAGS);
	put64(p + 16, setup->session); put64(p + 24, setup->generation);
	put64(p + 32, setup->seed); put32(p + 40, setup->source_rate_hz);
	put32(p + 44, setup->analog_bandwidth_hz); put32(p + 48, setup->duration_ms);
	put32(p + 52, setup->dwell_ms); put32(p + 56, setup->transition_budget_ms);
	put32(p + 60, setup->maximum_revisit_ms); put32(p + 64, setup->feedback_age_ms);
	put32(p + 68, setup->application_delay_ms); put32(p + 72, setup->decay_ms);
	put32(p + 76, setup->maximum_boost); put64(p + 80, setup->maximum_queue_bytes);
	put32(p + 88, setup->maximum_queue_age_ms);
	put32(p + 92, setup->maximum_queue_visits); put32(p + 96, setup->target_count);
	put32(p + 100, setup->rx_mask); put32(p + 104, setup->format);
	memcpy(p + 112, setup->analysis_digest, 32);
	for (i = 0; i < SPF_SCAN_TARGETS; i++) {
		uint8_t *target = p + 144 + i * 24;

		put32(target, setup->targets[i].channel);
		put32(target + 4, setup->targets[i].profile);
		put64(target + 8, setup->targets[i].frequency_hz);
		put32(target + 16, setup->targets[i].baseline_weight);
		put32(target + 20, setup->targets[i].profile_crc32);
	}
	put32(p + 348, crc32(p, 348));
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_scan_setup_decode(struct spf_scan_setup *setup,
			  const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	struct spf_scan_setup out = { 0 };
	unsigned i;
	int ret = check(p, bytes, SPF_SCAN_SETUP_BYTES, MAGIC_SETUP,
			SPF_SCAN_SETUP_FLAGS);

	if (!setup)
		return -EINVAL;
	if (ret)
		return ret;
	if (get32(p + 108) || !all_zero(p + 336, 12))
		return -EBADMSG;
	out.session = get64(p + 16); out.generation = get64(p + 24);
	out.seed = get64(p + 32); out.source_rate_hz = get32(p + 40);
	out.analog_bandwidth_hz = get32(p + 44); out.duration_ms = get32(p + 48);
	out.dwell_ms = get32(p + 52); out.transition_budget_ms = get32(p + 56);
	out.maximum_revisit_ms = get32(p + 60); out.feedback_age_ms = get32(p + 64);
	out.application_delay_ms = get32(p + 68); out.decay_ms = get32(p + 72);
	out.maximum_boost = get32(p + 76); out.maximum_queue_bytes = get64(p + 80);
	out.maximum_queue_age_ms = get32(p + 88); out.maximum_queue_visits = get32(p + 92);
	out.target_count = get32(p + 96); out.rx_mask = get32(p + 100);
	out.format = get32(p + 104); out.flags = get32(p + 12);
	memcpy(out.analysis_digest, p + 112, 32);
	for (i = 0; i < SPF_SCAN_TARGETS; i++) {
		const uint8_t *target = p + 144 + i * 24;

		out.targets[i].channel = get32(target);
		out.targets[i].profile = get32(target + 4);
		out.targets[i].frequency_hz = get64(target + 8);
		out.targets[i].baseline_weight = get32(target + 16);
		out.targets[i].profile_crc32 = get32(target + 20);
	}
	if (spf_scan_setup_validate(&out))
		return -EBADMSG;
	*setup = out;
	return 0;
}

static int visit_validate(const struct spf_scan_visit_record *visit)
{
	uint64_t samples;

	if (!visit || !visit->session || !visit->generation ||
	    visit->transition_before > visit->transition_after ||
	    visit->transition_after > visit->valid_start ||
	    visit->valid_start > visit->valid_end ||
	    visit->frequency_hz < UINT64_C(70000000) ||
	    visit->frequency_hz > UINT64_C(6000000000) ||
	    !rate_flag(visit->source_rate_hz) ||
	    visit->analog_bandwidth_hz < 200000 ||
	    visit->analog_bandwidth_hz > 56000000 ||
	    visit->target >= SPF_SCAN_TARGETS || visit->profile >= SPF_SCAN_TARGETS ||
	    visit->result > SPF_VISIT_CANCELLED ||
	    visit->eligible_mask & ~UINT32_C(0xff) ||
	    !(visit->flags & SPF_SCAN_VISIT_FLAGS) ||
	    visit->flags & ~SPF_SCAN_VISIT_FLAG_MASK)
		return -EINVAL;
	samples = visit->valid_end - visit->valid_start;
	if (samples > UINT64_MAX / 8)
		return -EINVAL;
	if ((visit->result == SPF_VISIT_COMPLETE) != !!visit->iq_bytes ||
	    (visit->result == SPF_VISIT_COMPLETE &&
	     visit->iq_bytes != samples * 4 && visit->iq_bytes != samples * 8))
		return -EINVAL;
	return 0;
}

int spf_scan_visit_encode(void *wire, size_t bytes,
			  const struct spf_scan_visit_record *visit)
{
	uint8_t p[SPF_SCAN_VISIT_BYTES] = { 0 };

	if (!wire || visit_validate(visit))
		return -EINVAL;
	if (bytes < sizeof(p))
		return -ENOSPC;
	header(p, MAGIC_VISIT, sizeof(p), visit->flags);
	put64(p + 16, visit->session); put64(p + 24, visit->generation);
	put64(p + 32, visit->visit); put64(p + 40, visit->selection_counter);
	put64(p + 48, visit->transition_before); put64(p + 56, visit->transition_after);
	put64(p + 64, visit->valid_start); put64(p + 72, visit->valid_end);
	put64(p + 80, visit->frequency_hz); put64(p + 88, visit->iq_bytes);
	put64(p + 96, visit->missing_samples_before);
	put32(p + 104, visit->analog_bandwidth_hz); put32(p + 108, visit->source_rate_hz);
	put32(p + 112, visit->target); put32(p + 116, visit->profile);
	put32(p + 120, visit->result); put32(p + 124, visit->eligible_mask);
	put32(p + 128, visit->effective_weight); put32(p + 132, visit->profile_crc32);
	put32(p + 136, visit->flags); put32(p + 156, crc32(p, 156));
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_scan_visit_decode(struct spf_scan_visit_record *visit,
			  const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	struct spf_scan_visit_record out = { 0 };
	int ret;

	if (!visit)
		return -EINVAL;
	if (!p || bytes != SPF_SCAN_VISIT_BYTES)
		return !p ? -EINVAL : -EMSGSIZE;
	ret = check(p, bytes, SPF_SCAN_VISIT_BYTES, MAGIC_VISIT, get32(p + 12));
	if (ret)
		return ret;
	if (!all_zero(p + 140, 16))
		return -EBADMSG;
	out.session = get64(p + 16); out.generation = get64(p + 24);
	out.visit = get64(p + 32); out.selection_counter = get64(p + 40);
	out.transition_before = get64(p + 48); out.transition_after = get64(p + 56);
	out.valid_start = get64(p + 64); out.valid_end = get64(p + 72);
	out.frequency_hz = get64(p + 80); out.iq_bytes = get64(p + 88);
	out.missing_samples_before = get64(p + 96);
	out.analog_bandwidth_hz = get32(p + 104); out.source_rate_hz = get32(p + 108);
	out.target = get32(p + 112); out.profile = get32(p + 116);
	out.result = get32(p + 120); out.eligible_mask = get32(p + 124);
	out.effective_weight = get32(p + 128); out.profile_crc32 = get32(p + 132);
	out.flags = get32(p + 136);
	if (out.flags != get32(p + 12) || visit_validate(&out))
		return -EBADMSG;
	*visit = out;
	return 0;
}

static int feedback_validate(const struct spf_scan_feedback *feedback)
{
	if (!feedback || !feedback->session || !feedback->generation ||
	    !feedback->sequence || feedback->valid_start >= feedback->valid_end ||
	    feedback->target >= SPF_SCAN_TARGETS ||
	    feedback->outcome > SPF_SCAN_QUIET ||
	    !digest_present(feedback->analysis_digest))
		return -EINVAL;
	return 0;
}

int spf_scan_feedback_encode(void *wire, size_t bytes,
			     const struct spf_scan_feedback *feedback)
{
	uint8_t p[SPF_SCAN_FEEDBACK_BYTES] = { 0 };

	if (!wire || feedback_validate(feedback))
		return -EINVAL;
	if (bytes < sizeof(p))
		return -ENOSPC;
	header(p, MAGIC_FEEDBACK, sizeof(p), 0);
	put64(p + 16, feedback->session); put64(p + 24, feedback->generation);
	put64(p + 32, feedback->sequence); put64(p + 40, feedback->visit);
	put64(p + 48, feedback->valid_start); put64(p + 56, feedback->valid_end);
	put32(p + 64, feedback->target); put32(p + 68, feedback->outcome);
	memcpy(p + 72, feedback->analysis_digest, 32);
	put32(p + 108, crc32(p, 108));
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_scan_feedback_decode(struct spf_scan_feedback *feedback,
			     const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	struct spf_scan_feedback out = { 0 };
	int ret = check(p, bytes, SPF_SCAN_FEEDBACK_BYTES, MAGIC_FEEDBACK, 0);

	if (!feedback)
		return -EINVAL;
	if (ret)
		return ret;
	if (get32(p + 104))
		return -EBADMSG;
	out.session = get64(p + 16); out.generation = get64(p + 24);
	out.sequence = get64(p + 32); out.visit = get64(p + 40);
	out.valid_start = get64(p + 48); out.valid_end = get64(p + 56);
	out.target = get32(p + 64); out.outcome = get32(p + 68);
	memcpy(out.analysis_digest, p + 72, 32);
	if (feedback_validate(&out))
		return -EBADMSG;
	*feedback = out;
	return 0;
}

static int ack_validate(const struct spf_scan_ack *ack)
{
	if (!ack || !ack->sequence || ack->target >= SPF_SCAN_TARGETS ||
	    ack->result > SPF_SCAN_CANCELLED ||
	    ((ack->result == SPF_SCAN_APPLIED) != (ack->first_visit != UINT64_MAX)))
		return -EINVAL;
	return 0;
}

int spf_scan_ack_encode(void *wire, size_t bytes, const struct spf_scan_ack *ack)
{
	uint8_t p[SPF_SCAN_ACK_BYTES] = { 0 };

	if (!wire || ack_validate(ack))
		return -EINVAL;
	if (bytes < sizeof(p))
		return -ENOSPC;
	header(p, MAGIC_ACK, sizeof(p), 0);
	put64(p + 16, ack->sequence); put64(p + 24, ack->source_visit);
	put64(p + 32, ack->first_visit); put64(p + 40, ack->received_counter);
	put64(p + 48, ack->application_counter); put32(p + 56, ack->target);
	put32(p + 60, ack->result); put32(p + 64, ack->old_boost);
	put32(p + 68, ack->new_boost); put32(p + 92, crc32(p, 92));
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_scan_ack_decode(struct spf_scan_ack *ack, const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	struct spf_scan_ack out = { 0 };
	int ret = check(p, bytes, SPF_SCAN_ACK_BYTES, MAGIC_ACK, 0);

	if (!ack)
		return -EINVAL;
	if (ret)
		return ret;
	if (!all_zero(p + 72, 20))
		return -EBADMSG;
	out.sequence = get64(p + 16); out.source_visit = get64(p + 24);
	out.first_visit = get64(p + 32); out.received_counter = get64(p + 40);
	out.application_counter = get64(p + 48); out.target = get32(p + 56);
	out.result = get32(p + 60); out.old_boost = get32(p + 64);
	out.new_boost = get32(p + 68);
	if (ack_validate(&out))
		return -EBADMSG;
	*ack = out;
	return 0;
}

static int terminal_validate(const struct spf_scan_terminal *terminal)
{
	uint64_t remaining;

	if (!terminal || !terminal->session || !terminal->generation ||
	    terminal->restore_before > terminal->restore_after ||
	    terminal->state < SPF_SCAN_TERMINAL_COMPLETED ||
	    terminal->state > SPF_SCAN_TERMINAL_FAILED ||
	    (terminal->state == SPF_SCAN_TERMINAL_COMPLETED && terminal->error) ||
	    (terminal->state == SPF_SCAN_TERMINAL_COMPLETED &&
	     terminal->restore_before < terminal->final_counter) ||
	    (terminal->state == SPF_SCAN_TERMINAL_FAILED && terminal->error >= 0) ||
	    terminal->flags != SPF_SCAN_TERMINAL_FLAGS ||
	    terminal->delivered > terminal->planned)
		return -EINVAL;
	remaining = terminal->planned - terminal->delivered;
	if (terminal->skipped > remaining)
		return -EINVAL;
	remaining -= terminal->skipped;
	if (terminal->invalid > remaining)
		return -EINVAL;
	remaining -= terminal->invalid;
	if (terminal->cancelled != remaining)
		return -EINVAL;
	return 0;
}

int spf_scan_terminal_encode(void *wire, size_t bytes,
			     const struct spf_scan_terminal *terminal)
{
	uint8_t p[SPF_SCAN_TERMINAL_BYTES] = { 0 };

	if (!wire || terminal_validate(terminal))
		return -EINVAL;
	if (bytes < sizeof(p))
		return -ENOSPC;
	header(p, MAGIC_TERMINAL, sizeof(p), terminal->flags);
	put64(p + 16, terminal->session); put64(p + 24, terminal->generation);
	put64(p + 32, terminal->final_counter); put64(p + 40, terminal->restore_before);
	put64(p + 48, terminal->restore_after); put64(p + 56, terminal->planned);
	put64(p + 64, terminal->delivered); put64(p + 72, terminal->skipped);
	put64(p + 80, terminal->invalid); put64(p + 88, terminal->cancelled);
	put64(p + 96, terminal->iq_bytes); put32(p + 104, terminal->state);
	put32(p + 108, terminal->reason); put32(p + 112, (uint32_t)terminal->error);
	put32(p + 116, terminal->flags); put32(p + 124, crc32(p, 124));
	memcpy(wire, p, sizeof(p));
	return 0;
}

int spf_scan_terminal_decode(struct spf_scan_terminal *terminal,
			     const void *wire, size_t bytes)
{
	const uint8_t *p = wire;
	struct spf_scan_terminal out = { 0 };
	int ret;

	if (!terminal)
		return -EINVAL;
	if (!p || bytes != SPF_SCAN_TERMINAL_BYTES)
		return !p ? -EINVAL : -EMSGSIZE;
	ret = check(p, bytes, SPF_SCAN_TERMINAL_BYTES, MAGIC_TERMINAL, get32(p + 12));
	if (ret)
		return ret;
	if (get32(p + 120))
		return -EBADMSG;
	out.session = get64(p + 16); out.generation = get64(p + 24);
	out.final_counter = get64(p + 32); out.restore_before = get64(p + 40);
	out.restore_after = get64(p + 48); out.planned = get64(p + 56);
	out.delivered = get64(p + 64); out.skipped = get64(p + 72);
	out.invalid = get64(p + 80); out.cancelled = get64(p + 88);
	out.iq_bytes = get64(p + 96); out.state = get32(p + 104);
	out.reason = get32(p + 108); out.error = (int32_t)get32(p + 112);
	out.flags = get32(p + 116);
	if (out.flags != get32(p + 12) || terminal_validate(&out))
		return -EBADMSG;
	*terminal = out;
	return 0;
}
