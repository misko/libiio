/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_BUFFER_LAYOUT_H__
#define __SPF_BUFFER_LAYOUT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spf_buffer_layout {
	uint32_t enabled_scan_mask;
	uint8_t receiver_count;
	uint8_t scan_bytes;
	uint8_t extra_samples;
	uint32_t samples_per_channel;
	uint32_t iq_bytes_per_sample;
	uint32_t timestamp_words;
	size_t iq_bytes;
	size_t raw_bytes;
};

struct spf_buffer_sequence_state {
	uint64_t previous_frame_end;
	uint64_t previous_buffer_sequence;
	bool valid;
};

struct spf_buffer_sequence_result {
	uint64_t buffer_sequence;
	uint64_t missing_samples_before;
	uint64_t frame_end;
};

int spf_buffer_layout_resolve(size_t samples_count, const uint32_t *mask,
	size_t words, size_t scan_bytes, struct spf_buffer_layout *layout);
int spf_buffer_sequence_resolve(const struct spf_buffer_sequence_state *state,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	struct spf_buffer_sequence_result *result);
int spf_buffer_sequence_require_contiguous(
	const struct spf_buffer_sequence_result *result);
void spf_buffer_sequence_commit(struct spf_buffer_sequence_state *state,
	const struct spf_buffer_sequence_result *result);

#endif
