/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scan-session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define NO_ACTIVE SIZE_MAX
#define TERMINAL_FLAG_RESTORED UINT32_C(1)
#define TERMINAL_REASON_COMPLETE UINT32_C(1)
#define TERMINAL_REASON_INTERNAL UINT32_C(3)

struct ledger_entry {
	struct spf_scan_choice choice;
	struct spf_scan_radio_receipt recall;
	uint64_t valid_start, valid_end;
	enum spf_visit_result admission;
	bool closed, queued;
};

struct spf_scan_session {
	struct spf_scan_setup setup;
	struct spf_scan_policy *policy;
	struct spf_visit_queue *queue;
	struct spf_scan_radio *radio;
	struct ledger_entry *ledger;
	size_t ledger_capacity, ledger_count, active, output_index;
	uint64_t latest_counter, final_counter;
	uint64_t delivered, skipped, invalid, cancelled, iq_bytes;
	struct spf_scan_radio_release_receipt restoration;
	enum spf_visit_result inflight_result;
	int error;
	bool stopping, failed, cancelled_session, released, output_inflight;
	bool terminal_taken;
};

static uint64_t ticks(const struct spf_scan_session *session, uint32_t ms)
{
	return (uint64_t)session->setup.source_rate_hz * ms / 1000;
}

static int close_active(struct spf_scan_session *session, uint64_t counter)
{
	struct ledger_entry *entry;
	int ret;

	if (session->active == NO_ACTIVE)
		return 0;
	entry = &session->ledger[session->active];
	if (entry->closed)
		return -EALREADY;
	if (entry->queued) {
		ret = spf_visit_queue_close_window(session->queue,
						 entry->choice.visit, counter);
		if (ret)
			return ret;
	}
	entry->closed = true;
	session->active = NO_ACTIVE;
	return 0;
}

static int maybe_release(struct spf_scan_session *session)
{
	int ret;

	if (!session->stopping || session->released ||
	    !spf_visit_queue_capture_complete(session->queue))
		return 0;
	ret = spf_scan_radio_release(session->radio, session->latest_counter,
				     &session->restoration);
	if (ret) {
		session->failed = true;
		session->error = ret;
		return ret;
	}
	session->released = true;
	if (session->restoration.counter_after > session->latest_counter)
		session->latest_counter = session->restoration.counter_after;
	return 0;
}

static int fail_session(struct spf_scan_session *session, int error,
			uint64_t counter)
{
	if (!session->stopping) {
		(void)close_active(session, counter);
		spf_scan_policy_stop(session->policy, counter);
		(void)spf_visit_queue_cancel(session->queue);
		for (size_t i = 0; i < session->ledger_count; i++)
			session->ledger[i].closed = true;
		session->stopping = true;
		session->final_counter = counter;
	}
	session->failed = true;
	session->error = error < 0 ? error : -EIO;
	if (counter > session->latest_counter)
		session->latest_counter = counter;
	(void)maybe_release(session);
	return session->error;
}

int spf_scan_session_create(struct spf_scan_session **out,
	const struct spf_scan_setup *setup,
	const struct spf_scan_session_runtime *runtime,
	struct spf_scan_radio *radio, uint64_t start_counter)
{
	struct spf_scan_session *session;
	struct spf_scan_radio_profile profiles[SPF_SCAN_TARGETS];
	struct spf_visit_queue_config queue_config;
	struct spf_scan_policy_config policy_config;
	struct spf_scan_radio_release_receipt ignored;
	size_t capacity;
	unsigned i;
	int ret;

	if (!out || !setup || !runtime || !radio || !runtime->release_block)
		return -EINVAL;
	ret = spf_scan_setup_validate(setup);
	if (ret)
		return ret;
	capacity = setup->duration_ms / setup->dwell_ms + 1U;
	if (capacity > SPF_SCAN_MAX_VISITS)
		return -E2BIG;
	session = calloc(1, sizeof(*session));
	if (!session)
		return -ENOMEM;
	session->ledger = calloc(capacity, sizeof(*session->ledger));
	if (!session->ledger) {
		free(session);
		return -ENOMEM;
	}
	session->setup = *setup;
	session->radio = radio;
	session->ledger_capacity = capacity;
	session->active = NO_ACTIVE;
	session->latest_counter = start_counter;
	for (i = 0; i < setup->target_count; i++) {
		profiles[i].profile = setup->targets[i].profile;
		profiles[i].frequency_hz = setup->targets[i].frequency_hz;
		profiles[i].crc32 = setup->targets[i].profile_crc32;
	}
	ret = spf_scan_radio_acquire(radio, setup->source_rate_hz,
				     runtime->block_samples);
	if (ret)
		goto restore;
	ret = spf_scan_radio_configure(radio, profiles, setup->target_count);
	if (ret)
		goto restore;
	spf_scan_setup_policy(setup, &policy_config);
	ret = spf_scan_policy_create(&session->policy, &policy_config, start_counter);
	if (ret)
		goto restore;
	queue_config = (struct spf_visit_queue_config) {
		.block_count = runtime->block_count,
		.headroom_blocks = runtime->headroom_blocks,
		.maximum_visits = setup->maximum_queue_visits,
		.block_samples = runtime->block_samples,
		.source_rate_hz = setup->source_rate_hz,
		.maximum_bytes = setup->maximum_queue_bytes,
		.maximum_age_ticks = (uint64_t)setup->source_rate_hz *
			setup->maximum_queue_age_ms / 1000,
		.drain_bytes_per_second = runtime->drain_bytes_per_second,
	};
	ret = spf_visit_queue_create(&session->queue, &queue_config,
				     runtime->release_block,
				     runtime->release_context);
	if (ret)
		goto restore;
	*out = session;
	return 0;

restore:
	(void)spf_scan_radio_release(radio, start_counter, &ignored);
	spf_scan_policy_destroy(session->policy);
	free(session->ledger);
	free(session);
	return ret;
}

int spf_scan_session_schedule(struct spf_scan_session *session,
	uint64_t now, uint64_t counter_anchor, struct spf_scan_choice *choice)
{
	struct spf_scan_radio_receipt recall;
	struct ledger_entry *entry;
	enum spf_visit_result admission;
	struct spf_scan_choice selected;
	uint64_t valid_start, transition;
	uint32_t samples;
	int ret;

	if (!session || !choice || session->stopping || session->failed)
		return -EINVAL;
	ret = close_active(session, now);
	if (ret)
		return fail_session(session, ret, now);
	ret = spf_scan_policy_select(session->policy, now, &selected);
	if (ret)
		return ret;
	if (selected.visit != session->ledger_count ||
	    session->ledger_count >= session->ledger_capacity)
		return fail_session(session, -EOVERFLOW, now);
	entry = &session->ledger[session->ledger_count++];
	memset(entry, 0, sizeof(*entry));
	entry->choice = selected;
	samples = (uint32_t)ticks(session, session->setup.dwell_ms);
	ret = spf_visit_queue_reserve(session->queue, selected.visit, samples,
				      now, &admission);
	if (ret)
		return fail_session(session, ret, now);
	entry->admission = admission;
	entry->queued = admission == SPF_VISIT_ADMITTED;
	ret = spf_scan_radio_recall(session->radio,
			session->setup.targets[selected.target].profile,
			counter_anchor, &recall);
	if (ret)
		return fail_session(session, ret, now);
	entry->recall = recall;
	transition = ticks(session, session->setup.transition_budget_ms);
	if (selected.selection_counter > UINT64_MAX - transition)
		return fail_session(session, -EOVERFLOW, recall.counter_after);
	valid_start = selected.selection_counter + transition;
	if (recall.counter_before < selected.selection_counter ||
	    recall.counter_before > recall.counter_after ||
	    recall.counter_after > valid_start)
		return fail_session(session, -ETIME, recall.counter_after);
	ret = spf_scan_policy_commit(session->policy, valid_start);
	if (ret)
		return fail_session(session, ret, recall.counter_after);
	if (valid_start > UINT64_MAX - samples)
		return fail_session(session, -EOVERFLOW, recall.counter_after);
	entry->valid_start = valid_start;
	entry->valid_end = valid_start + samples;
	if (entry->queued) {
		ret = spf_visit_queue_bind(session->queue, selected.visit,
						 valid_start);
		if (ret)
			return fail_session(session, ret, recall.counter_after);
	}
	session->active = selected.visit;
	if (recall.counter_after > session->latest_counter)
		session->latest_counter = recall.counter_after;
	*choice = selected;
	return 0;
}

int spf_scan_session_next_boundary(const struct spf_scan_session *session,
	uint64_t *counter)
{
	if (!session || !counter)
		return -EINVAL;
	if (session->stopping)
		return -ESHUTDOWN;
	if (session->active == NO_ACTIVE)
		return -EAGAIN;
	*counter = session->ledger[session->active].valid_end;
	return 0;
}

int spf_scan_session_feed(struct spf_scan_session *session, uintptr_t token,
	const void *data, uint64_t first, uint32_t samples)
{
	int ret;

	if (!session || session->released || first > UINT64_MAX - samples)
		return -EINVAL;
	ret = spf_visit_queue_feed(session->queue, token, data, first, samples);
	if (ret)
		return fail_session(session, ret, first);
	if (first + samples > session->latest_counter)
		session->latest_counter = first + samples;
	ret = spf_visit_queue_reap(session->queue);
	if (ret)
		return fail_session(session, ret, first + samples);
	return maybe_release(session);
}

enum spf_scan_feedback_result spf_scan_session_feedback(
	struct spf_scan_session *session,
	const struct spf_scan_feedback *feedback, uint64_t received_counter)
{
	if (!session || session->stopping || session->failed)
		return SPF_SCAN_REJECTED;
	return spf_scan_policy_feedback(session->policy, feedback,
					received_counter);
}

int spf_scan_session_take_ack(struct spf_scan_session *session,
	struct spf_scan_ack *ack)
{
	return session ? spf_scan_policy_take_ack(session->policy, ack) : -EINVAL;
}

static void make_record(const struct spf_scan_session *session,
	const struct ledger_entry *entry, enum spf_visit_result result,
	struct spf_scan_visit_record *record)
{
	const struct spf_scan_target *target =
		&session->setup.targets[entry->choice.target];

	*record = (struct spf_scan_visit_record) {
		.session = session->setup.session,
		.generation = session->setup.generation,
		.visit = entry->choice.visit,
		.selection_counter = entry->choice.selection_counter,
		.transition_before = entry->recall.counter_before,
		.transition_after = entry->recall.counter_after,
		.valid_start = entry->valid_start,
		.valid_end = entry->valid_end,
		.frequency_hz = target->frequency_hz,
		.iq_bytes = result == SPF_VISIT_COMPLETE ?
			(entry->valid_end - entry->valid_start) * 4 : 0,
		.analog_bandwidth_hz = session->setup.analog_bandwidth_hz,
		.source_rate_hz = session->setup.source_rate_hz,
		.target = entry->choice.target,
		.profile = target->profile,
		.result = result,
		.eligible_mask = entry->choice.eligible_mask,
		.effective_weight = entry->choice.effective_weight,
		.profile_crc32 = entry->recall.profile_crc32,
		.flags = SPF_SCAN_VISIT_FLAGS |
			(entry->choice.deadline_forced ?
			 SPF_SCAN_VISIT_DEADLINE_FORCED : 0),
	};
}

int spf_scan_session_take_output(struct spf_scan_session *session,
	struct spf_scan_session_output *output)
{
	struct ledger_entry *entry;
	struct spf_visit_view view;
	int ret;

	if (!session || !output)
		return -EINVAL;
	if (session->output_inflight)
		return -EBUSY;
	if (session->output_index >= session->ledger_count)
		return -EAGAIN;
	entry = &session->ledger[session->output_index];
	if (!entry->closed)
		return -EAGAIN;
	memset(output, 0, sizeof(*output));
	if (!entry->queued) {
		make_record(session, entry, entry->admission, &output->record);
		session->inflight_result = entry->admission;
	} else {
		ret = spf_visit_queue_take(session->queue, &view);
		if (ret)
			return ret;
		if (view.id != entry->choice.visit)
			return fail_session(session, -EILSEQ, session->latest_counter);
		make_record(session, entry, view.result, &output->record);
		session->inflight_result = view.result;
		output->slice_count = view.slice_count;
		memcpy(output->slices, view.slices,
		       view.slice_count * sizeof(*view.slices));
	}
	session->output_inflight = true;
	return 0;
}

static int finish_output(struct spf_scan_session *session, uint64_t visit,
			 bool transported, int transport_error)
{
	struct ledger_entry *entry;
	struct spf_visit_view view;
	enum spf_visit_result result;
	int ret;

	if (!session || !session->output_inflight ||
	    session->output_index >= session->ledger_count)
		return -EINVAL;
	entry = &session->ledger[session->output_index];
	if (entry->choice.visit != visit)
		return -EINVAL;
	result = session->inflight_result;
	if (entry->queued) {
		ret = spf_visit_queue_take(session->queue, &view);
		if (ret != -EBUSY)
			return ret ? ret : -EIO;
		/* The pinned head is the output currently being completed. */
		ret = spf_visit_queue_complete_send(session->queue, visit);
		if (ret)
			return fail_session(session, ret, session->latest_counter);
		ret = spf_visit_queue_reap(session->queue);
		if (ret)
			return fail_session(session, ret, session->latest_counter);
	}
	if (!transported)
		result = SPF_VISIT_CANCELLED;
	if (entry->valid_end > entry->valid_start) {
		ret = spf_scan_policy_finish_visit(session->policy, visit,
						   result == SPF_VISIT_COMPLETE);
		if (ret)
			return fail_session(session, ret, session->latest_counter);
	}
	if (result == SPF_VISIT_COMPLETE) {
		session->delivered++;
		session->iq_bytes += (entry->valid_end - entry->valid_start) * 4;
	} else if (result == SPF_VISIT_SKIP_CAPACITY || result == SPF_VISIT_SKIP_AGE) {
		session->skipped++;
	} else if (result == SPF_VISIT_INVALID_GAP) {
		session->invalid++;
	} else {
		session->cancelled++;
	}
	session->output_inflight = false;
	session->output_index++;
	if (!transported)
		return fail_session(session,
			transport_error < 0 ? transport_error : -EIO,
			session->latest_counter);
	return maybe_release(session);
}

int spf_scan_session_complete_output(struct spf_scan_session *session,
	uint64_t visit)
{
	return finish_output(session, visit, true, 0);
}

int spf_scan_session_abort_output(struct spf_scan_session *session,
	uint64_t visit, int transport_error)
{
	return finish_output(session, visit, false, transport_error);
}

int spf_scan_session_stop(struct spf_scan_session *session,
	uint64_t final_counter)
{
	int ret;

	if (!session || session->stopping)
		return -EINVAL;
	ret = close_active(session, final_counter);
	if (ret)
		return fail_session(session, ret, final_counter);
	spf_scan_policy_stop(session->policy, final_counter);
	session->stopping = true;
	session->final_counter = final_counter;
	if (final_counter > session->latest_counter)
		session->latest_counter = final_counter;
	return maybe_release(session);
}

int spf_scan_session_cancel(struct spf_scan_session *session,
	uint64_t final_counter)
{
	int ret;

	if (!session || session->stopping)
		return -EINVAL;
	ret = close_active(session, final_counter);
	if (ret)
		return fail_session(session, ret, final_counter);
	spf_scan_policy_stop(session->policy, final_counter);
	ret = spf_visit_queue_cancel(session->queue);
	if (ret && ret != -EBUSY)
		return fail_session(session, ret, final_counter);
	session->stopping = true;
	session->cancelled_session = true;
	session->error = -ECANCELED;
	session->final_counter = final_counter;
	if (final_counter > session->latest_counter)
		session->latest_counter = final_counter;
	return maybe_release(session);
}

int spf_scan_session_terminal(struct spf_scan_session *session,
	struct spf_scan_terminal *terminal)
{
	struct spf_visit_queue_stats stats;

	if (!session || !terminal)
		return -EINVAL;
	if (!session->stopping || !session->released || session->output_inflight ||
	    session->output_index != session->ledger_count)
		return -EAGAIN;
	spf_visit_queue_stats(session->queue, &stats);
	if (stats.visits || stats.leased_blocks || stats.reserved_blocks ||
	    stats.reserved_bytes)
		return -EAGAIN;
	if (session->terminal_taken)
		return -EALREADY;
	*terminal = (struct spf_scan_terminal) {
		.session = session->setup.session,
		.generation = session->setup.generation,
		.final_counter = session->final_counter,
		.restore_before = session->restoration.counter_before,
		.restore_after = session->restoration.counter_after,
		.planned = session->ledger_count,
		.delivered = session->delivered,
		.skipped = session->skipped,
		.invalid = session->invalid,
		.cancelled = session->cancelled,
		.iq_bytes = session->iq_bytes,
		.state = session->failed ? SPF_SCAN_TERMINAL_FAILED :
			(session->cancelled_session ? SPF_SCAN_TERMINAL_CANCELLED :
			 SPF_SCAN_TERMINAL_COMPLETED),
		.reason = session->failed ? TERMINAL_REASON_INTERNAL :
			(session->cancelled_session ? UINT32_C(2) :
			 TERMINAL_REASON_COMPLETE),
		.error = session->failed || session->cancelled_session ?
			session->error : 0,
		.flags = TERMINAL_FLAG_RESTORED,
	};
	session->terminal_taken = true;
	return 0;
}

int spf_scan_session_destroy(struct spf_scan_session *session)
{
	int ret;

	if (!session)
		return -EINVAL;
	if (!session->released || session->output_inflight ||
	    session->output_index != session->ledger_count)
		return -EBUSY;
	ret = spf_visit_queue_destroy(session->queue);
	if (ret)
		return ret;
	spf_scan_policy_destroy(session->policy);
	free(session->ledger);
	free(session);
	return 0;
}
