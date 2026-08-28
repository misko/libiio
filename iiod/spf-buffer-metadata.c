/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "buffer-metadata.h"

#include <spf_gain_metadata.h>
#include <spf_gain_read.h>
#include <spf_gain_sampler.h>
#include <spf_radio_frame_v3.h>
#include <spf_rssi_read.h>

#include <errno.h>
#include <iio.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SPF_IIOD_SCAN_MASK UINT32_C(0x0f)
#define SPF_IIOD_OBSERVATION_CAPACITY UINT16_C(64)
#define SPF_IIOD_OBSERVATION_INTERVAL_MAX UINT32_C(32768)
#define SPF_IIOD_OBSERVATION_INTERVAL_MIN UINT32_C(1024)
/* Sampler start/finish are short control-plane fences, independent of the
 * longer DMA block-dequeue bound enforced by iiod/ops.c.
 */
#define SPF_IIOD_SAMPLER_SYNC_TIMEOUT_MS UINT32_C(100)

struct spf_iiod_metadata_context {
	struct iio_device *rx;
	struct iio_device *phy;
	spf_gain_sampler_t sampler;
	uint32_t timestamp_control_previous;
	uint32_t samples_per_channel;
	uint32_t observation_interval_samples;
	uint64_t stream_id;
	uint64_t stream_first_sample_sequence;
	uint64_t frames_emitted;
	uint64_t refills_started;
	uint32_t startup_frames_discarded;
	bool stream_origin_valid;
	bool sampler_started;
	bool timestamp_configured;
};

static uint64_t make_stream_id(const void *address)
{
	struct timespec now = {0, 0};
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t value = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
	value ^= (uint64_t)(uintptr_t)address;
	value ^= (uint64_t)(unsigned int)getpid() << 32;
	return value ? value : UINT64_C(1);
}

static uint64_t monotonic_now_ns(void)
{
	struct timespec now = {0, 0};
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
}

static int sampler_sync_failed(const struct spf_iiod_metadata_context *ctx,
		const char *phase, uint64_t started_ns)
{
	const uint64_t finished_ns = monotonic_now_ns();
	const uint64_t elapsed_us = started_ns && finished_ns >= started_ns ?
		(finished_ns - started_ns) / UINT64_C(1000) : 0;

	fprintf(stderr,
		"iiod metadata sampler %s synchronization failed: refill=%" PRIu64
		" samples_per_channel=%u elapsed_us=%" PRIu64 " deadline_ms=%u\n",
		phase, ctx->refills_started, ctx->samples_per_channel, elapsed_us,
		SPF_IIOD_SAMPLER_SYNC_TIMEOUT_MS);
	fflush(stderr);
	return -ETIMEDOUT;
}

static uint32_t observation_interval(size_t samples_count)
{
	uint32_t interval = (uint32_t)(samples_count / 4U);
	if (interval < SPF_IIOD_OBSERVATION_INTERVAL_MIN)
		interval = SPF_IIOD_OBSERVATION_INTERVAL_MIN;
	if (interval > SPF_IIOD_OBSERVATION_INTERVAL_MAX)
		interval = SPF_IIOD_OBSERVATION_INTERVAL_MAX;
	return interval;
}

int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		void **provider_context, size_t *extra_samples)
{
	struct spf_iiod_metadata_context *ctx;
	const struct iio_context *iio_ctx;
	uint32_t timestamp_control;

	if (!dev || !mask || words != 1 || mask[0] != SPF_IIOD_SCAN_MASK ||
		!provider_context || !extra_samples || samples_count == 0 ||
		samples_count > (UINT32_MAX >> 1))
		return -EINVAL;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->rx = (struct iio_device *)dev;
	iio_ctx = iio_device_get_context(dev);
	ctx->phy = iio_context_find_device(iio_ctx, "ad9361-phy");
	if (!ctx->phy || !spf_gain_is_full_table_mode(ctx->phy) ||
		!spf_gain_is_digital_gain_disabled(ctx->phy)) {
		free(ctx);
		return -ENOTSUP;
	}

	if (iio_device_reg_read(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
			&ctx->timestamp_control_previous) != 0) {
		free(ctx);
		return -EIO;
	}
	timestamp_control = ((uint32_t)samples_count << 1) |
		(ctx->timestamp_control_previous & UINT32_C(1));
	if (iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
			timestamp_control) != 0) {
		free(ctx);
		return -EIO;
	}
	ctx->timestamp_configured = true;
	ctx->samples_per_channel = (uint32_t)samples_count;
	ctx->observation_interval_samples = observation_interval(samples_count);
	ctx->stream_id = make_stream_id(ctx);
	if (!spf_gain_sampler_start(&ctx->sampler,
			ctx->observation_interval_samples)) {
		(void)iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
				ctx->timestamp_control_previous);
		free(ctx);
		return -EIO;
	}
	ctx->sampler_started = true;

	*provider_context = ctx;
	*extra_samples = 1;
	return 0;
}

int iiod_buffer_metadata_buffer_opened(void *provider_context,
		unsigned int kernel_buffers_count)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	uint64_t initial_credit;
	uint64_t observations_per_frame;

	if (!ctx || !kernel_buffers_count)
		return -EINVAL;
	observations_per_frame =
		(ctx->samples_per_channel + ctx->observation_interval_samples - 1U) /
		ctx->observation_interval_samples;
	/* One additional window covers buffer-arm latency without retaining an
	 * unbounded observation history after the kernel queue becomes full.
	 */
	if (observations_per_frame * ((uint64_t)kernel_buffers_count + 1U) >
			SPF_GAIN_SAMPLER_RING_CAPACITY)
		return -E2BIG;
	initial_credit = (uint64_t)ctx->samples_per_channel *
		((uint64_t)kernel_buffers_count + 1U);
	ctx->refills_started = 0;
	spf_gain_sampler_limit(&ctx->sampler, initial_credit);
	return 0;
}

int iiod_buffer_metadata_before_refill(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	uint64_t started_ns;

	if (!ctx)
		return -EINVAL;
	/* The first dequeue consumes an already queued block. Every later refill
	 * re-enqueues exactly one consumed block before dequeuing another. Reset,
	 * rather than accumulate, one frame plus one bounded arm-safety window.
	 */
	if (ctx->refills_started != 0) {
		started_ns = monotonic_now_ns();
		if (!spf_gain_sampler_limit_and_wait_started(&ctx->sampler,
				(uint64_t)ctx->samples_per_channel * 2U,
				SPF_IIOD_SAMPLER_SYNC_TIMEOUT_MS))
			return sampler_sync_failed(ctx, "start", started_ns);
	}
	ctx->refills_started++;
	return 0;
}

int iiod_buffer_metadata_after_refill(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	uint64_t started_ns;

	if (!ctx)
		return -EINVAL;
	if (ctx->refills_started <= 1)
		return 0;
	started_ns = monotonic_now_ns();
	if (!spf_gain_sampler_finish_capture(
			&ctx->sampler, SPF_IIOD_SAMPLER_SYNC_TIMEOUT_MS))
		return sampler_sync_failed(ctx, "finish", started_ns);
	return 0;
}

void iiod_buffer_metadata_close(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	if (!ctx)
		return;
	if (ctx->sampler_started)
		spf_gain_sampler_stop(&ctx->sampler);
	if (ctx->timestamp_configured)
		(void)iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
				ctx->timestamp_control_previous);
	free(ctx);
}

ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	spf_gain_observation_v3_t observations[SPF_IIOD_OBSERVATION_CAPACITY];
	uint32_t observation_overflow_count = 0;
	uint64_t first_sample_sequence;
	uint64_t capture_index;
	uint64_t sample_delta;
	spf_rssi_pair_t rssi_start;
	spf_rssi_pair_t rssi_end;
	uint32_t rssi_overflow_count = 0;
	uint16_t observation_count;
	spf_gain_frame_decision_t frame_decision;
	size_t header_bytes;
	const uint8_t *raw;

	if (!ctx || dev != ctx->rx || !buffer || !metadata || !iq_offset ||
		!iq_bytes)
		return -EINVAL;
	header_bytes = spf_radio_frame_v3_header_bytes(
		SPF_IIOD_OBSERVATION_CAPACITY, 0);
	if (!header_bytes || metadata_capacity < header_bytes)
		return -ENOSPC;
	if (raw_bytes != ((size_t)ctx->samples_per_channel + 1U) * 8U)
		return -EIO;

	raw = iio_buffer_start(buffer);
	memcpy(&first_sample_sequence, raw, sizeof(first_sample_sequence));
	observation_count = spf_gain_sampler_collect(&ctx->sampler,
		first_sample_sequence, ctx->samples_per_channel, observations,
		SPF_IIOD_OBSERVATION_CAPACITY, &observation_overflow_count);
	frame_decision = spf_gain_frame_decide(ctx->frames_emitted,
		observation_count, ctx->startup_frames_discarded);
	if (frame_decision == SPF_GAIN_FRAME_DISCARD_STARTUP) {
		ctx->startup_frames_discarded++;
		return -EAGAIN;
	}
	if (frame_decision != SPF_GAIN_FRAME_ACCEPT)
		return -ENODATA;
	if (!spf_gain_sampler_collect_rssi(&ctx->sampler,
			first_sample_sequence, ctx->samples_per_channel,
			&rssi_start, &rssi_end, &rssi_overflow_count))
		return -ENODATA;
	if (rssi_overflow_count)
		return -EOVERFLOW;
	if (!ctx->stream_origin_valid) {
		ctx->stream_first_sample_sequence = first_sample_sequence;
		ctx->stream_origin_valid = true;
	}
	if (first_sample_sequence < ctx->stream_first_sample_sequence)
		return -ERANGE;
	sample_delta = first_sample_sequence - ctx->stream_first_sample_sequence;
	if (sample_delta % ctx->samples_per_channel)
		return -EIO;
	capture_index = sample_delta / ctx->samples_per_channel;
	const spf_radio_frame_v3_args_t args = {
		.metadata_features = SPF_META_REQUIRED_FEATURES_V3,
		.stream_id = ctx->stream_id,
		.buffer_sequence = capture_index,
		.first_sample_sequence = first_sample_sequence,
		.samples_per_channel = ctx->samples_per_channel,
		.iq_payload_bytes = ctx->samples_per_channel * UINT32_C(8),
		.enabled_scan_mask = SPF_IIOD_SCAN_MASK,
		.gain_observation_interval_samples =
			ctx->observation_interval_samples,
		.gain_observations = observations,
		.gain_observation_count = observation_count,
		.gain_observation_capacity = SPF_IIOD_OBSERVATION_CAPACITY,
		.gain_observation_overflow_count = observation_overflow_count,
		.gain_events = NULL,
		.gain_event_count = 0,
		.gain_event_capacity = 0,
		.gain_event_overflow_count = 0,
		.rssi_start = {
			.rx1_qdb = rssi_start.rx1_qdb,
			.rx2_qdb = rssi_start.rx2_qdb,
			.valid = rssi_start.valid,
			.duration_ns = rssi_start.duration_ns,
		},
		.rssi_end = {
			.rx1_qdb = rssi_end.rx1_qdb,
			.rx2_qdb = rssi_end.rx2_qdb,
			.valid = rssi_end.valid,
			.duration_ns = rssi_end.duration_ns,
		},
		.device_iio_overflow = false,
	};
	if (!spf_radio_frame_v3_build(metadata, metadata_capacity, &args))
		return -EIO;

	ctx->frames_emitted++;
	*iq_offset = sizeof(first_sample_sequence);
	*iq_bytes = (size_t)ctx->samples_per_channel * 8U;
	return (ssize_t)header_bytes;
}
