/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_DDR_RING_CORE_H__
#define __IIOD_DDR_RING_CORE_H__

#include "spf-ddr-ring-status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum iiod_ddr_ring_slot_state {
	IIOD_DDR_RING_SLOT_FREE = 0,
	IIOD_DDR_RING_SLOT_PRODUCER,
	IIOD_DDR_RING_SLOT_COMMITTED,
	IIOD_DDR_RING_SLOT_CONSUMER,
};

struct iiod_ddr_ring_core {
	enum iiod_ddr_ring_slot_state *slots;
	size_t slot_count;
	size_t producer_position;
	size_t consumer_position;
	size_t occupied;
	uint64_t target_frames;
	uint64_t produced_frames;
	uint64_t consumed_frames;
	uint64_t high_water_frames;
	uint64_t wrap_count;
	uint32_t state;
	uint32_t terminal_reason;
	int error_code;
	bool producer_reserved;
	bool consumer_reserved;
};

int iiod_ddr_ring_core_init(struct iiod_ddr_ring_core *ring,
	enum iiod_ddr_ring_slot_state *slots, size_t slot_count,
	uint64_t target_frames);
int iiod_ddr_ring_core_start(struct iiod_ddr_ring_core *ring);
int iiod_ddr_ring_core_producer_reserve(struct iiod_ddr_ring_core *ring,
	size_t *slot);
int iiod_ddr_ring_core_producer_commit(struct iiod_ddr_ring_core *ring);
int iiod_ddr_ring_core_producer_abort(struct iiod_ddr_ring_core *ring);
int iiod_ddr_ring_core_consumer_reserve(struct iiod_ddr_ring_core *ring,
	size_t *slot);
int iiod_ddr_ring_core_consumer_release(struct iiod_ddr_ring_core *ring);
int iiod_ddr_ring_core_fail(struct iiod_ddr_ring_core *ring,
	uint32_t reason, int error_code);
int iiod_ddr_ring_core_cancel(struct iiod_ddr_ring_core *ring,
	uint32_t reason);
bool iiod_ddr_ring_core_is_terminal(const struct iiod_ddr_ring_core *ring);

#endif
