/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "buffer-metadata.h"
#include "spf-buffer-layout.h"
#include "spf-ddr-burst-request.h"
#include "spf-ddr-ring-request.h"
#include "spf-sampler-coverage.h"
#include "spf-tandem-metadata.h"
#include "spf-tandem-session.h"
#include "spf-temperature-cache.h"
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
#include "spf-hop-device.h"
#include "spf-hop-protocol.h"
#include "spf-hop-session.h"
#endif

#include <spf_gain_metadata.h>
#include <spf_gain_read.h>
#include <spf_gain_sampler.h>
#include <spf_radio_frame_v3.h>
#include <spf_rssi_read.h>

#include <errno.h>
#include <iio.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SPF_IIOD_OBSERVATION_CAPACITY UINT16_C(64)
#define SPF_IIOD_SAMPLER_START_TIMEOUT_MS UINT32_C(100)

struct spf_iiod_metadata_context {
	struct iio_device *rx;
	struct iio_device *phy;
	spf_gain_sampler_t sampler;
	struct spf_temperature_sampler temperature_sampler;
	struct spf_tandem_session tandem;
	struct spf_buffer_layout layout;
	uint32_t timestamp_control_previous;
	uint32_t samples_per_channel;
	uint32_t observation_interval_samples;
	uint64_t sampler_coverage_window_samples;
	uint64_t stream_id;
	struct spf_buffer_sequence_state sequence;
	uint64_t frames_emitted;
	uint64_t refills_started;
	uint32_t startup_frames_discarded;
	bool sampler_started;
	bool temperature_sampler_started;
	bool timestamp_configured;
	bool tandem_initialized;
	bool burst_enabled;
	bool ring_enabled;
	bool ring_prefix_complete;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	struct spf_hop_request_v1 hop_request;
	struct spf_hop_session_v1 hop_session;
	const struct spf_hop_device_ops_v1 *hop_ops;
	void *hop_device_context;
	pthread_mutex_t hop_lock;
	pthread_mutex_t tandem_lock;
	bool hop_enabled;
	bool hop_lock_initialized;
	bool tandem_lock_initialized;
	bool hop_device_opened;
	bool hop_session_initialized;
#endif
};

#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
static int tandem_acquire(struct spf_iiod_metadata_context *ctx)
{
	int unlock_ret;
	int ret;

	if (!ctx->hop_enabled)
		return spf_tandem_session_acquire(&ctx->tandem);
	ret = pthread_mutex_lock(&ctx->tandem_lock);
	if (ret)
		return -ret;
	ret = spf_tandem_session_acquire(&ctx->tandem);
	unlock_ret = pthread_mutex_unlock(&ctx->tandem_lock);
	return ret ? ret : (unlock_ret ? -unlock_ret : 0);
}

static int tandem_heartbeat(struct spf_iiod_metadata_context *ctx)
{
	int unlock_ret;
	int ret;

	if (!ctx->hop_enabled)
		return spf_tandem_session_heartbeat(&ctx->tandem);
	ret = pthread_mutex_lock(&ctx->tandem_lock);
	if (ret)
		return -ret;
	ret = spf_tandem_session_heartbeat(&ctx->tandem);
	unlock_ret = pthread_mutex_unlock(&ctx->tandem_lock);
	return ret ? ret : (unlock_ret ? -unlock_ret : 0);
}

static int tandem_collect(struct spf_iiod_metadata_context *ctx,
	uint64_t first_sample_sequence, struct adi_tandem_agc_event *events,
	size_t *event_count, struct adi_tandem_agc_status *status)
{
	int unlock_ret;
	int ret;

	if (!ctx || !events || !event_count || !status)
		return -EINVAL;
	if (ctx->hop_enabled) {
		ret = pthread_mutex_lock(&ctx->tandem_lock);
		if (ret)
			return -ret;
	}
	ret = spf_tandem_session_collect(&ctx->tandem, first_sample_sequence,
		ctx->samples_per_channel, events,
		ctx->tandem.request.event_capacity, event_count);
	if (!ret)
		*status = ctx->tandem.status;
	if (!ctx->hop_enabled)
		return ret;
	unlock_ret = pthread_mutex_unlock(&ctx->tandem_lock);
	return ret ? ret : (unlock_ret ? -unlock_ret : 0);
}
#else
static int tandem_acquire(struct spf_iiod_metadata_context *ctx)
{
	return spf_tandem_session_acquire(&ctx->tandem);
}

static int tandem_heartbeat(struct spf_iiod_metadata_context *ctx)
{
	return spf_tandem_session_heartbeat(&ctx->tandem);
}

static int tandem_collect(struct spf_iiod_metadata_context *ctx,
	uint64_t first_sample_sequence, struct adi_tandem_agc_event *events,
	size_t *event_count, struct adi_tandem_agc_status *status)
{
	int ret;

	if (!ctx || !events || !event_count || !status)
		return -EINVAL;
	ret = spf_tandem_session_collect(&ctx->tandem, first_sample_sequence,
		ctx->samples_per_channel, events,
		ctx->tandem.request.event_capacity, event_count);
	if (!ret)
		*status = ctx->tandem.status;
	return ret;
}
#endif

static bool buffered_capture_is_strict(
	const struct spf_iiod_metadata_context *ctx)
{
	return ctx->burst_enabled ||
		(ctx->ring_enabled && !ctx->ring_prefix_complete);
}

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

int iiod_buffer_metadata_open(const struct iio_device *dev,
		size_t samples_count, const uint32_t *mask, size_t words,
		size_t scan_bytes,
		const void *request, size_t request_bytes,
		void **provider_context, size_t *extra_samples,
		struct iiod_buffer_burst_plan *burst_plan)
{
	struct spf_ddr_burst_request burst_request;
	struct spf_ddr_ring_request ring_request;
	struct spf_iiod_metadata_context *ctx;
	const struct iio_context *iio_ctx;
	struct iio_channel *rx0;
	long long sample_rate_hz;
	size_t tandem_request_bytes = request_bytes;
	uint32_t timestamp_control;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	struct spf_hop_request_v1 hop_request;
	long long rf_bandwidth_hz = -1;
	bool hop_enabled = false;
#endif
	int ret;

	if (!dev || !request || !request_bytes || !provider_context ||
		!extra_samples || !burst_plan)
		return -EINVAL;
	memset(burst_plan, 0, sizeof(*burst_plan));
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (request_bytes == sizeof(struct adi_tandem_agc_request_v1) +
			SPF_HOP_REQUEST_BYTES) {
		ret = spf_hop_request_v1_decode(&hop_request,
			(const uint8_t *)request +
				sizeof(struct adi_tandem_agc_request_v1),
			SPF_HOP_REQUEST_BYTES);
		if (ret)
			return ret;
		hop_enabled = true;
		tandem_request_bytes = sizeof(struct adi_tandem_agc_request_v1);
	} else
#endif
	if (request_bytes == sizeof(struct adi_tandem_agc_request_v1) +
			SPF_DDR_BURST_REQUEST_BYTES) {
		ret = spf_ddr_burst_request_decode(&burst_request,
			(const uint8_t *)request + sizeof(struct adi_tandem_agc_request_v1),
			SPF_DDR_BURST_REQUEST_BYTES);
		if (ret)
			return ret;
		tandem_request_bytes = sizeof(struct adi_tandem_agc_request_v1);
	} else if (request_bytes == sizeof(struct adi_tandem_agc_request_v1) +
			SPF_DDR_RING_REQUEST_BYTES) {
		ret = spf_ddr_ring_request_decode(&ring_request,
			(const uint8_t *)request + sizeof(struct adi_tandem_agc_request_v1),
			SPF_DDR_RING_REQUEST_BYTES);
		if (ret)
			return ret;
		tandem_request_bytes = sizeof(struct adi_tandem_agc_request_v1);
	}
	struct spf_buffer_layout layout;
	ret = spf_buffer_layout_resolve(samples_count, mask, words, scan_bytes,
		&layout);
	if (ret)
		return ret;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (hop_enabled) {
		if (layout.receiver_count != 2U)
			return -EINVAL;
	} else
#endif
	if (tandem_request_bytes != request_bytes && layout.receiver_count != 1U) {
		return -EINVAL;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ret = spf_tandem_session_init(&ctx->tandem, request,
		tandem_request_bytes, NULL);
	if (ret) {
		free(ctx);
		return ret;
	}
	if (!ctx->tandem.request.observation_capacity ||
		ctx->tandem.request.observation_capacity >
			SPF_IIOD_OBSERVATION_CAPACITY ||
		!ctx->tandem.request.event_capacity ||
		ctx->tandem.request.event_capacity >
			SPF_TANDEM_EVENT_QUEUE_CAPACITY) {
		free(ctx);
		return -ENOSPC;
	}
	ret = spf_tandem_request_validate_event_window(&ctx->tandem.request,
		(uint32_t)samples_count);
	if (ret) {
		fprintf(stderr,
			"SPF tandem request cannot retain the refill arm window: "
			"samples=%zu events=%u cooldown=%u measurement=%u error=%d\n",
			samples_count, ctx->tandem.request.event_capacity,
			ctx->tandem.request.cooldown_periods,
			ctx->tandem.request.power_measurement_samples, ret);
		free(ctx);
		return ret;
	}
	ctx->tandem_initialized = true;
	ctx->burst_enabled = request_bytes == tandem_request_bytes +
		SPF_DDR_BURST_REQUEST_BYTES;
	ctx->ring_enabled = request_bytes == tandem_request_bytes +
		SPF_DDR_RING_REQUEST_BYTES;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	ctx->hop_enabled = hop_enabled;
	if (ctx->hop_enabled)
		ctx->hop_request = hop_request;
#endif
	ctx->layout = layout;
	ctx->rx = (struct iio_device *)dev;
	iio_ctx = iio_device_get_context(dev);
	ctx->phy = iio_context_find_device(iio_ctx, "ad9361-phy");
	if (!ctx->phy || !spf_gain_is_full_table_mode(ctx->phy) ||
		!spf_gain_is_digital_gain_disabled(ctx->phy)) {
		free(ctx);
		return -ENOTSUP;
	}
	if (ctx->burst_enabled || ctx->ring_enabled
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
			|| ctx->hop_enabled
#endif
			) {
		rx0 = iio_device_find_channel(ctx->phy, "voltage0", false);
		if (!rx0 || iio_channel_attr_read_longlong(rx0,
				"sampling_frequency", &sample_rate_hz) != 0 ||
			sample_rate_hz <= 0 || sample_rate_hz > UINT32_MAX) {
			free(ctx);
			return -EIO;
		}
		if (ctx->burst_enabled || ctx->ring_enabled) {
			ret = spf_ddr_burst_validate_frame_period((uint32_t)samples_count,
				(uint32_t)sample_rate_hz);
			if (ret) {
				fprintf(stderr,
					"SPF DDR buffered frame period is unsupported: samples=%zu "
					"rate=%lld minimum_us=%u error=%d\n",
					samples_count, sample_rate_hz,
					SPF_DDR_BURST_MIN_FRAME_DURATION_US, ret);
				free(ctx);
				return ret;
			}
		}
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
		if (ctx->hop_enabled &&
			((uint64_t)sample_rate_hz != ctx->hop_request.sample_rate_hz ||
			 iio_channel_attr_read_longlong(rx0, "rf_bandwidth",
				&rf_bandwidth_hz) != 0 || rf_bandwidth_hz <= 0 ||
			 (uint64_t)rf_bandwidth_hz !=
				ctx->hop_request.rf_bandwidth_hz)) {
			fprintf(stderr,
				"SPF persistent-hop settings do not match request: "
				"sample_rate=%lld requested_rate=%llu bandwidth=%lld "
				"requested_bandwidth=%llu\n", sample_rate_hz,
				(unsigned long long)ctx->hop_request.sample_rate_hz,
				rf_bandwidth_hz,
				(unsigned long long)ctx->hop_request.rf_bandwidth_hz);
			free(ctx);
			return -ESTALE;
		}
#endif
	}

	if (iio_device_reg_read(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
			&ctx->timestamp_control_previous) != 0) {
		free(ctx);
		return -EIO;
	}
	timestamp_control = (ctx->layout.timestamp_words << 1) |
		(ctx->timestamp_control_previous & UINT32_C(1));
	if (iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
			timestamp_control) != 0) {
		free(ctx);
		return -EIO;
	}
	ctx->timestamp_configured = true;
	ctx->samples_per_channel = (uint32_t)samples_count;
	ret = spf_tandem_request_observation_interval(&ctx->tandem.request,
		(uint32_t)samples_count, &ctx->observation_interval_samples);
	if (ret) {
		(void)iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
			ctx->timestamp_control_previous);
		free(ctx);
		return ret;
	}
	ctx->stream_id = make_stream_id(ctx);
	if (!spf_gain_sampler_start(&ctx->sampler,
			ctx->observation_interval_samples)) {
		(void)iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
				ctx->timestamp_control_previous);
		free(ctx);
		return -EIO;
	}
	ctx->sampler_started = true;
	ctx->temperature_sampler_started =
		spf_temperature_sampler_start(&ctx->temperature_sampler);
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		ret = pthread_mutex_init(&ctx->hop_lock, NULL);
		if (ret) {
			iiod_buffer_metadata_close(ctx);
			return -ret;
		}
		ctx->hop_lock_initialized = true;
		ret = pthread_mutex_init(&ctx->tandem_lock, NULL);
		if (ret) {
			iiod_buffer_metadata_close(ctx);
			return -ret;
		}
		ctx->tandem_lock_initialized = true;
		ret = spf_hop_device_v1_open(ctx->rx, ctx->phy, &ctx->tandem,
			&ctx->tandem_lock, &ctx->hop_request,
			&ctx->hop_device_context, &ctx->hop_ops);
		if (ret) {
			iiod_buffer_metadata_close(ctx);
			return ret;
		}
		ctx->hop_device_opened = true;
		ret = spf_hop_session_v1_init(&ctx->hop_session,
			&ctx->hop_request, ctx->hop_ops, ctx->hop_device_context);
		if (ret) {
			iiod_buffer_metadata_close(ctx);
			return ret;
		}
		ctx->hop_session_initialized = true;
	}
#endif

	*provider_context = ctx;
	*extra_samples = ctx->layout.extra_samples;
	if (ctx->burst_enabled || ctx->ring_enabled) {
		if (ctx->burst_enabled) {
			burst_plan->requested_iq_bytes =
				burst_request.requested_iq_bytes;
		} else {
			burst_plan->ring_capacity_iq_bytes = ring_request.capacity_iq_bytes;
			burst_plan->ring_capture_frames = ring_request.capture_frames;
			burst_plan->ring_flags = ring_request.flags;
		}
		burst_plan->metadata_capacity = spf_radio_frame_v5_header_bytes(
			(uint16_t)ctx->tandem.request.observation_capacity,
			(uint16_t)ctx->tandem.request.event_capacity);
		if (!burst_plan->metadata_capacity) {
			iiod_buffer_metadata_close(ctx);
			*provider_context = NULL;
			return -EOVERFLOW;
		}
	}
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		burst_plan->metadata_capacity = spf_radio_frame_v5_header_bytes(
			(uint16_t)ctx->tandem.request.observation_capacity,
			(uint16_t)ctx->tandem.request.event_capacity);
		if (!burst_plan->metadata_capacity || burst_plan->metadata_capacity >
				SIZE_MAX - SPF_HOP_SIDECAR_MAX_BYTES) {
			iiod_buffer_metadata_close(ctx);
			*provider_context = NULL;
			return -EOVERFLOW;
		}
		burst_plan->metadata_capacity += SPF_HOP_SIDECAR_MAX_BYTES;
	}
#endif
	return 0;
}

int iiod_buffer_metadata_buffer_opened(void *provider_context,
		unsigned int kernel_buffers_count)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	struct spf_sampler_coverage_plan coverage;
	int ret;

	if (!ctx || !kernel_buffers_count)
		return -EINVAL;
	ret = spf_sampler_coverage_plan_compute(ctx->samples_per_channel,
		ctx->observation_interval_samples, kernel_buffers_count,
		SPF_GAIN_SAMPLER_RING_CAPACITY, &coverage);
	if (ret)
		return ret;
	ret = tandem_acquire(ctx);
	if (ret)
		return ret;
	ctx->sampler_coverage_window_samples = coverage.window_samples;
	ctx->refills_started = 0;
	spf_gain_sampler_limit(&ctx->sampler,
		ctx->sampler_coverage_window_samples);
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		pthread_mutex_lock(&ctx->hop_lock);
		ret = spf_hop_session_v1_start(&ctx->hop_session);
		pthread_mutex_unlock(&ctx->hop_lock);
		if (ret)
			return ret;
	}
#endif
	return 0;
}

int iiod_buffer_metadata_before_refill(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;

	if (!ctx)
		return -EINVAL;
	/* The first dequeue consumes an already queued block. Every later refill
	 * can rearm one block while all older queued blocks remain capture work.
	 * Reset to the complete queue-depth window so producer copy/backpressure
	 * cannot put the sampler to sleep while DMA is still filling those blocks.
	 * The fixed window remains bounded even if the producer outruns sampling.
	 */
	if (ctx->refills_started != 0 &&
		!spf_gain_sampler_limit_and_wait_started(&ctx->sampler,
			ctx->sampler_coverage_window_samples,
			SPF_IIOD_SAMPLER_START_TIMEOUT_MS))
		return -ETIMEDOUT;
	ctx->refills_started++;
	return 0;
}

int iiod_buffer_metadata_after_refill(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;

	if (!ctx)
		return -EINVAL;
	if (ctx->refills_started > 1 &&
		!spf_gain_sampler_finish_capture(
			&ctx->sampler, SPF_IIOD_SAMPLER_START_TIMEOUT_MS))
		return -ETIMEDOUT;
	/* Every completed refill proves the owner is alive, including frames that
	 * metadata_get() subsequently discards during sampler startup.
	 */
	return tandem_heartbeat(ctx);
}

void iiod_buffer_metadata_ring_prefix_complete(void *provider_context,
	bool complete)
{
	struct spf_iiod_metadata_context *ctx = provider_context;

	if (ctx && ctx->ring_enabled)
		ctx->ring_prefix_complete = complete;
}

void iiod_buffer_metadata_close(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	if (!ctx)
		return;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_session_initialized) {
		pthread_mutex_lock(&ctx->hop_lock);
		(void)spf_hop_session_v1_cancel(&ctx->hop_session,
			SPF_HOP_REASON_CLIENT_CLOSE);
		pthread_mutex_unlock(&ctx->hop_lock);
	}
#endif
	if (ctx->tandem_initialized)
		spf_tandem_session_close(&ctx->tandem);
	if (ctx->sampler_started)
		spf_gain_sampler_stop(&ctx->sampler);
	if (ctx->temperature_sampler_started)
		spf_temperature_sampler_stop(&ctx->temperature_sampler);
	if (ctx->timestamp_configured)
		(void)iio_device_reg_write(ctx->rx, SPF_ADC_TIMESTAMP_CONTROL_REG,
				ctx->timestamp_control_previous);
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_device_opened)
		spf_hop_device_v1_destroy(ctx->hop_device_context);
	if (ctx->tandem_lock_initialized)
		pthread_mutex_destroy(&ctx->tandem_lock);
	if (ctx->hop_lock_initialized)
		pthread_mutex_destroy(&ctx->hop_lock);
#endif
	free(ctx);
}

ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	spf_gain_observation_v3_t observations[SPF_IIOD_OBSERVATION_CAPACITY];
	struct adi_tandem_agc_event events[SPF_TANDEM_EVENT_QUEUE_CAPACITY];
	struct adi_tandem_agc_status tandem_status;
	uint32_t observation_overflow_count = 0;
	uint64_t first_sample_sequence;
	struct spf_buffer_sequence_result sequence;
	spf_rssi_pair_t rssi_start;
	spf_rssi_pair_t rssi_end;
	uint32_t rssi_overflow_count = 0;
	uint16_t observation_count;
	size_t event_count;
	spf_gain_frame_decision_t frame_decision;
	size_t header_bytes;
	size_t total_metadata_bytes;
	const uint8_t *raw;
	int ret;

	if (!ctx || dev != ctx->rx || !buffer || !metadata || !iq_offset ||
		!iq_bytes)
		return -EINVAL;
	header_bytes = spf_radio_frame_v5_header_bytes(
		(uint16_t)ctx->tandem.request.observation_capacity,
		(uint16_t)ctx->tandem.request.event_capacity);
	total_metadata_bytes = header_bytes;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		if (header_bytes > SIZE_MAX - SPF_HOP_SIDECAR_MAX_BYTES)
			return -EOVERFLOW;
		total_metadata_bytes += SPF_HOP_SIDECAR_MAX_BYTES;
	}
#endif
	if (!header_bytes || metadata_capacity < total_metadata_bytes)
		return -ENOSPC;
	if (raw_bytes != ctx->layout.raw_bytes) {
		fprintf(stderr,
			"SPF metadata raw frame mismatch: expected=%zu observed=%zu "
			"frame=%llu buffered=%u\n",
			ctx->layout.raw_bytes, raw_bytes,
			(unsigned long long)ctx->frames_emitted,
			(ctx->burst_enabled || ctx->ring_enabled) ? 1U : 0U);
		return -EIO;
	}

	raw = iio_buffer_start(buffer);
	if (!raw)
		return -EIO;
	memcpy(&first_sample_sequence, raw, sizeof(first_sample_sequence));
	observation_count = spf_gain_sampler_collect(&ctx->sampler,
		first_sample_sequence, ctx->samples_per_channel, observations,
		(uint16_t)ctx->tandem.request.observation_capacity,
		&observation_overflow_count);
	if (buffered_capture_is_strict(ctx) &&
			observation_overflow_count) {
		fprintf(stderr,
			"SPF metadata gain observation overflow: count=%u frame=%llu\n",
			observation_overflow_count,
			(unsigned long long)ctx->frames_emitted);
		return -EOVERFLOW;
	}
	if (ctx->tandem.request.mode == ADI_TANDEM_AGC_MODE_AUTO)
		observation_count = spf_tandem_compact_coherent_observations(
			observations, observation_count);
	frame_decision = spf_gain_frame_decide(ctx->frames_emitted,
		observation_count, ctx->startup_frames_discarded);
	if (frame_decision == SPF_GAIN_FRAME_DISCARD_STARTUP) {
		ctx->startup_frames_discarded++;
		return -EAGAIN;
	}
	if (frame_decision != SPF_GAIN_FRAME_ACCEPT) {
		fprintf(stderr,
			"SPF metadata frame has no gain coverage: frame=%llu "
			"observations=%u startup_discards=%u buffered=%u\n",
			(unsigned long long)ctx->frames_emitted, observation_count,
			ctx->startup_frames_discarded,
			(ctx->burst_enabled || ctx->ring_enabled) ? 1U : 0U);
		return -ESTALE;
	}
	if (!spf_gain_sampler_collect_rssi(&ctx->sampler,
			first_sample_sequence, ctx->samples_per_channel,
			&rssi_start, &rssi_end, &rssi_overflow_count)) {
		fprintf(stderr,
			"SPF metadata frame has no RSSI coverage: frame=%llu "
			"first_sample=%llu samples=%u observations=%u buffered=%u\n",
			(unsigned long long)ctx->frames_emitted,
			(unsigned long long)first_sample_sequence,
			ctx->samples_per_channel, observation_count,
			(ctx->burst_enabled || ctx->ring_enabled) ? 1U : 0U);
		return -ESTALE;
	}
	if (rssi_overflow_count) {
		fprintf(stderr,
			"SPF metadata RSSI observation overflow: count=%u frame=%llu\n",
			rssi_overflow_count, (unsigned long long)ctx->frames_emitted);
		return -EOVERFLOW;
	}
	ret = tandem_collect(ctx, first_sample_sequence, events, &event_count,
		&tandem_status);
	if (ret) {
		fprintf(stderr,
			"SPF metadata tandem collection failed: error=%d frame=%llu "
			"first_sample=%llu samples=%u\n",
			ret, (unsigned long long)ctx->frames_emitted,
			(unsigned long long)first_sample_sequence,
			ctx->samples_per_channel);
		return ret;
	}
	ret = spf_buffer_sequence_resolve(&ctx->sequence, first_sample_sequence,
			ctx->samples_per_channel, &sequence);
	if (ret)
		return ret;
	if (buffered_capture_is_strict(ctx) &&
			sequence.missing_samples_before) {
		fprintf(stderr,
			"SPF metadata strict buffered counter gap: frame=%llu "
			"first_sample=%llu "
			"missing=%llu\n",
			(unsigned long long)ctx->frames_emitted,
			(unsigned long long)first_sample_sequence,
			(unsigned long long)sequence.missing_samples_before);
		return -EOVERFLOW;
	}
	if (sequence.missing_samples_before) {
		fprintf(stderr,
			"SPF metadata streaming counter gap accounted: frame=%llu "
			"first_sample=%llu missing=%llu ring_prefix_complete=%u\n",
			(unsigned long long)ctx->frames_emitted,
			(unsigned long long)first_sample_sequence,
			(unsigned long long)sequence.missing_samples_before,
			ctx->ring_prefix_complete ? 1U : 0U);
	}
	const spf_radio_frame_v6_args_t args = {
		.frame = {
			.metadata_features = SPF_META_REQUIRED_FEATURES_V6,
			.stream_id = ctx->stream_id,
			.buffer_sequence = sequence.buffer_sequence,
			.first_sample_sequence = first_sample_sequence,
			.samples_per_channel = ctx->samples_per_channel,
			.iq_payload_bytes = (uint32_t)ctx->layout.iq_bytes,
			.enabled_scan_mask = ctx->layout.enabled_scan_mask,
			.gain_observation_interval_samples =
				ctx->observation_interval_samples,
			.gain_observations = observations,
			.gain_observation_count = observation_count,
			.gain_observation_capacity =
				(uint16_t)ctx->tandem.request.observation_capacity,
			.gain_observation_overflow_count = observation_overflow_count,
			.gain_events = (const spf_gain_event_v3_t *)events,
			.gain_event_count = (uint16_t)event_count,
			.gain_event_capacity =
				(uint16_t)ctx->tandem.request.event_capacity,
			.gain_event_overflow_count = tandem_status.overflow_count,
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
			.device_iio_overflow = sequence.missing_samples_before != 0,
		},
		.tandem_status = &tandem_status,
		.ad9361_temperature_mdeg_c = ctx->temperature_sampler_started ?
			spf_temperature_sampler_get(&ctx->temperature_sampler) :
			SPF_TEMPERATURE_INVALID,
		.missing_samples_before = sequence.missing_samples_before,
	};
	if (!spf_radio_frame_v6_build(metadata, metadata_capacity, &args))
		return -EIO;

#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		struct spf_hop_sidecar_v1 sidecar;
		int sidecar_bytes;

		pthread_mutex_lock(&ctx->hop_lock);
		ret = spf_hop_session_v1_on_block(&ctx->hop_session,
			sequence.buffer_sequence, first_sample_sequence,
			first_sample_sequence + ctx->samples_per_channel, &sidecar);
		if (!ret)
			sidecar_bytes = spf_hop_sidecar_v1_encode(
				(uint8_t *)metadata + header_bytes,
				metadata_capacity - header_bytes, &sidecar);
		else {
			fprintf(stderr,
				"SPF persistent-hop block failed: error=%d state=%u reason=%u "
				"session_error=%d block=%llu first=%llu end=%llu visits=%llu "
				"events=%llu\n", ret, ctx->hop_session.status.state,
				ctx->hop_session.status.terminal_reason,
				ctx->hop_session.status.error_code,
				(unsigned long long)sequence.buffer_sequence,
				(unsigned long long)first_sample_sequence,
				(unsigned long long)(first_sample_sequence +
					ctx->samples_per_channel),
				(unsigned long long)ctx->hop_session.status.visits_started,
				(unsigned long long)ctx->hop_session.status.events_emitted);
			sidecar_bytes = ret;
		}
		pthread_mutex_unlock(&ctx->hop_lock);
		if (sidecar_bytes < 0)
			return sidecar_bytes;
		total_metadata_bytes = header_bytes + (size_t)sidecar_bytes;
	} else
#endif
	{
		total_metadata_bytes = header_bytes;
	}

	spf_buffer_sequence_commit(&ctx->sequence, &sequence);
	ctx->frames_emitted++;
	*iq_offset = sizeof(first_sample_sequence);
	*iq_bytes = ctx->layout.iq_bytes;
	return (ssize_t)total_metadata_bytes;
}

ssize_t iiod_buffer_metadata_status(void *provider_context,
		void *status, size_t status_capacity)
{
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	struct spf_iiod_metadata_context *ctx = provider_context;
	struct spf_hop_status_v1 hop_status;
	int ret;

	if (!ctx || !ctx->hop_enabled)
		return -ENODATA;
	if (!status)
		return -EINVAL;
	if (status_capacity < SPF_HOP_STATUS_BYTES)
		return -ENOSPC;
	pthread_mutex_lock(&ctx->hop_lock);
	spf_hop_session_v1_get_status(&ctx->hop_session, &hop_status);
	ret = spf_hop_status_v1_encode(status, status_capacity, &hop_status);
	pthread_mutex_unlock(&ctx->hop_lock);
	return ret ? ret : SPF_HOP_STATUS_BYTES;
#else
	(void)provider_context;
	(void)status;
	(void)status_capacity;
	return -ENODATA;
#endif
}

int iiod_buffer_metadata_cancel(void *provider_context)
{
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	struct spf_iiod_metadata_context *ctx = provider_context;
	int ret;

	if (!ctx || !ctx->hop_enabled || !ctx->hop_session_initialized)
		return -ENODATA;
	pthread_mutex_lock(&ctx->hop_lock);
	ret = spf_hop_session_v1_cancel(&ctx->hop_session,
		SPF_HOP_REASON_CLIENT_CLOSE);
	pthread_mutex_unlock(&ctx->hop_lock);
	return ret;
#else
	(void)provider_context;
	return -ENODATA;
#endif
}

static int spf_exact_gap_header(
		const struct spf_iiod_metadata_context *ctx,
		const void *metadata, size_t metadata_bytes,
		const spf_radio_meta_v3_prefix_t **header)
{
	const spf_radio_meta_v3_prefix_t *record = metadata;

	if (!ctx || !metadata || !header ||
			metadata_bytes < sizeof(*record) + sizeof(uint32_t))
		return -EINVAL;
	if (record->magic != SPF_GAIN_META_MAGIC ||
			record->version != SPF_GAIN_META_VERSION_V6 ||
			record->header_bytes > metadata_bytes ||
			(record->features & SPF_META_FEATURE_EXACT_GAP_ACCOUNTING) == 0 ||
			record->samples_per_channel != ctx->samples_per_channel ||
			record->first_sample_sequence >
				UINT64_MAX - record->samples_per_channel)
		return -EBADMSG;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		struct spf_hop_sidecar_v1 sidecar;
		int ret;

		if (record->header_bytes == metadata_bytes)
			return -EBADMSG;
		ret = spf_hop_sidecar_v1_decode(&sidecar,
			(const uint8_t *)metadata + record->header_bytes,
			metadata_bytes - record->header_bytes);
		if (ret || sidecar.session_id != ctx->hop_request.session_id ||
			sidecar.buffer_sequence != record->buffer_sequence ||
			sidecar.block_first_sample != record->first_sample_sequence ||
			sidecar.block_end_sample != record->first_sample_sequence +
				record->samples_per_channel)
			return -EBADMSG;
	} else
#endif
	if (record->header_bytes != metadata_bytes) {
		return -EBADMSG;
	}
	*header = record;
	return 0;
}

int iiod_buffer_metadata_describe_frame(void *provider_context,
		const void *metadata, size_t metadata_bytes,
		struct iiod_buffer_metadata_frame_info *info)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	const spf_radio_meta_v3_prefix_t *header;
	int ret;

	if (!info)
		return -EINVAL;
	ret = spf_exact_gap_header(ctx, metadata, metadata_bytes, &header);
	if (ret)
		return ret;
	info->first_sample_sequence = header->first_sample_sequence;
	info->frame_end = header->first_sample_sequence +
		header->samples_per_channel;
	info->missing_samples_before =
		spf_radio_meta_v6_missing_samples_before(header);
	return 0;
}

int iiod_buffer_metadata_rebase_frame(void *provider_context,
		void *metadata, size_t metadata_bytes,
		uint64_t previous_frame_end)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	const spf_radio_meta_v3_prefix_t *const_header;
	int ret;

	ret = spf_exact_gap_header(ctx, metadata, metadata_bytes, &const_header);
	if (ret)
		return ret;
	return spf_radio_frame_v6_rebase_gap(metadata, const_header->header_bytes,
		previous_frame_end) ? 0 : -ERANGE;
}
