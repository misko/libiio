/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-buffer-layout.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

static void expect_layout(uint32_t mask, size_t samples, size_t scan_bytes,
	uint8_t receivers, uint8_t extra_samples, size_t iq_bytes,
	uint32_t timestamp_words)
{
	struct spf_buffer_layout layout;

	assert(spf_buffer_layout_resolve(samples, &mask, 1, scan_bytes,
		&layout) == 0);
	assert(layout.enabled_scan_mask == mask);
	assert(layout.receiver_count == receivers);
	assert(layout.scan_bytes == scan_bytes);
	assert(layout.extra_samples == extra_samples);
	assert(layout.samples_per_channel == samples);
	assert(layout.iq_bytes == iq_bytes);
	assert(layout.raw_bytes == iq_bytes + 8U);
	assert(layout.timestamp_words == timestamp_words);
}

int main(void)
{
	struct spf_buffer_layout layout;
	struct spf_buffer_sequence_state state = {0};
	struct spf_buffer_sequence_result sequence;
	uint32_t mask;

	expect_layout(UINT32_C(0x03), 1024, 4, 1, 2, 4096, 512);
	expect_layout(UINT32_C(0x0c), 524288, 4, 1, 2, 2097152, 262144);
	expect_layout(UINT32_C(0x0f), 1023, 8, 2, 1, 8184, 1023);

	mask = UINT32_C(0x03);
	assert(spf_buffer_layout_resolve(1023, &mask, 1, 4, &layout) == -EINVAL);
	assert(spf_buffer_layout_resolve(1024, &mask, 1, 8, &layout) == -EINVAL);
	mask = UINT32_C(0x0c);
	assert(spf_buffer_layout_resolve(1024, &mask, 2, 4, &layout) == -EINVAL);
	mask = UINT32_C(0x01);
	assert(spf_buffer_layout_resolve(1024, &mask, 1, 2, &layout) == -EINVAL);
	mask = UINT32_C(0x05);
	assert(spf_buffer_layout_resolve(1024, &mask, 1, 4, &layout) == -EINVAL);
	mask = UINT32_C(0x10f);
	assert(spf_buffer_layout_resolve(1024, &mask, 1, 8, &layout) == -EINVAL);
	mask = UINT32_C(0x0f);
	assert(spf_buffer_layout_resolve((size_t)(UINT32_MAX >> 1) + 1U,
		&mask, 1, 8, &layout) == -E2BIG);

	assert(spf_buffer_sequence_resolve(&state, UINT64_C(1000), 100,
		&sequence) == 0);
	assert(sequence.buffer_sequence == 0 &&
		sequence.missing_samples_before == 0 && sequence.frame_end == 1100);
	spf_buffer_sequence_commit(&state, &sequence);
	assert(spf_buffer_sequence_resolve(&state, UINT64_C(1100), 100,
		&sequence) == 0);
	assert(sequence.buffer_sequence == 1 &&
		sequence.missing_samples_before == 0);
	spf_buffer_sequence_commit(&state, &sequence);
	assert(spf_buffer_sequence_resolve(&state, UINT64_C(1201), 100,
		&sequence) == 0);
	assert(sequence.buffer_sequence == 2 &&
		sequence.missing_samples_before == 1);
	assert(spf_buffer_sequence_require_contiguous(&sequence) == -EOVERFLOW);
	spf_buffer_sequence_commit(&state, &sequence);
	assert(spf_buffer_sequence_resolve(&state, UINT64_C(1501), 100,
		&sequence) == 0);
	assert(sequence.buffer_sequence == 5 &&
		sequence.missing_samples_before == 200);
	assert(spf_buffer_sequence_require_contiguous(&sequence) == -EOVERFLOW);
	sequence.missing_samples_before = 0;
	assert(spf_buffer_sequence_require_contiguous(&sequence) == 0);
	assert(spf_buffer_sequence_require_contiguous(NULL) == -EINVAL);
	assert(spf_buffer_sequence_resolve(&state, UINT64_C(1200), 100,
		&sequence) == -ERANGE);
	assert(spf_buffer_sequence_resolve(&state, UINT64_MAX - 99U, 100,
		&sequence) == -EINVAL);
	state.previous_buffer_sequence = UINT64_MAX;
	assert(spf_buffer_sequence_resolve(&state, UINT64_C(1301), 100,
		&sequence) == -EOVERFLOW);
	return 0;
}
