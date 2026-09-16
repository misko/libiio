/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "spf-hop-scheduler.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPF_HOP_SCHEDULER_QUEUE_CAPACITY 64U
#define SPF_HOP_SCHEDULER_MAX_SLEEP_NS UINT64_C(5000000)

struct spf_hop_scheduler_v1 {
	struct spf_hop_request_v1 request;
	struct spf_hop_scheduler_io_v1 io;
	void *io_context;
	struct spf_hop_request_v2 adaptive_request;
	struct spf_hop_scheduler_policy_v2 policy;
	void *policy_context;
	bool adaptive;
	pthread_mutex_t lock;
	pthread_t thread;
	struct spf_hop_device_event_v2 queue[SPF_HOP_SCHEDULER_QUEUE_CAPACITY];
	struct spf_hop_restore_receipt_v1 restore_receipt;
	size_t queue_head;
	size_t queue_count;
	uint64_t dropped_events;
	int worker_error;
	int restore_error;
	bool thread_started;
	bool worker_done;
	bool cancel_requested;
	bool restore_called;
};

static int request_matches(const struct spf_hop_request_v1 *left,
	const struct spf_hop_request_v1 *right)
{
	uint8_t left_wire[SPF_HOP_REQUEST_BYTES];
	uint8_t right_wire[SPF_HOP_REQUEST_BYTES];

	if (spf_hop_request_v1_encode(left_wire, sizeof(left_wire), left) ||
		spf_hop_request_v1_encode(right_wire, sizeof(right_wire), right))
		return 0;
	return !memcmp(left_wire, right_wire, sizeof(left_wire));
}

static bool cancellation_requested(struct spf_hop_scheduler_v1 *scheduler)
{
	bool requested;

	pthread_mutex_lock(&scheduler->lock);
	requested = scheduler->cancel_requested;
	pthread_mutex_unlock(&scheduler->lock);
	return requested;
}

static int wait_until(struct spf_hop_scheduler_v1 *scheduler,
	uint64_t deadline)
{
	uint64_t counter;
	uint64_t remaining;
	uint64_t nanoseconds;
	int ret;

	for (;;) {
		if (cancellation_requested(scheduler))
			return -ECANCELED;
		ret = scheduler->io.get_counter(scheduler->io_context, &counter);
		if (ret)
			return ret > 0 ? -EIO : ret;
		if (counter >= deadline)
			return 0;
		remaining = deadline - counter;
		if (remaining > UINT64_MAX / UINT64_C(1000000000))
			nanoseconds = SPF_HOP_SCHEDULER_MAX_SLEEP_NS;
		else
			nanoseconds = remaining * UINT64_C(1000000000) /
				scheduler->request.sample_rate_hz;
		if (!nanoseconds)
			nanoseconds = 1;
		if (nanoseconds > SPF_HOP_SCHEDULER_MAX_SLEEP_NS)
			nanoseconds = SPF_HOP_SCHEDULER_MAX_SLEEP_NS;
		ret = scheduler->io.sleep_ns(scheduler->io_context, nanoseconds);
		if (ret)
			return ret > 0 ? -EIO : ret;
	}
}

static int enqueue_event(struct spf_hop_scheduler_v1 *scheduler,
	const struct spf_hop_device_event_v2 *event)
{
	size_t tail;

	pthread_mutex_lock(&scheduler->lock);
	if (scheduler->queue_count == SPF_HOP_SCHEDULER_QUEUE_CAPACITY) {
		scheduler->dropped_events++;
		pthread_mutex_unlock(&scheduler->lock);
		return -EOVERFLOW;
	}
	tail = (scheduler->queue_head + scheduler->queue_count) %
		SPF_HOP_SCHEDULER_QUEUE_CAPACITY;
	scheduler->queue[tail] = *event;
	scheduler->queue_count++;
	pthread_mutex_unlock(&scheduler->lock);
	return 0;
}

static void finish_worker(struct spf_hop_scheduler_v1 *scheduler, int error)
{
	if (error && error != -ECANCELED)
		fprintf(stderr, "SPF hop scheduler failed: error=%d\n",
			error);
	pthread_mutex_lock(&scheduler->lock);
	if (error != -ECANCELED)
		scheduler->worker_error = error;
	scheduler->worker_done = true;
	pthread_mutex_unlock(&scheduler->lock);
}

static void *scheduler_worker(void *opaque)
{
	struct spf_hop_scheduler_v1 *scheduler = opaque;
	struct spf_hop_scheduler_transition_v1 transition;
	struct spf_hop_device_event_v1 event;
	struct spf_hop_device_event_v2 adaptive_event;
	uint64_t startup_invalid_start = 0;
	uint64_t next_invalid_start = 0;
	uint8_t previous_profile = SPF_HOP_PROFILE_NONE;
	uint64_t index;
	int ret = 0;

	for (index = 0; index < scheduler->request.dwell_count; index++) {
		uint8_t profile = (uint8_t)(index % SPF_HOP_PROFILE_COUNT);

		if (index) {
			if (next_invalid_start < startup_invalid_start ||
				next_invalid_start - startup_invalid_start >=
					scheduler->request.capture_span_samples)
				break;
			ret = wait_until(scheduler, next_invalid_start);
			if (ret)
				break;
		} else if (cancellation_requested(scheduler)) {
			ret = -ECANCELED;
			break;
		}
		memset(&adaptive_event, 0, sizeof(adaptive_event));
		if (scheduler->adaptive) {
			uint64_t now;
			ret = scheduler->io.get_counter(scheduler->io_context, &now);
			if (ret) { ret = ret > 0 ? -EIO : ret; break; }
			ret = scheduler->policy.choose(scheduler->policy_context, index, now,
				&adaptive_event.choice);
			if (ret) { ret = ret > 0 ? -EIO : ret; break; }
			if (adaptive_event.choice.decision_counter != now ||
				adaptive_event.choice.generation != scheduler->adaptive_request.policy.generation ||
				adaptive_event.choice.mode != scheduler->adaptive_request.policy.mode ||
				adaptive_event.choice.proposed_target >= SPF_HOP_PROFILE_COUNT) {
				ret = -EBADMSG; break;
			}
			if (scheduler->adaptive_request.policy.mode == SPF_HOP_ADAPTIVE)
				profile = (uint8_t)adaptive_event.choice.proposed_target;
		}

		memset(&transition, 0, sizeof(transition));
		ret = scheduler->io.recall(scheduler->io_context,
			scheduler->request.profiles[profile].fastlock_slot,
			scheduler->request.profiles[profile].lo_frequency_hz,
			&transition);
		if (ret) {
			ret = ret > 0 ? -EIO : ret;
			break;
		}
		if (transition.transition_before > transition.transition_after ||
			transition.actual_lo_frequency_hz !=
				scheduler->request.profiles[profile].lo_frequency_hz ||
			transition.active_profile !=
				scheduler->request.profiles[profile].fastlock_slot ||
			!transition.device_event_id) {
			ret = -EBADMSG;
			break;
		}
		memset(&event, 0, sizeof(event));
		event.event_sequence = index;
		event.dwell_index = index;
		event.transition_before = transition.transition_before;
		event.transition_after = transition.transition_after;
		event.actual_lo_frequency_hz = transition.actual_lo_frequency_hz;
		event.actual_if_offset_hz = scheduler->request.if_offset_hz;
		event.device_event_id = transition.device_event_id;
		event.from_profile = previous_profile;
		event.to_profile = profile;
		event.kind = index ? SPF_HOP_EVENT_RETUNE : SPF_HOP_EVENT_STARTUP;
		event.flags = SPF_HOP_EVENT_FLAGS_V1;
		event.fastlock_slot =
			scheduler->request.profiles[profile].fastlock_slot;
		if (transition.transition_after > UINT64_MAX -
				scheduler->request.transition_guard_samples ||
			transition.transition_after +
				scheduler->request.transition_guard_samples >
				UINT64_MAX - scheduler->request.dwell_samples) {
			ret = -EOVERFLOW;
			break;
		}
		next_invalid_start = transition.transition_after +
			scheduler->request.transition_guard_samples +
			scheduler->request.dwell_samples;
		adaptive_event.device = event;
		if (scheduler->adaptive) {
			struct spf_hop_event_v1 checked = {0};
			checked.device = event;
			ret = spf_hop_choice_v2_validate(&adaptive_event.choice, &checked);
			if (ret) break;
			ret = scheduler->policy.commit(scheduler->policy_context, &adaptive_event,
				transition.transition_after + scheduler->request.transition_guard_samples,
				next_invalid_start);
			if (ret) { ret = ret > 0 ? -EIO : ret; break; }
		}
		ret = enqueue_event(scheduler, &adaptive_event);
		if (ret) break;
		previous_profile = profile;
		if (!index) startup_invalid_start = transition.transition_before;
	}
	if (!ret && index == scheduler->request.dwell_count &&
		(next_invalid_start < startup_invalid_start ||
		 next_invalid_start - startup_invalid_start <
			scheduler->request.capture_span_samples))
		ret = -ERANGE;
	finish_worker(scheduler, ret);
	return NULL;
}

static int scheduler_submit(void *opaque,
	const struct spf_hop_request_v1 *request)
{
	struct spf_hop_scheduler_v1 *scheduler = opaque;
	int ret;

	if (!scheduler || !request || !request_matches(request,
			&scheduler->request))
		return -EINVAL;
	if (scheduler->thread_started || scheduler->restore_called)
		return -EBUSY;
	ret = scheduler->io.start(scheduler->io_context,
		scheduler->request.profiles[0].lo_frequency_hz);
	if (ret)
		return ret > 0 ? -EIO : ret;
	ret = pthread_create(&scheduler->thread, NULL, scheduler_worker,
		scheduler);
	if (ret)
		return -ret;
	scheduler->thread_started = true;
	return 0;
}

static int scheduler_drain(void *opaque,
	struct spf_hop_device_event_v1 *events, size_t capacity,
	size_t *event_count, uint64_t *dropped_events)
{
	struct spf_hop_scheduler_v1 *scheduler = opaque;
	size_t count;
	size_t i;
	int error;

	if (!scheduler || (!events && capacity) || !event_count ||
		!dropped_events)
		return -EINVAL;
	pthread_mutex_lock(&scheduler->lock);
	error = scheduler->worker_error;
	count = scheduler->queue_count < capacity ? scheduler->queue_count :
		capacity;
	for (i = 0; i < count; i++)
		events[i] = scheduler->queue[(scheduler->queue_head + i) %
			SPF_HOP_SCHEDULER_QUEUE_CAPACITY].device;
	scheduler->queue_head = (scheduler->queue_head + count) %
		SPF_HOP_SCHEDULER_QUEUE_CAPACITY;
	scheduler->queue_count -= count;
	*event_count = count;
	*dropped_events = scheduler->dropped_events;
	pthread_mutex_unlock(&scheduler->lock);
	return error;
}

static int scheduler_cancel_restore(void *opaque, uint16_t reason,
	struct spf_hop_restore_receipt_v1 *receipt)
{
	struct spf_hop_scheduler_v1 *scheduler = opaque;
	struct spf_hop_scheduler_restore_v1 restored;
	bool join_thread;
	int ret;

	(void)reason;
	if (!scheduler || !receipt)
		return -EINVAL;
	pthread_mutex_lock(&scheduler->lock);
	if (scheduler->restore_called) {
		*receipt = scheduler->restore_receipt;
		ret = scheduler->restore_error;
		pthread_mutex_unlock(&scheduler->lock);
		return ret;
	}
	scheduler->cancel_requested = true;
	join_thread = scheduler->thread_started;
	pthread_mutex_unlock(&scheduler->lock);
	if (join_thread)
		(void)pthread_join(scheduler->thread, NULL);

	memset(&restored, 0, sizeof(restored));
	ret = scheduler->io.restore(scheduler->io_context,
		scheduler->request.profiles[0].lo_frequency_hz, &restored);
	ret = ret > 0 ? -EIO : ret;
	memset(receipt, 0, sizeof(*receipt));
	receipt->transition_before = restored.transition_before;
	receipt->transition_after = restored.transition_after;
	receipt->restored_lo_frequency_hz = restored.actual_lo_frequency_hz;
	receipt->restored_profile = SPF_HOP_PROFILE_NONE;
	receipt->flags = SPF_HOP_EVENT_FLAGS_V1;
	receipt->error_code = ret;
	if (!ret && (restored.transition_before > restored.transition_after ||
		restored.actual_lo_frequency_hz !=
			scheduler->request.profiles[0].lo_frequency_hz ||
		restored.active_profile != UINT32_MAX)) {
		ret = -ESTALE;
		receipt->error_code = ret;
	}
	pthread_mutex_lock(&scheduler->lock);
	scheduler->restore_called = true;
	scheduler->restore_error = ret;
	scheduler->restore_receipt = *receipt;
	pthread_mutex_unlock(&scheduler->lock);
	return ret;
}

static const struct spf_hop_device_ops_v1 scheduler_ops = {
	.submit_plan = scheduler_submit,
	.drain_events = scheduler_drain,
	.cancel_restore = scheduler_cancel_restore,
};

int spf_hop_scheduler_v1_create(const struct spf_hop_request_v1 *request,
	const struct spf_hop_scheduler_io_v1 *io, void *io_context,
	void **device_context, const struct spf_hop_device_ops_v1 **ops)
{
	struct spf_hop_scheduler_v1 *scheduler;
	uint8_t wire[SPF_HOP_REQUEST_BYTES];
	int ret;

	if (!request || !io || !io->start || !io->get_counter || !io->recall ||
		!io->restore || !io->sleep_ns || !device_context || !ops)
		return -EINVAL;
	ret = spf_hop_request_v1_encode(wire, sizeof(wire), request);
	if (ret)
		return ret;
	scheduler = calloc(1, sizeof(*scheduler));
	if (!scheduler)
		return -ENOMEM;
	ret = pthread_mutex_init(&scheduler->lock, NULL);
	if (ret) {
		free(scheduler);
		return -ret;
	}
	scheduler->request = *request;
	scheduler->io = *io;
	scheduler->io_context = io_context;
	*device_context = scheduler;
	*ops = &scheduler_ops;
	return 0;
}

void spf_hop_scheduler_v1_destroy(void *device_context)
{
	struct spf_hop_scheduler_v1 *scheduler = device_context;
	struct spf_hop_restore_receipt_v1 ignored;

	if (!scheduler)
		return;
	if (scheduler->thread_started && !scheduler->restore_called)
		(void)scheduler_cancel_restore(scheduler,
			SPF_HOP_REASON_CLIENT_DISCONNECT, &ignored);
	if (scheduler->io.destroy)
		scheduler->io.destroy(scheduler->io_context);
	pthread_mutex_destroy(&scheduler->lock);
	free(scheduler);
}

static int scheduler_submit_v2(void *opaque, const struct spf_hop_request_v2 *request)
{
	struct spf_hop_scheduler_v1 *s = opaque;
	uint8_t actual[SPF_HOP_HOST_REQUEST_BYTES], expected[SPF_HOP_HOST_REQUEST_BYTES];
	if (!s || !s->adaptive || !request ||
		spf_hop_adaptive_configuration(actual, sizeof(actual), request) ||
		spf_hop_adaptive_configuration(expected, sizeof(expected), &s->adaptive_request) ||
		memcmp(actual, expected, sizeof(actual))) return -EINVAL;
	return scheduler_submit(s, &request->geometry);
}

static int scheduler_drain_v2(void *opaque, struct spf_hop_device_event_v2 *events,
	size_t capacity, size_t *count, uint64_t *dropped)
{
	struct spf_hop_scheduler_v1 *s = opaque;
	size_t i, n;
	int ret;
	if (!s || !s->adaptive || (!events && capacity) || !count || !dropped) return -EINVAL;
	pthread_mutex_lock(&s->lock);
	n = s->queue_count < capacity ? s->queue_count : capacity;
	for (i = 0; i < n; ++i)
		events[i] = s->queue[(s->queue_head + i) % SPF_HOP_SCHEDULER_QUEUE_CAPACITY];
	s->queue_head = (s->queue_head + n) % SPF_HOP_SCHEDULER_QUEUE_CAPACITY;
	s->queue_count -= n;
	*count = n;
	*dropped = s->dropped_events;
	ret = s->worker_error;
	pthread_mutex_unlock(&s->lock);
	return ret;
}

static const struct spf_hop_device_ops_v2 scheduler_ops_v2 = {
	.submit_plan = scheduler_submit_v2, .drain_events = scheduler_drain_v2,
	.cancel_restore = scheduler_cancel_restore,
};

int spf_hop_scheduler_v2_create(const struct spf_hop_request_v2 *request,
	const struct spf_hop_scheduler_io_v1 *io, void *io_context,
	const struct spf_hop_scheduler_policy_v2 *policy, void *policy_context,
	void **context, const struct spf_hop_device_ops_v2 **ops)
{
	uint8_t wire[SPF_HOP_HOST_REQUEST_BYTES];
	const struct spf_hop_device_ops_v1 *unused;
	struct spf_hop_scheduler_v1 *s;
	void *created;
	int ret;
	if (!request || !policy || !policy->choose || !policy->commit || !context || !ops)
		return -EINVAL;
	ret = spf_hop_adaptive_configuration(wire, sizeof(wire), request);
	if (ret) return ret;
	ret = spf_hop_scheduler_v1_create(&request->geometry, io, io_context, &created, &unused);
	if (ret) return ret;
	s = created;
	s->adaptive = true;
	s->adaptive_request = *request;
	s->policy = *policy;
	s->policy_context = policy_context;
	*context = s;
	*ops = &scheduler_ops_v2;
	return 0;
}

void spf_hop_scheduler_v2_destroy(void *context)
{ spf_hop_scheduler_v1_destroy(context); }
