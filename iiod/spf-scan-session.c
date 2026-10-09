/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "spf-scan-session.h"
#include "spf-scan-rate.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#define NO_ACTIVE UINT64_MAX
#define TERMINAL_FLAG_RESTORED UINT32_C(1)
#define TERMINAL_REASON_COMPLETE UINT32_C(1)
#define TERMINAL_REASON_INTERNAL UINT32_C(3)
/* Failed-session subreasons are intentionally carried in the existing
 * protocol reason field.  Older clients treat this field as opaque. */
#define TERMINAL_REASON_POLICY_SELECT UINT32_C(4)
#define TERMINAL_REASON_RECALL_LATE UINT32_C(5)
#define TERMINAL_REASON_POLICY_COMMIT UINT32_C(6)

struct ledger_entry {
	struct spf_scan_choice choice;
	struct spf_scan_radio_receipt recall;
	uint64_t valid_start, valid_end;
	enum spf_visit_result admission;
	struct spf_scan_gain_observation gain;
	bool closed, queued;
};

struct spf_scan_session {
	struct spf_scan_setup setup;
	struct spf_scan_policy *policy;
	struct spf_visit_queue *queue;
	struct spf_scan_radio *radio;
	uint32_t bytes_per_sample;
	struct ledger_entry *ledger;
	size_t ledger_capacity;
	uint64_t ledger_count, active, output_index;
	uint64_t latest_counter, final_counter;
	uint32_t current_profile;
	uint64_t delivered, skipped, invalid, cancelled, iq_bytes;
	uint64_t inflight_bytes;
	struct spf_scan_radio_release_receipt restoration;
	enum spf_visit_result inflight_result;
	int error;
	int restoration_error;
	uint32_t failure_stage;
	uint64_t failure_counter, failure_ns, failure_visit;
	uint32_t failure_reason;
	bool stopping, failed, cancelled_session, released, output_inflight;
	bool current_profile_valid;
	bool terminal_taken;
};

static uint64_t ticks(const struct spf_scan_session *session, uint32_t ms)
{
	return spf_scan_ticks(session->setup.source_rate_hz, ms);
}

static int close_active(struct spf_scan_session *session, uint64_t counter)
{
	struct ledger_entry *entry;
	int ret;

	if (session->active == NO_ACTIVE)
		return 0;
	entry = &session->ledger[session->active % session->ledger_capacity];
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
		if (!session->restoration_error)
			session->restoration_error = ret;
		if (!session->failed) {
			session->error = ret;
			session->failure_stage = SPF_SCAN_STAGE_RESTORE;
		}
		session->failed = true;
		return ret;
	}
	session->released = true;
	if (session->restoration.counter_after > session->latest_counter)
		session->latest_counter = session->restoration.counter_after;
	return 0;
}

static int fail_session_reason(struct spf_scan_session *session, int error,
			uint64_t counter, uint32_t reason)
{
	if (!session->stopping) {
		(void)close_active(session, counter);
		spf_scan_policy_stop(session->policy, counter);
		for (uint64_t i = session->output_index; i < session->ledger_count; i++)
			session->ledger[i % session->ledger_capacity].closed = true;
		session->stopping = true;
		session->final_counter = counter;
	}
	/* A gracefully stopped session can still be waiting for its final DMA
	 * block. Failure must abandon those leases too, or terminal waits forever. */
	(void)spf_visit_queue_cancel(session->queue);
	if (!session->failed) {
		struct timespec timestamp;
		session->error = error < 0 ? error : -EIO;
		session->failure_counter = counter;
		session->failure_visit = session->ledger_count ? session->ledger_count - 1 : UINT64_MAX;
		if (!clock_gettime(CLOCK_MONOTONIC, &timestamp))
			session->failure_ns = (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) + timestamp.tv_nsec;
		if (!session->failure_stage)
			session->failure_stage = SPF_SCAN_STAGE_SESSION;
		if (reason)
			session->failure_reason = reason;
	}
	session->failed = true;
	if (counter > session->latest_counter)
		session->latest_counter = counter;
	(void)maybe_release(session);
	return session->error;
}

static int fail_session(struct spf_scan_session *session, int error,
	uint64_t counter)
{
	return fail_session_reason(session, error, counter, 0);
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
	uint64_t actual_start, visit_samples, visit_bytes, required_blocks;
	int ret;

	if (!out || !setup || !runtime || !radio || !runtime->release_block ||
	    (runtime->bytes_per_sample != 4 && runtime->bytes_per_sample != 8))
		return -EINVAL;
	ret = spf_scan_setup_validate(setup);
	if (ret)
		return ret;
	if (setup->protocol_version == SPF_SCAN_RANDOM_DWELL_VERSION ||
	    setup->protocol_version == SPF_SCAN_FIXED_DWELL_VERSION ||
	    setup->protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION) {
		if (!runtime->block_samples || runtime->block_count < 4 ||
		    !runtime->headroom_blocks ||
		    runtime->headroom_blocks >= runtime->block_count)
			return -EINVAL;
		visit_samples = spf_scan_ticks(setup->source_rate_hz, setup->dwell_ms);
		visit_bytes = visit_samples * runtime->bytes_per_sample;
		required_blocks = (visit_samples + runtime->block_samples - 1) /
			runtime->block_samples + 1;
		if (visit_bytes > setup->maximum_queue_bytes ||
		    required_blocks > runtime->block_count - runtime->headroom_blocks)
			return -ENOSPC;
	}
	capacity = setup->duration_ms /
		(setup->protocol_version == SPF_SCAN_RANDOM_DWELL_VERSION ?
		 120U : setup->dwell_ms) + 1U;
	if (setup->protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
		capacity = setup->maximum_queue_visits + 2U;
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
	session->bytes_per_sample = runtime->bytes_per_sample;
	session->ledger_capacity = capacity;
	session->active = NO_ACTIVE;
	session->latest_counter = start_counter;
	for (i = 0; i < setup->target_count; i++) {
		profiles[i].profile = setup->targets[i].profile;
		profiles[i].frequency_hz = setup->targets[i].frequency_hz;
		profiles[i].crc32 = setup->targets[i].profile_crc32;
	}
	ret = spf_scan_radio_acquire(radio, setup->source_rate_hz,
				     runtime->block_samples,
				     setup->rx_mask == SPF_SCAN_RX1_RX2 ?
				     UINT32_C(0x0f) : UINT32_C(0x03));
	if (ret)
		goto restore;
	ret = spf_scan_radio_diag_context(radio, setup->session, UINT64_MAX);
	if (ret)
		goto restore;
	ret = spf_scan_radio_configure(radio, profiles, setup->target_count);
	if (ret)
		goto restore;
	ret = spf_scan_radio_snapshot(radio, start_counter, &actual_start);
	if (ret)
		goto restore;
	session->latest_counter = actual_start;
	spf_scan_setup_policy(setup, &policy_config);
	ret = spf_scan_policy_create(&session->policy, &policy_config, actual_start);
	if (ret)
		goto restore;
	queue_config = (struct spf_visit_queue_config) {
		.block_count = runtime->block_count,
		.headroom_blocks = runtime->headroom_blocks,
		.maximum_visits = setup->maximum_queue_visits,
		.block_samples = runtime->block_samples,
		.bytes_per_sample = runtime->bytes_per_sample,
		.source_rate_hz = setup->source_rate_hz,
		.maximum_bytes = setup->maximum_queue_bytes,
		.maximum_age_ticks = (uint64_t)setup->source_rate_hz *
			setup->maximum_queue_age_ms / 1000,
		.drain_bytes_per_second = runtime->drain_bytes_per_second,
		.maximum_visit_ms = setup->protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION ? 20U :
			setup->protocol_version == SPF_SCAN_RANDOM_DWELL_VERSION ||
			setup->protocol_version == SPF_SCAN_FIXED_DWELL_VERSION ? 360U : 240U,
		.preserve_partial = setup->protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION,
	};
	ret = spf_visit_queue_create(&session->queue, &queue_config,
				     runtime->release_block,
				     runtime->release_context);
	if (ret)
		goto restore;
	*out = session;
	return 0;

restore:
	(void)spf_scan_radio_release(radio, session->latest_counter, &ignored);
	spf_scan_policy_destroy(session->policy);
	free(session->ledger);
	free(session);
	return ret;
}

int spf_scan_session_rebase(struct spf_scan_session *session,
	uint64_t full_counter_anchor)
{
	struct spf_scan_policy_config policy_config;
	struct spf_scan_policy *policy;
	uint64_t actual;
	int ret;

	if (!session || session->stopping || session->released ||
	    session->ledger_count || session->active != NO_ACTIVE ||
	    session->output_index || session->output_inflight)
		return -EBUSY;
	ret = spf_scan_radio_snapshot(session->radio, full_counter_anchor, &actual);
	if (ret)
		return fail_session(session, ret, full_counter_anchor);
	spf_scan_setup_policy(&session->setup, &policy_config);
	ret = spf_scan_policy_create(&policy, &policy_config, actual);
	if (ret)
		return fail_session(session, ret, actual);
	spf_scan_policy_destroy(session->policy);
	session->policy = policy;
	session->latest_counter = actual;
	return 0;
}

static void make_failed_entry_coherent(struct spf_scan_session *session,
	struct ledger_entry *entry, uint64_t counter, bool queued)
{
	const struct spf_scan_target *target =
		&session->setup.targets[entry->choice.target];

	if (!queued)
		entry->admission = SPF_VISIT_CANCELLED;
	entry->queued = queued;
	entry->recall = (struct spf_scan_radio_receipt) {
		.profile = target->profile,
		.frequency_hz = target->frequency_hz,
		.profile_crc32 = target->profile_crc32,
		.counter_before = counter,
		.counter_after = counter,
	};
	entry->valid_start = counter;
	entry->valid_end = counter;
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
	if (ret) {
		if (ret == -ETIME || session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
			return fail_session_reason(session, ret, now,
				TERMINAL_REASON_POLICY_SELECT);
		return ret;
	}
	if (selected.visit != session->ledger_count ||
	    session->ledger_count - session->output_index >= session->ledger_capacity)
		return fail_session(session, -EOVERFLOW, now);
	entry = &session->ledger[session->ledger_count++ % session->ledger_capacity];
	memset(entry, 0, sizeof(*entry));
	entry->choice = selected;
	samples = (uint32_t)ticks(session, selected.dwell_ms);
	if (session->current_profile_valid &&
	    session->current_profile == session->setup.targets[selected.target].profile) {
		/* The shared LO is already at this target.  Do not manufacture an
		 * invalid interval or perform a redundant Fast Lock recall. */
		recall = (struct spf_scan_radio_receipt) {
			.profile = session->current_profile,
			.frequency_hz = session->setup.targets[selected.target].frequency_hz,
			.profile_crc32 = session->setup.targets[selected.target].profile_crc32,
			.counter_before = counter_anchor,
			.counter_after = counter_anchor,
		};
	} else {
		ret = spf_scan_radio_diag_context(session->radio,
			session->setup.session, selected.visit);
		if (!ret)
			ret = spf_scan_radio_recall(session->radio,
			session->setup.targets[selected.target].profile,
			counter_anchor, &recall);
		if (ret) {
			make_failed_entry_coherent(session, entry, now, entry->queued);
			return spf_scan_session_fail_stage(session, now, ret,
						   SPF_SCAN_STAGE_RECALL);
		}
		session->current_profile = recall.profile;
		session->current_profile_valid = true;
	}
	entry->recall = recall;
	transition = session->current_profile_valid &&
		session->current_profile == session->setup.targets[selected.target].profile &&
		recall.counter_before == recall.counter_after ? 0 :
		ticks(session, session->setup.transition_budget_ms);
	if (session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
		transition = ticks(session, session->setup.transition_budget_ms);
	if (selected.selection_counter > UINT64_MAX - transition) {
		make_failed_entry_coherent(session, entry, now, entry->queued);
		return fail_session(session, -EOVERFLOW, recall.counter_after);
	}
	valid_start = selected.selection_counter + transition;
	/* A snapshot and a Fast-Lock receipt are separate kernel operations.  The
	 * receipt can legitimately precede selection. Neither clock boundary may
	 * move backwards, and the continuous guard must follow actual completion. */
	if (recall.counter_before > recall.counter_after) {
		make_failed_entry_coherent(session, entry, now, entry->queued);
		return fail_session_reason(session, -ETIME, recall.counter_after,
			TERMINAL_REASON_RECALL_LATE);
	}
	/* Correct the unpublished continuous candidate: selection plus a budget
	 * does not guarantee that budget after recall. Even a recall completing one
	 * tick before the old boundary needs the entire configured guard. Retain
	 * the existing finite v1-v4 boundary rule and their historical receipts. */
	if (session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION ||
	    recall.counter_after > valid_start) {
		if (recall.counter_after > UINT64_MAX - transition) {
			make_failed_entry_coherent(session, entry, now, entry->queued);
			return fail_session(session, -EOVERFLOW, recall.counter_after);
		}
		if (recall.counter_after + transition > valid_start)
			valid_start = recall.counter_after + transition;
	}
	ret = spf_scan_policy_commit(session->policy, valid_start);
	if (ret == -ENODATA && session->ledger_count > 1 &&
	    session->setup.protocol_version != SPF_SCAN_CONTINUOUS_ORDERED_VERSION) {
		/* Recall succeeded, but there is no room for another complete dwell.
		 * Keep earlier windows drainable and exclude this uncommitted choice
		 * from the ledger. The caller takes the normal end-of-scan path. */
		session->ledger_count--;
		if (recall.counter_after > session->latest_counter)
			session->latest_counter = recall.counter_after;
		return -ENODATA;
	}
	/* A session that cannot capture even its first dwell still fails closed. */
	if (ret == -ENODATA)
		ret = -ETIME;
	if (ret) {
		make_failed_entry_coherent(session, entry, now, entry->queued);
		return fail_session_reason(session, ret, recall.counter_after,
			ret == -ETIME ? TERMINAL_REASON_POLICY_COMMIT : 0);
	}
	if (valid_start > UINT64_MAX - samples) {
		make_failed_entry_coherent(session, entry, now, entry->queued);
		return fail_session(session, -EOVERFLOW, recall.counter_after);
	}
	/* Reserve only after the measured window fits the session deadline. */
	ret = spf_visit_queue_reserve(session->queue, selected.visit, samples,
				      now, &admission);
	if (ret) {
		make_failed_entry_coherent(session, entry, now, false);
		return fail_session(session, ret, now);
	}
	entry->admission = admission;
	entry->queued = admission == SPF_VISIT_ADMITTED;
	entry->valid_start = valid_start;
	entry->valid_end = valid_start + samples;
	if (!entry->queued && session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION) {
		entry->closed = true;
		return fail_session(session, -ENOSPC, recall.counter_after);
	}
	if (entry->queued) {
		ret = spf_visit_queue_bind(session->queue, selected.visit,
						 valid_start);
		if (ret) {
			make_failed_entry_coherent(session, entry, now, true);
			return fail_session(session, ret, recall.counter_after);
		}
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
	*counter = session->ledger[session->active % session->ledger_capacity].valid_end;
	return 0;
}

int spf_scan_session_observe_gain(struct spf_scan_session *session,
	uint64_t visit, const struct spf_scan_gain_observation *observation)
{
	struct ledger_entry *entry;

	if (!session || !observation || visit >= session->ledger_count ||
	    visit != session->active)
		return -EINVAL;
	/* Gain telemetry was negotiated only by v3. In older protocols these
	 * bytes are reserved; observing gain must not make a valid visit
	 * impossible to encode (and strand READSCAN before its first record). */
	if (session->setup.protocol_version != SPF_SCAN_RANDOM_DWELL_VERSION)
		return 0;
	if (session->ledger_count - visit > session->ledger_capacity)
		return -ESTALE;
	entry = &session->ledger[visit % session->ledger_capacity];
	if (observation->counter < entry->valid_end ||
	    (observation->valid &&
	     (observation->rx1_gain_index > UINT8_C(0x7f) ||
	      observation->rx2_gain_index > UINT8_C(0x7f))))
		return -ERANGE;
	entry->gain = *observation;
	return 0;
}

int spf_scan_session_counter(const struct spf_scan_session *session,
	uint64_t *counter)
{
	if (!session || !counter)
		return -EINVAL;
	*counter = session->latest_counter;
	return 0;
}

bool spf_scan_session_capture_complete(const struct spf_scan_session *session)
{
	return session && spf_visit_queue_capture_complete(session->queue);
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
	if (session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION && !session->stopping) {
		struct spf_visit_queue_stats stats;
		spf_visit_queue_stats(session->queue,&stats);
		if (stats.missing_samples) {
			/* The lease was consumed successfully. Signal failure through the
			 * session state, not a return value that would double-return it. */
			(void)fail_session(session,-ENODATA,first+samples);
			return 0;
		}
	}
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
		.protocol_version = session->setup.protocol_version,
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
			(entry->valid_end - entry->valid_start) * session->bytes_per_sample : 0,
		.analog_bandwidth_hz = session->setup.analog_bandwidth_hz,
		.source_rate_hz = session->setup.source_rate_hz,
		.target = entry->choice.target,
		.profile = target->profile,
		.result = result,
		.eligible_mask = entry->choice.eligible_mask,
		.effective_weight = entry->choice.effective_weight,
		.profile_crc32 = entry->recall.profile_crc32,
		.gain_counter = entry->gain.valid ? entry->gain.counter : 0,
		.gain_read_duration_ns = entry->gain.valid ?
			entry->gain.read_duration_ns : 0,
		.rx1_gain_index = entry->gain.valid ? entry->gain.rx1_gain_index : 0,
		.rx2_gain_index = entry->gain.valid ? entry->gain.rx2_gain_index : 0,
		.gain_valid = entry->gain.valid,
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
	entry = &session->ledger[session->output_index % session->ledger_capacity];
	if (!entry->closed)
		return -EAGAIN;
	memset(output, 0, sizeof(*output));
	if (!entry->queued) {
		make_record(session, entry, entry->admission, &output->record);
		if (session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
			output->record.valid_end = output->record.valid_start;
		session->inflight_result = entry->admission;
	} else {
		ret = spf_visit_queue_take(session->queue, &view);
		if (ret)
			return ret;
		if (view.id != entry->choice.visit)
			return fail_session(session, -EILSEQ, session->latest_counter);
		make_record(session, entry, view.result, &output->record);
		if (session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION) {
			output->record.valid_end = view.end;
			output->record.iq_bytes = (view.end-view.start)*session->bytes_per_sample;
		}
		session->inflight_result = view.result;
		output->slice_count = view.slice_count;
		memcpy(output->slices, view.slices,
		       view.slice_count * sizeof(*view.slices));
	}
	session->output_inflight = true;
	session->inflight_bytes = output->record.iq_bytes;
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
	entry = &session->ledger[session->output_index % session->ledger_capacity];
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
		if (session->setup.protocol_version != SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
			session->iq_bytes += (entry->valid_end - entry->valid_start) * session->bytes_per_sample;
	} else if (result == SPF_VISIT_SKIP_CAPACITY || result == SPF_VISIT_SKIP_AGE) {
		session->skipped++;
	} else if (result == SPF_VISIT_INVALID_GAP) {
		session->invalid++;
	} else {
		session->cancelled++;
	}
	if (transported && session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION) {
		if (session->inflight_bytes > UINT64_MAX-session->iq_bytes) {
			session->output_inflight=false;
			session->output_index++;
			return fail_session(session,-EOVERFLOW,session->latest_counter);
		}
		session->iq_bytes += session->inflight_bytes;
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

	if (!session)
		return -EINVAL;
	if (session->stopping)
		return session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION ? maybe_release(session) : -EINVAL;
	if (final_counter < session->latest_counter)
		final_counter = session->latest_counter;
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

	if (!session)
		return -EINVAL;
	if (session->stopping) {
		if (session->setup.protocol_version != SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
			return -EINVAL;
		if (session->released) return 0;
		(void)spf_visit_queue_cancel(session->queue);
		session->cancelled_session = true;
		if (!session->failed) session->error = -ECANCELED;
		return maybe_release(session);
	}
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

int spf_scan_session_fail(struct spf_scan_session *session,
	uint64_t final_counter, int error)
{
	if (!session || session->released || !error)
		return -EINVAL;
	return fail_session(session, error, final_counter);
}

int spf_scan_session_fail_stage(struct spf_scan_session *session,
	uint64_t counter, int error, uint32_t stage)
{
	if (!session)
		return -EINVAL;
	if (!session->failed)
		session->failure_stage = stage;
	return spf_scan_session_fail(session, counter, error);
}

static int diagnostic_event_json(char *output, size_t capacity,
				 const struct adi_rx_counter_diag_event *e)
{
	int size = snprintf(output, capacity,
		"{\"sequence\":%" PRIu64 ",\"session\":%" PRIu64 ",\"visit\":%" PRIu64
		",\"stage\":%u,\"step\":%u,\"profile\":%u,\"start_ns\":%" PRIu64
		",\"end_ns\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64
		",\"spi_last_ns\":%" PRIu64 ",\"spi_max_ns\":%" PRIu64
		",\"counter_before\":%u,\"counter_after\":%u,\"polls\":%u"
		",\"last_status\":%d,\"error\":%d}",
		(uint64_t)e->sequence, (uint64_t)e->session, (uint64_t)e->visit,
		e->stage, e->step, e->profile, (uint64_t)e->start_ns,
		(uint64_t)e->end_ns, (uint64_t)(e->end_ns - e->start_ns),
		(uint64_t)e->spi_last_ns, (uint64_t)e->spi_max_ns,
		e->counter_before, e->counter_after, e->polls, e->last_status, e->error);
	return size < 0 || (size_t)size >= capacity ? -ENOSPC : size;
}

int spf_scan_session_diagnostics(struct spf_scan_session *session,
	uint64_t identity, uint64_t generation, char *output, size_t capacity)
{
	struct adi_rx_counter_diagnostics kernel;
	size_t used;
	unsigned i;
	int ret, kernel_error;
	if (!session || !output || !capacity || capacity > SPF_SCAN_DIAG_MAX_BYTES)
		return -EINVAL;
	if (identity != session->setup.session || generation != session->setup.generation)
		return -ESTALE;
	kernel_error = spf_scan_radio_diagnostics(session->radio, &kernel);
	if (kernel_error)
		memset(&kernel, 0, sizeof(kernel));
	ret = snprintf(output, capacity,
		"{\"schema\":\"spf.scan-diagnostics/v1\",\"session\":%" PRIu64
		",\"generation\":%" PRIu64 ",\"first_error\":%d,\"restoration_error\":%d"
		",\"failure_stage\":%u,\"failure_counter\":%" PRIu64
		",\"failure_ns\":%" PRIu64 ",\"failure_visit\":%" PRIu64
		",\"kernel_error\":%d,\"kernel_total\":%" PRIu64 ",\"first_failure\":",
		identity, generation, session->error, session->restoration_error,
		session->failure_stage, session->failure_counter, session->failure_ns,
		session->failure_visit, kernel_error, (uint64_t)kernel.total);
	if (ret < 0 || (size_t)ret >= capacity)
		return -ENOSPC;
	used = (size_t)ret;
#define APPEND_EVENT(event) do { \
	ret = diagnostic_event_json(output + used, capacity - used, event); \
	if (ret < 0) return ret; used += (size_t)ret; \
} while (0)
#define APPEND_TEXT(text) do { \
	ret = snprintf(output + used, capacity - used, "%s", text); \
	if (ret < 0 || (size_t)ret >= capacity - used) return -ENOSPC; \
	used += (size_t)ret; \
} while (0)
	APPEND_EVENT(&kernel.first_failure);
	APPEND_TEXT(",\"restoration_failure\":");
	APPEND_EVENT(&kernel.restoration_failure);
	APPEND_TEXT(",\"events\":[");
	for (i = 0; i < kernel.count; i++) {
		/* Oldest first; the ABI stores the bounded ring in physical order. */
		uint64_t sequence = kernel.total - kernel.count + i;
		if (i) APPEND_TEXT(",");
		APPEND_EVENT(&kernel.events[sequence % ADI_RX_COUNTER_DIAG_CAPACITY]);
	}
	APPEND_TEXT("]}");
#undef APPEND_EVENT
#undef APPEND_TEXT
	return (int)used;
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
		.reason = session->failed ? (session->failure_reason ?
			session->failure_reason : TERMINAL_REASON_INTERNAL) :
			(session->cancelled_session ? UINT32_C(2) :
			 (session->setup.protocol_version == SPF_SCAN_CONTINUOUS_ORDERED_VERSION ? UINT32_C(2) : TERMINAL_REASON_COMPLETE)),
		.error = session->failed || session->cancelled_session ?
			session->error : 0,
		.flags = TERMINAL_FLAG_RESTORED,
	};
	session->terminal_taken = true;
	return 0;
}

int spf_scan_session_status(struct spf_scan_session *s,
	const struct spf_scan_control *q, struct spf_scan_status *out)
{
	if (!s || !q || !out || s->setup.protocol_version != SPF_SCAN_CONTINUOUS_ORDERED_VERSION)
		return -EOPNOTSUPP;
	if (q->session != s->setup.session || q->generation != s->setup.generation)
		return -ESTALE;
	*out = (struct spf_scan_status){
		.identity = *q, .planned = s->ledger_count, .delivered = s->delivered,
		.counter = s->latest_counter, .sweep = s->ledger_count / SPF_SCAN_TARGETS,
		.state = s->failed ? 4 : (s->released ? 3 : (s->stopping ? 2 : 1)),
		.target = UINT32_MAX,
		.queued_visits = (uint32_t)(s->ledger_count - s->output_index),
		.error = s->error, .restore_after = s->restoration.counter_after,
		.restored_flags = s->released ? TERMINAL_FLAG_RESTORED : 0,
		.terminal_state = s->stopping ? (s->failed ? SPF_SCAN_TERMINAL_FAILED :
			(s->cancelled_session ? SPF_SCAN_TERMINAL_CANCELLED : SPF_SCAN_TERMINAL_COMPLETED)) : 0,
		.terminal_reason = s->stopping ? (s->failed ? (s->failure_reason ? s->failure_reason : TERMINAL_REASON_INTERNAL) : 2) : 0,
	};
	if (s->active != NO_ACTIVE) {
		const struct ledger_entry *e = &s->ledger[s->active % s->ledger_capacity];
		out->target = e->choice.target;
		out->valid_start = e->valid_start;
		out->valid_end = e->valid_end;
	}
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
