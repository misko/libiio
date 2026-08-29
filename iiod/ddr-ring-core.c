/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ddr-ring-core.h"

#include <errno.h>
#include <string.h>

static bool state_allows_drain(uint32_t state)
{
	return state == SPF_DDR_RING_STATE_RUNNING ||
		state == SPF_DDR_RING_STATE_DRAINING ||
		state == SPF_DDR_RING_STATE_FAILED;
}

int iiod_ddr_ring_core_init(struct iiod_ddr_ring_core *ring,
	enum iiod_ddr_ring_slot_state *slots, size_t slot_count,
	uint64_t target_frames)
{
	if (!ring || !slots || !slot_count)
		return -EINVAL;
	memset(ring, 0, sizeof(*ring));
	memset(slots, 0, slot_count * sizeof(*slots));
	ring->slots = slots;
	ring->slot_count = slot_count;
	ring->target_frames = target_frames;
	ring->prefill_frames = target_frames && target_frames < slot_count ?
		(size_t)target_frames : slot_count;
	ring->low_water_frames = target_frames && target_frames <= slot_count ?
		0 : slot_count / 2U;
	ring->state = SPF_DDR_RING_STATE_RESERVED;
	return 0;
}

int iiod_ddr_ring_core_start(struct iiod_ddr_ring_core *ring)
{
	if (!ring || ring->state != SPF_DDR_RING_STATE_RESERVED)
		return -EINVAL;
	ring->state = SPF_DDR_RING_STATE_RUNNING;
	return 0;
}

int iiod_ddr_ring_core_producer_reserve(struct iiod_ddr_ring_core *ring,
	size_t *slot)
{
	if (!ring || !slot)
		return -EINVAL;
	if (ring->state != SPF_DDR_RING_STATE_RUNNING)
		return -ESHUTDOWN;
	if (ring->producer_reserved)
		return -EBUSY;
	if (ring->target_frames && ring->produced_frames >= ring->target_frames)
		return -ENODATA;
	if (ring->slots[ring->producer_position] != IIOD_DDR_RING_SLOT_FREE)
		return -EAGAIN;
	ring->slots[ring->producer_position] = IIOD_DDR_RING_SLOT_PRODUCER;
	ring->producer_reserved = true;
	*slot = ring->producer_position;
	return 0;
}

int iiod_ddr_ring_core_producer_commit(struct iiod_ddr_ring_core *ring)
{
	size_t slot;

	if (!ring || !ring->producer_reserved)
		return -EINVAL;
	slot = ring->producer_position;
	if (ring->slots[slot] != IIOD_DDR_RING_SLOT_PRODUCER)
		return -EIO;
	ring->slots[slot] = IIOD_DDR_RING_SLOT_COMMITTED;
	ring->producer_reserved = false;
	ring->occupied++;
	ring->produced_frames++;
	if (ring->occupied > ring->high_water_frames)
		ring->high_water_frames = ring->occupied;
	ring->producer_position++;
	if (ring->producer_position == ring->slot_count) {
		ring->producer_position = 0;
		ring->wrap_count++;
	}
	if (ring->target_frames && ring->produced_frames == ring->target_frames)
		ring->state = SPF_DDR_RING_STATE_DRAINING;
	return 0;
}

int iiod_ddr_ring_core_producer_abort(struct iiod_ddr_ring_core *ring)
{
	if (!ring || !ring->producer_reserved)
		return -EINVAL;
	if (ring->slots[ring->producer_position] != IIOD_DDR_RING_SLOT_PRODUCER)
		return -EIO;
	ring->slots[ring->producer_position] = IIOD_DDR_RING_SLOT_FREE;
	ring->producer_reserved = false;
	return 0;
}

int iiod_ddr_ring_core_consumer_reserve(struct iiod_ddr_ring_core *ring,
	size_t *slot)
{
	if (!ring || !slot)
		return -EINVAL;
	if (!state_allows_drain(ring->state))
		return ring->state == SPF_DDR_RING_STATE_COMPLETE ? -ENODATA :
			-ESHUTDOWN;
	if (ring->consumer_reserved)
		return -EBUSY;
	if (!ring->occupied) {
		if (ring->state == SPF_DDR_RING_STATE_DRAINING)
			return -ENODATA;
		if (ring->state == SPF_DDR_RING_STATE_FAILED)
			return ring->error_code ? ring->error_code : -EIO;
		return -EAGAIN;
	}
	if (ring->state == SPF_DDR_RING_STATE_RUNNING) {
		if (!ring->consumer_started) {
			if (ring->occupied < ring->prefill_frames)
				return -EAGAIN;
			ring->consumer_started = true;
		} else if (ring->occupied <= ring->low_water_frames) {
			return -EAGAIN;
		}
	}
	if (ring->slots[ring->consumer_position] != IIOD_DDR_RING_SLOT_COMMITTED)
		return -EIO;
	ring->slots[ring->consumer_position] = IIOD_DDR_RING_SLOT_CONSUMER;
	ring->consumer_reserved = true;
	*slot = ring->consumer_position;
	return 0;
}

bool iiod_ddr_ring_core_consumer_ready(
	const struct iiod_ddr_ring_core *ring)
{
	if (!ring || !state_allows_drain(ring->state) || !ring->occupied)
		return false;
	if (ring->state != SPF_DDR_RING_STATE_RUNNING)
		return true;
	if (!ring->consumer_started)
		return ring->occupied >= ring->prefill_frames;
	return ring->occupied > ring->low_water_frames;
}

bool iiod_ddr_ring_core_prefix_complete(
	const struct iiod_ddr_ring_core *ring)
{
	return ring && ring->produced_frames >= ring->prefill_frames;
}

void iiod_ddr_ring_core_mark_unavailable(struct iiod_ddr_ring_core *ring,
	uint64_t observed_first_sample_sequence)
{
	if (!ring || ring->first_unavailable_valid)
		return;
	ring->first_unavailable_sample_sequence = ring->expected_sample_valid ?
		ring->expected_sample_sequence : observed_first_sample_sequence;
	ring->first_unavailable_valid = true;
	ring->continuity_broken = true;
}

int iiod_ddr_ring_core_observe_samples(struct iiod_ddr_ring_core *ring,
	uint64_t first_sample_sequence, uint64_t samples_per_frame)
{
	uint64_t exclusive_boundary;

	if (!ring || !samples_per_frame)
		return -EINVAL;
	if (first_sample_sequence > UINT64_MAX - samples_per_frame)
		return -EOVERFLOW;
	exclusive_boundary = first_sample_sequence + samples_per_frame;
	if (ring->expected_sample_valid &&
			first_sample_sequence != ring->expected_sample_sequence)
		iiod_ddr_ring_core_mark_unavailable(ring, first_sample_sequence);
	if (!ring->continuity_broken) {
		ring->last_contiguous_sample_sequence = exclusive_boundary;
		ring->last_contiguous_valid = true;
	}
	ring->expected_sample_sequence = exclusive_boundary;
	ring->expected_sample_valid = true;
	return 0;
}

int iiod_ddr_ring_core_consumer_release(struct iiod_ddr_ring_core *ring)
{
	size_t slot;

	if (!ring || !ring->consumer_reserved || !ring->occupied)
		return -EINVAL;
	slot = ring->consumer_position;
	if (ring->slots[slot] != IIOD_DDR_RING_SLOT_CONSUMER)
		return -EIO;
	ring->slots[slot] = IIOD_DDR_RING_SLOT_FREE;
	ring->consumer_reserved = false;
	ring->occupied--;
	ring->consumed_frames++;
	ring->consumer_position++;
	if (ring->consumer_position == ring->slot_count)
		ring->consumer_position = 0;
	if (ring->state == SPF_DDR_RING_STATE_DRAINING && !ring->occupied) {
		ring->state = SPF_DDR_RING_STATE_COMPLETE;
		ring->terminal_reason = SPF_DDR_RING_REASON_TARGET_COMPLETE;
	}
	return 0;
}

int iiod_ddr_ring_core_fail(struct iiod_ddr_ring_core *ring,
	uint32_t reason, int error_code)
{
	if (!ring || reason < SPF_DDR_RING_REASON_CONSUMER_STALL ||
		reason > SPF_DDR_RING_REASON_INTERNAL_ERROR || error_code >= 0)
		return -EINVAL;
	if (iiod_ddr_ring_core_is_terminal(ring))
		return -ESHUTDOWN;
	if (ring->producer_reserved)
		(void)iiod_ddr_ring_core_producer_abort(ring);
	ring->state = SPF_DDR_RING_STATE_FAILED;
	ring->terminal_reason = reason;
	ring->error_code = error_code;
	return 0;
}

int iiod_ddr_ring_core_cancel(struct iiod_ddr_ring_core *ring,
	uint32_t reason)
{
	if (!ring || (reason != SPF_DDR_RING_REASON_CLIENT_CANCELLED &&
		reason != SPF_DDR_RING_REASON_CLIENT_DISCONNECTED))
		return -EINVAL;
	if (iiod_ddr_ring_core_is_terminal(ring))
		return -ESHUTDOWN;
	if (ring->producer_reserved)
		(void)iiod_ddr_ring_core_producer_abort(ring);
	ring->state = SPF_DDR_RING_STATE_CANCELLED;
	ring->terminal_reason = reason;
	ring->error_code = -ECANCELED;
	return 0;
}

bool iiod_ddr_ring_core_is_terminal(const struct iiod_ddr_ring_core *ring)
{
	return ring && (ring->state == SPF_DDR_RING_STATE_COMPLETE ||
		ring->state == SPF_DDR_RING_STATE_CANCELLED ||
		(ring->state == SPF_DDR_RING_STATE_FAILED && !ring->occupied &&
			!ring->consumer_reserved));
}
