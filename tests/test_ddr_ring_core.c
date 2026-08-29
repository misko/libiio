#include "ddr-ring-core.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stddef.h>

static void produce(struct iiod_ddr_ring_core *ring, size_t expected)
{
	size_t slot = SIZE_MAX;
	assert(iiod_ddr_ring_core_producer_reserve(ring, &slot) == 0);
	assert(slot == expected);
	assert(iiod_ddr_ring_core_producer_commit(ring) == 0);
}

static void consume(struct iiod_ddr_ring_core *ring, size_t expected)
{
	size_t slot = SIZE_MAX;
	assert(iiod_ddr_ring_core_consumer_reserve(ring, &slot) == 0);
	assert(slot == expected);
	assert(iiod_ddr_ring_core_consumer_release(ring) == 0);
}

static void test_finite_wrap(void)
{
	enum iiod_ddr_ring_slot_state slots[3];
	struct iiod_ddr_ring_core ring;
	size_t slot;

	assert(iiod_ddr_ring_core_init(&ring, slots, 3, 8) == 0);
	assert(iiod_ddr_ring_core_start(&ring) == 0);
	produce(&ring, 0);
	produce(&ring, 1);
	produce(&ring, 2);
	assert(ring.high_water_frames == 3 && ring.wrap_count == 1);
	assert(iiod_ddr_ring_core_producer_reserve(&ring, &slot) == -EAGAIN);
	consume(&ring, 0);
	produce(&ring, 0);
	consume(&ring, 1);
	produce(&ring, 1);
	consume(&ring, 2);
	produce(&ring, 2);
	consume(&ring, 0);
	produce(&ring, 0);
	consume(&ring, 1);
	produce(&ring, 1);
	assert(ring.state == SPF_DDR_RING_STATE_DRAINING);
	assert(iiod_ddr_ring_core_producer_reserve(&ring, &slot) == -ESHUTDOWN);
	consume(&ring, 2);
	consume(&ring, 0);
	consume(&ring, 1);
	assert(ring.state == SPF_DDR_RING_STATE_COMPLETE);
	assert(ring.terminal_reason == SPF_DDR_RING_REASON_TARGET_COMPLETE);
	assert(ring.produced_frames == 8 && ring.consumed_frames == 8);
	assert(iiod_ddr_ring_core_is_terminal(&ring));
}

static void test_failure_drains_committed(void)
{
	enum iiod_ddr_ring_slot_state slots[2];
	struct iiod_ddr_ring_core ring;
	size_t slot;

	assert(iiod_ddr_ring_core_init(&ring, slots, 2, 0) == 0);
	assert(iiod_ddr_ring_core_start(&ring) == 0);
	produce(&ring, 0);
	produce(&ring, 1);
	assert(iiod_ddr_ring_core_fail(&ring, SPF_DDR_RING_REASON_COUNTER_GAP,
		-EOVERFLOW) == 0);
	assert(!iiod_ddr_ring_core_is_terminal(&ring));
	assert(iiod_ddr_ring_core_producer_reserve(&ring, &slot) == -ESHUTDOWN);
	consume(&ring, 0);
	consume(&ring, 1);
	assert(iiod_ddr_ring_core_is_terminal(&ring));
	assert(iiod_ddr_ring_core_consumer_reserve(&ring, &slot) == -EOVERFLOW);
}

static void test_abort_and_cancel(void)
{
	enum iiod_ddr_ring_slot_state slots[1];
	struct iiod_ddr_ring_core ring;
	size_t slot;

	assert(iiod_ddr_ring_core_init(&ring, slots, 1, 0) == 0);
	assert(iiod_ddr_ring_core_start(&ring) == 0);
	assert(iiod_ddr_ring_core_producer_reserve(&ring, &slot) == 0);
	assert(iiod_ddr_ring_core_producer_abort(&ring) == 0);
	assert(slots[0] == IIOD_DDR_RING_SLOT_FREE);
	assert(iiod_ddr_ring_core_cancel(&ring,
		SPF_DDR_RING_REASON_CLIENT_DISCONNECTED) == 0);
	assert(iiod_ddr_ring_core_is_terminal(&ring));
	assert(iiod_ddr_ring_core_producer_reserve(&ring, &slot) == -ESHUTDOWN);
}

int main(void)
{
	test_finite_wrap();
	test_failure_drains_committed();
	test_abort_and_cancel();
	return 0;
}
