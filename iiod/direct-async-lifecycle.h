#ifndef IIOD_DIRECT_ASYNC_LIFECYCLE_H
#define IIOD_DIRECT_ASYNC_LIFECYCLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool iiod_direct_async_segment_is_complete(
		bool requested, bool producer_started, bool producer_exited,
		bool consumer_active, bool cancelled, size_t queued_frames,
		uint64_t consumed_frames, uint64_t target_frames, int error)
{
	return requested && producer_started && producer_exited &&
		!consumer_active && !cancelled && !queued_frames && !error &&
		target_frames && consumed_frames == target_frames;
}

#endif
