/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-buffer-layout.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int spf_buffer_layout_resolve(size_t samples_count, const uint32_t *mask,
	size_t words, size_t scan_bytes, struct spf_buffer_layout *layout)
{
	uint8_t receiver_count;
	uint8_t expected_scan_bytes;
	size_t iq_bytes;

	if (!mask || !layout || words != 1 || samples_count == 0 ||
		samples_count > UINT32_MAX)
		return -EINVAL;
	switch (mask[0]) {
	case UINT32_C(0x03):
	case UINT32_C(0x0c):
		receiver_count = UINT8_C(1);
		expected_scan_bytes = UINT8_C(4);
		if ((samples_count & 1U) != 0)
			return -EINVAL;
		break;
	case UINT32_C(0x0f):
		receiver_count = UINT8_C(2);
		expected_scan_bytes = UINT8_C(8);
		break;
	default:
		return -EINVAL;
	}
	if (scan_bytes != expected_scan_bytes ||
		samples_count > (SIZE_MAX - 8U) / scan_bytes)
		return -EINVAL;
	iq_bytes = samples_count * scan_bytes;
	if ((iq_bytes & 7U) != 0 || iq_bytes / 8U > (UINT32_MAX >> 1))
		return -E2BIG;

	memset(layout, 0, sizeof(*layout));
	layout->enabled_scan_mask = mask[0];
	layout->receiver_count = receiver_count;
	layout->scan_bytes = expected_scan_bytes;
	layout->extra_samples = UINT8_C(8) / expected_scan_bytes;
	layout->samples_per_channel = (uint32_t)samples_count;
	layout->iq_bytes_per_sample = expected_scan_bytes;
	layout->timestamp_words = (uint32_t)(iq_bytes / 8U);
	/* Metadata capture owns timestamp insertion for the buffer lifetime. */
	layout->timestamp_control = (layout->timestamp_words << 1) | UINT32_C(1);
	layout->iq_bytes = iq_bytes;
	layout->raw_bytes = iq_bytes + 8U;
	return 0;
}

int spf_buffer_sequence_resolve(const struct spf_buffer_sequence_state *state,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	struct spf_buffer_sequence_result *result)
{
	uint64_t missing = 0;
	uint64_t sequence = 0;

	if (!state || !result || samples_per_channel == 0 ||
		first_sample_sequence > UINT64_MAX - samples_per_channel)
		return -EINVAL;
	if (state->valid) {
		uint64_t skipped_refills;
		if (first_sample_sequence < state->previous_frame_end)
			return -ERANGE;
		missing = first_sample_sequence - state->previous_frame_end;
		skipped_refills = missing / samples_per_channel;
		if (state->previous_buffer_sequence >
				UINT64_MAX - UINT64_C(1) - skipped_refills)
			return -EOVERFLOW;
		sequence = state->previous_buffer_sequence + UINT64_C(1) +
			skipped_refills;
	}
	result->buffer_sequence = sequence;
	result->missing_samples_before = missing;
	result->frame_end = first_sample_sequence + samples_per_channel;
	return 0;
}

void spf_buffer_sequence_commit(struct spf_buffer_sequence_state *state,
	const struct spf_buffer_sequence_result *result)
{
	if (!state || !result)
		return;
	state->previous_frame_end = result->frame_end;
	state->previous_buffer_sequence = result->buffer_sequence;
	state->valid = true;
}
