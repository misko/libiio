/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "buffer-metadata.h"
#include "spf-counter-metadata.h"
#include "adi-rx-counter.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include "spf-buffer-layout.h"
#include "spf-ddr-burst-request.h"
#include "spf-ddr-ring-request.h"
#include "spf-sampler-coverage.h"
#include "spf-tandem-metadata.h"
#include "spf-tandem-session.h"
#include "spf-legacy-metadata.h"
#include "spf-temperature-cache.h"
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
#include "spf-hop-device.h"
#include "spf-hop-protocol.h"
#include "spf-hop-session.h"
#endif
#ifdef IIOD_HAS_SCANNER_GLRT
#include "spf-scanner-glrt.h"
#endif
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
#include "spf-hop-adaptive-policy.h"
#ifndef IIOD_SCANNER_GLRT_POSITIVE_ONLY
#error Adaptive hopping requires an explicit positive-only detector build
#endif
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
	bool counter_only;
	int counter_fd;
	uint32_t counter_rate;
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
#ifdef IIOD_HAS_SCANNER_GLRT
	struct spf_scanner_glrt *glrt;
	uint8_t *glrt_legacy_metadata;
	size_t glrt_legacy_capacity;
#endif
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	struct spf_hop_request_v1 hop_request;
	struct spf_hop_session_v1 hop_session;
	const struct spf_hop_device_ops_v1 *hop_ops;
	struct spf_hop_request_v2 adaptive_request;
	struct spf_hop_session_v2 adaptive_session;
	const struct spf_hop_device_ops_v2 *adaptive_ops;
	bool hop_adaptive;
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	struct spf_hop_adaptive_policy *policy;
#endif
	void *hop_device_context;
	pthread_mutex_t hop_lock;
	pthread_mutex_t tandem_lock;
	bool hop_enabled;
	bool hop_lock_initialized;
	bool tandem_lock_initialized;
	bool hop_device_opened;
	bool hop_session_initialized;
#endif
	bool scan_enabled;
	bool scan_started;
	bool scan_thread_live;
	bool scan_cancel_requested;
	bool scan_finished;
	unsigned int scan_kernel_buffers;
	int scan_error;
	pthread_t scan_thread;
	pthread_mutex_t scan_lock;
	struct spf_scan_setup scan_setup;
	struct spf_scan_radio scan_radio;
	struct spf_scan_session *scan_session;
	uint64_t scan_counter_anchor;
};

static int scan_release_block(void *context, uintptr_t token)
{
	(void)context;
	return iio_buffer_block_release((struct iio_buffer_block *)token);
}

static int scan_open(const struct iio_device *dev, size_t samples_count,
		const uint32_t *mask, size_t words, size_t scan_bytes,
		const void *request, size_t request_bytes, void **provider_context,
		size_t *extra_samples)
{
	struct spf_scan_session_runtime runtime;
	struct spf_buffer_layout layout;
	struct spf_iiod_metadata_context *ctx;
	const struct iio_context *iio_ctx;
	struct iio_channel *rx0;
	long long rate, bandwidth;
	unsigned int buffers;
	int ret;

	if (!iio_device_get_name(dev) ||
	    strcmp(iio_device_get_name(dev), "cf-ad9361-lpc"))
		return -ENODEV;
	ret = spf_buffer_layout_resolve(samples_count, mask, words, scan_bytes,
					&layout);
	if (ret)
		return ret;
	if ((layout.enabled_scan_mask != 3 && layout.enabled_scan_mask != 0x0f) ||
	    (layout.receiver_count != 1 && layout.receiver_count != 2))
		return -EINVAL;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ret = spf_scan_setup_decode(&ctx->scan_setup, request, request_bytes);
	if (ret)
		goto error;
	ctx->rx = (struct iio_device *)dev;
	ctx->layout = layout;
	if ((ctx->scan_setup.rx_mask == SPF_SCAN_RX1 && layout.enabled_scan_mask != 3) ||
	    (ctx->scan_setup.rx_mask == SPF_SCAN_RX1_RX2 && layout.enabled_scan_mask != 0x0f)) {
		ret = -EINVAL;
		goto error;
	}
	ctx->samples_per_channel = (uint32_t)samples_count;
	iio_ctx = iio_device_get_context(dev);
	ctx->phy = iio_context_find_device(iio_ctx, "ad9361-phy");
	rx0 = ctx->phy ? iio_device_find_channel(ctx->phy, "voltage0", false) : NULL;
	if (!rx0 || iio_channel_attr_read_longlong(rx0, "sampling_frequency",
						  &rate) ||
	    iio_channel_attr_read_longlong(rx0, "rf_bandwidth", &bandwidth) ||
	    rate != ctx->scan_setup.source_rate_hz ||
	    bandwidth != ctx->scan_setup.analog_bandwidth_hz) {
		ret = -ERANGE;
		goto error;
	}
	buffers = iio_device_get_kernel_buffers_count(dev);
	if (buffers < 4 || buffers > SPF_VISIT_QUEUE_MAX_BLOCKS) {
		ret = -ENOSPC;
		goto error;
	}
	ctx->counter_fd = open("/dev/tandem-agc-events", O_RDWR | O_CLOEXEC);
	if (ctx->counter_fd < 0) {
		ret = -errno;
		goto error;
	}
	ret = spf_scan_radio_init(&ctx->scan_radio, ctx->counter_fd, NULL);
	if (ret)
		goto error_close;
	runtime = (struct spf_scan_session_runtime) {
		.block_count = buffers,
		/* A Pluto's ADC DMA queue has four blocks.  A full dwell reserves
		 * its data block plus the conservative boundary block, so retaining
		 * two blocks as headroom leaves no forward-progress slot once the
		 * first dwell is queued.  Larger queues retain the two-block margin;
		 * the native four-block queue retains one rearm block. */
		.headroom_blocks = buffers == 4 ? 1 : 2,
		.block_samples = (uint32_t)samples_count,
		.bytes_per_sample = layout.iq_bytes_per_sample,
		.drain_bytes_per_second = UINT64_C(60000000),
		.release_block = scan_release_block,
	};
	ret = spf_scan_session_create(&ctx->scan_session, &ctx->scan_setup,
				      &runtime, &ctx->scan_radio, 0);
	if (ret)
		goto error_close;
	ret = spf_scan_session_counter(ctx->scan_session,
				       &ctx->scan_counter_anchor);
	if (ret)
		goto error_session;
	ret = pthread_mutex_init(&ctx->scan_lock, NULL);
	if (ret) {
		ret = -ret;
		goto error_session;
	}
	ctx->scan_enabled = true;
	ctx->scan_kernel_buffers = buffers;
	*provider_context = ctx;
	*extra_samples = layout.extra_samples;
	return 0;

error_session:
	(void)spf_scan_session_cancel(ctx->scan_session, 0);
	(void)spf_scan_session_destroy(ctx->scan_session);
error_close:
	close(ctx->counter_fd);
error:
	free(ctx);
	return ret;
}

#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
static size_t hop_sidecar_capacity(const struct spf_iiod_metadata_context *ctx)
{ return ctx->hop_adaptive ? SPF_HOP_ADAPTIVE_SIDECAR_MAX_BYTES : SPF_HOP_SIDECAR_MAX_BYTES; }

static const struct spf_hop_status_v1 *hop_status_state(const struct spf_iiod_metadata_context *ctx)
{ return ctx->hop_adaptive ? &ctx->adaptive_session.core.status : &ctx->hop_session.status; }

static int hop_cancel(struct spf_iiod_metadata_context *ctx, uint16_t reason)
{
	return ctx->hop_adaptive ? spf_hop_session_v2_cancel(&ctx->adaptive_session, reason) :
		spf_hop_session_v1_cancel(&ctx->hop_session, reason);
}

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

static int counter_open(const struct iio_device *dev, size_t samples_count, const uint32_t *mask,
			size_t words, size_t scan_bytes, const void *request, size_t request_bytes,
			void **provider_context, size_t *extra_samples)
{
	struct spf_counter_request decoded;
	struct spf_buffer_layout layout;
	struct spf_iiod_metadata_context *ctx;
	struct adi_rx_counter_request lease = {0};
	int ret;
	if (!iio_device_get_name(dev) || strcmp(iio_device_get_name(dev), "cf-ad9361-lpc"))
		return -ENODEV;
	ret = spf_buffer_layout_resolve(samples_count, mask, words, scan_bytes, &layout);
	if (ret)
		return ret;
	ret = spf_counter_request_decode(request, request_bytes, samples_count,
					 layout.enabled_scan_mask, &decoded);
	if (ret)
		return ret;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->counter_fd = open("/dev/tandem-agc-events", O_RDWR | O_CLOEXEC);
	if (ctx->counter_fd < 0) {
		ret = -errno;
		free(ctx);
		return ret;
	}
	lease.magic = ADI_RX_COUNTER_MAGIC;
	lease.version = 1;
	lease.size = sizeof(lease);
	lease.required_features = ADI_RX_COUNTER_FEATURES;
	lease.scan_mask = 3;
	lease.sample_rate_hz = decoded.sample_rate_hz;
	lease.samples_per_channel = decoded.samples_per_channel;
	if (ioctl(ctx->counter_fd, ADI_RX_COUNTER_IOC_ACQUIRE, &lease) < 0) {
		ret = -errno;
		close(ctx->counter_fd);
		free(ctx);
		return ret;
	}
	ctx->counter_only = true;
	ctx->counter_rate = decoded.sample_rate_hz;
	ctx->rx = (struct iio_device *)dev;
	ctx->layout = layout;
	ctx->samples_per_channel = decoded.samples_per_channel;
	ctx->stream_id = make_stream_id(ctx);
	*provider_context = ctx;
	*extra_samples = layout.extra_samples;
	return 0;
}

#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
static uint64_t scanner_callback_clock(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
#endif

#ifdef IIOD_HAS_SCANNER_GLRT
static ssize_t scanner_glrt_drain(void *provider_context, void *output,
	size_t capacity)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	return ctx ? spf_scanner_glrt_drain(ctx->glrt, output, capacity) : -ENODATA;
}
#endif

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
#ifdef IIOD_HAS_SCANNER_GLRT
	leo_scanner_glrt_request_v1 glrt_request;
	bool glrt_enabled = false;
#endif
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	struct spf_hop_request_v1 hop_request;
	long long rf_bandwidth_hz = -1;
	bool hop_enabled = false;
	bool hop_adaptive = false;
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	struct spf_hop_request_v2 adaptive_request;
#endif
#endif
	int ret;

	if (!dev || !request || !request_bytes || !provider_context ||
		!extra_samples || !burst_plan)
		return -EINVAL;
	memset(burst_plan, 0, sizeof(*burst_plan));
	if (request_bytes >= 4 &&
	    spf_counter_read32(request) == UINT32_C(0x51535053))
		return scan_open(dev, samples_count, mask, words, scan_bytes,
				 request, request_bytes, provider_context,
				 extra_samples);
	if (request_bytes >= 4 && spf_counter_read32(request) == SPF_COUNTER_REQUEST_MAGIC)
		return counter_open(dev, samples_count, mask, words, scan_bytes, request,
				    request_bytes, provider_context, extra_samples);
#ifdef IIOD_HAS_SCANNER_GLRT
	if (request_bytes >= 4 && !memcmp(request, "LGO1", 4)) {
		ret = leo_scanner_glrt_request_decode(&glrt_request, request, request_bytes);
		if (ret)
			return ret;
		request = glrt_request.legacy_request;
		request_bytes = glrt_request.legacy_bytes;
		tandem_request_bytes = request_bytes;
		glrt_enabled = true;
	}
#endif
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (request_bytes == sizeof(struct adi_tandem_agc_request_v1) +
			SPF_HOP_HOST_REQUEST_BYTES) {
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
		if (glrt_enabled) return -ENOTSUP;
		const uint8_t *host_wire=(const uint8_t *)request+
			sizeof(struct adi_tandem_agc_request_v1);
		ret=host_wire[4]==4 ?
			spf_hop_request_v4_decode(&adaptive_request,host_wire,SPF_HOP_HOST_REQUEST_BYTES) :
			spf_hop_request_v3_decode(&adaptive_request,host_wire,SPF_HOP_HOST_REQUEST_BYTES);
		if (ret) {
			fprintf(stderr, "SPF host adaptive OPEN rejected: stage=request_decode version=%u error=%d\n",
				host_wire[4], ret);
			return ret;
		}
		ret=spf_hop_adaptive_policy_validate_pinned(&adaptive_request);
		if (ret) {
			fprintf(stderr, "SPF host adaptive OPEN rejected: stage=policy error=%d\n", ret);
			return ret;
		}
		hop_request=adaptive_request.geometry;
		hop_enabled=hop_adaptive=true;
		tandem_request_bytes=sizeof(struct adi_tandem_agc_request_v1);
#else
		return -ENOTSUP;
#endif
	} else if (request_bytes == sizeof(struct adi_tandem_agc_request_v1) +
			SPF_HOP_ADAPTIVE_REQUEST_BYTES) {
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
		if (!glrt_enabled) return -ENOTSUP;
		ret = spf_hop_request_v2_decode(&adaptive_request,
			(const uint8_t *)request + sizeof(struct adi_tandem_agc_request_v1),
			SPF_HOP_ADAPTIVE_REQUEST_BYTES);
		if (ret) return ret;
		if (adaptive_request.policy.generation != glrt_request.generation) return -ESTALE;
		ret = spf_hop_adaptive_policy_validate_pinned(&adaptive_request);
		if (ret) return ret;
		hop_request = adaptive_request.geometry;
		hop_enabled = hop_adaptive = true;
		tandem_request_bytes = sizeof(struct adi_tandem_agc_request_v1);
#else
		return -ENOTSUP;
#endif
	} else if (request_bytes == sizeof(struct adi_tandem_agc_request_v1) +
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
#ifdef IIOD_HAS_SCANNER_GLRT
	/* Only persistent, dual-RX scanner sessions have qualified ownership for
	 * a post-capture drain. Burst/ring/ordinary requests stay unchanged. */
	if (glrt_enabled && !hop_enabled)
		return -ENOTSUP;
	if (glrt_enabled) {
		ret = spf_scanner_glrt_validate(&glrt_request, &hop_request, samples_count);
		if (ret)
			return ret;
	}
#endif
	ret = spf_buffer_layout_resolve(samples_count, mask, words, scan_bytes,
		&layout);
	if (ret) {
		fprintf(stderr, "SPF metadata OPEN rejected: stage=buffer_layout error=%d\n", ret);
		return ret;
	}
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (hop_enabled) {
		ret = spf_buffer_hop_receiver_rate_validate(layout.receiver_count,
			hop_request.sample_rate_hz,
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
			hop_adaptive && adaptive_request.host.enabled
#else
			false
#endif
			);
		if (ret) {
			fprintf(stderr,
				"SPF persistent-hop OPEN rejected: stage=receiver_rate receivers=%u rate=%llu error=%d\n",
				layout.receiver_count, (unsigned long long)hop_request.sample_rate_hz, ret);
			return -EINVAL;
		}
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
		if (hop_adaptive && adaptive_request.host.enabled &&
			(layout.receiver_count!=1 ||
			layout.enabled_scan_mask!=(3U<<(2*adaptive_request.host.rx)))) {
			fprintf(stderr,
				"SPF host adaptive OPEN rejected: stage=rx_layout receivers=%u mask=%08x rx=%u\n",
				layout.receiver_count, layout.enabled_scan_mask, adaptive_request.host.rx);
			return -EINVAL;
		}
#endif
	} else
#endif
	if (tandem_request_bytes != request_bytes && layout.receiver_count != 1U) {
		return -EINVAL;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	/* A single transferred receiver still requires the paired physical PHY. */
	if (!iio_device_find_channel(dev, "voltage2", false) ||
	    !iio_device_find_channel(dev, "voltage3", false)) {
		free(ctx);
		return -EOPNOTSUPP;
	}
	if (request_bytes == SPF_LEGACY_METADATA_REQUEST_BYTES) {
		uint16_t capacity;
		ret = spf_legacy_metadata_decode(request, request_bytes,
			&ctx->observation_interval_samples, &capacity);
		if (ret) {
			free(ctx);
			return ret;
		}
		ctx->tandem.request.observation_capacity = capacity;
		/* No FPGA event claims and, critically, no tandem lease. */
		ctx->tandem.request.event_capacity = 0;
	} else {
		ret = spf_tandem_session_init(&ctx->tandem, request,
			tandem_request_bytes, NULL);
		if (ret) {
			fprintf(stderr,
				"SPF metadata OPEN rejected: stage=tandem_init error=%d\n",
				ret);
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
	}
	ctx->burst_enabled = request_bytes == tandem_request_bytes +
		SPF_DDR_BURST_REQUEST_BYTES;
	ctx->ring_enabled = request_bytes == tandem_request_bytes +
		SPF_DDR_RING_REQUEST_BYTES;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	ctx->hop_enabled = hop_enabled;
	ctx->hop_adaptive = hop_adaptive;
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	if (hop_adaptive) ctx->adaptive_request = adaptive_request;
#endif
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
	ret = ctx->tandem_initialized ?
		spf_tandem_request_observation_interval(&ctx->tandem.request,
		(uint32_t)samples_count, &ctx->observation_interval_samples) : 0;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (!ret && ctx->hop_enabled)
		ret = spf_sampler_queued_observation_interval((uint32_t)samples_count,
			ctx->observation_interval_samples, &ctx->observation_interval_samples);
#endif
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
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
		if (ctx->hop_adaptive) {
			ret = spf_hop_adaptive_policy_create(&ctx->policy, &ctx->adaptive_request);
			if (!ret)
				ret = spf_hop_device_userspace_v2_open(ctx->rx, ctx->phy, &ctx->tandem,
					&ctx->tandem_lock, &ctx->adaptive_request, spf_hop_adaptive_policy_ports(),
					ctx->policy, &ctx->hop_device_context, &ctx->adaptive_ops);
		} else
#endif
			ret = spf_hop_device_v1_open(ctx->rx, ctx->phy, &ctx->tandem,
				&ctx->tandem_lock, &ctx->hop_request,
				&ctx->hop_device_context, &ctx->hop_ops);
		if (ret) {
			fprintf(stderr, "SPF persistent-hop OPEN rejected: stage=%s error=%d\n",
				ctx->hop_adaptive ? "adaptive_device" : "fixed_device", ret);
			iiod_buffer_metadata_close(ctx);
			return ret;
		}
		ctx->hop_device_opened = true;
		ret = ctx->hop_adaptive ? spf_hop_session_v2_init(&ctx->adaptive_session,
			&ctx->adaptive_request, ctx->adaptive_ops, ctx->hop_device_context) :
			spf_hop_session_v1_init(&ctx->hop_session,
				&ctx->hop_request, ctx->hop_ops, ctx->hop_device_context);
		if (ret) {
			fprintf(stderr, "SPF persistent-hop OPEN rejected: stage=session_init error=%d\n", ret);
			iiod_buffer_metadata_close(ctx);
			return ret;
		}
		ctx->hop_session_initialized = true;
	}
#endif

	*provider_context = ctx;
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	if (ctx->hop_adaptive && ctx->adaptive_request.host.enabled)
		burst_plan->submit_feedback=iiod_buffer_metadata_feedback;
#endif
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
				SIZE_MAX - hop_sidecar_capacity(ctx)) {
			iiod_buffer_metadata_close(ctx);
			*provider_context = NULL;
			return -EOVERFLOW;
		}
		burst_plan->metadata_capacity += hop_sidecar_capacity(ctx);
	}
#endif
#ifdef IIOD_HAS_SCANNER_GLRT
	if (glrt_enabled) {
		/* Before the acquisition buffer is created: all allocation, artifact
		 * validation and worker startup must finish outside the refill path. */
		if (burst_plan->metadata_capacity >
				65536U - LEO_SCANNER_GLRT_FRAME_MAX_OVERHEAD) {
			ret = -EOVERFLOW;
			goto glrt_open_failed;
		}
		ctx->glrt_legacy_capacity = burst_plan->metadata_capacity;
		ctx->glrt_legacy_metadata = malloc(ctx->glrt_legacy_capacity);
		if (!ctx->glrt_legacy_metadata) {
			ret = -ENOMEM;
			goto glrt_open_failed;
		}
		ret = spf_scanner_glrt_open(&ctx->glrt, &glrt_request,
			&ctx->hop_request, samples_count);
		if (ret)
			goto glrt_open_failed;
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
		if (ctx->hop_adaptive) {
			ret = spf_scanner_glrt_attach_policy(ctx->glrt, ctx->policy);
			if (ret) goto glrt_open_failed;
		}
#endif
		burst_plan->metadata_capacity += LEO_SCANNER_GLRT_FRAME_MAX_OVERHEAD;
		burst_plan->drain_metadata = scanner_glrt_drain;
	}
#endif
	return 0;
#ifdef IIOD_HAS_SCANNER_GLRT
glrt_open_failed:
	iiod_buffer_metadata_close(ctx);
	*provider_context = NULL;
	return ret;
#endif
}

int iiod_buffer_metadata_buffer_opened(void *provider_context,
		unsigned int kernel_buffers_count)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	struct spf_sampler_coverage_plan coverage;
	int ret;

	if (!ctx || !kernel_buffers_count)
		return -EINVAL;
	if (ctx->scan_enabled)
		return kernel_buffers_count == ctx->scan_kernel_buffers ? 0 : -ENOSPC;
	if (ctx->counter_only)
		return 0;
	ret = spf_sampler_coverage_plan_compute(ctx->samples_per_channel,
		ctx->observation_interval_samples, kernel_buffers_count,
		SPF_GAIN_SAMPLER_RING_CAPACITY, &coverage);
	if (ret)
		return ret;
	ret = ctx->tandem_initialized ? tandem_acquire(ctx) : 0;
	if (ret)
		return ret;
	ctx->sampler_coverage_window_samples = coverage.window_samples;
	ctx->refills_started = 0;
	spf_gain_sampler_limit(&ctx->sampler,
		ctx->sampler_coverage_window_samples);
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		pthread_mutex_lock(&ctx->hop_lock);
		/* Opening/enabling DMA does not prove the first usable IQ has
		 * arrived. Starting here races startup latency and optional discarded
		 * frames, letting the first valid dwell precede delivered IQ. */
		ret = ctx->hop_adaptive ? spf_hop_session_v2_arm(&ctx->adaptive_session) :
			spf_hop_session_v1_arm(&ctx->hop_session);
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
	if (ctx->scan_enabled)
		return 0;
	if (ctx->counter_only)
		return 0;
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
	if (ctx->scan_enabled)
		return 0;
	if (ctx->counter_only)
		return 0;
	if (ctx->refills_started > 1 &&
		!spf_gain_sampler_finish_capture(
			&ctx->sampler, SPF_IIOD_SAMPLER_START_TIMEOUT_MS))
		return -ETIMEDOUT;
	/* Every completed refill proves the owner is alive, including frames that
	 * metadata_get() subsequently discards during sampler startup.
	 */
	return ctx->tandem_initialized ? tandem_heartbeat(ctx) : 0;
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
	if (ctx->scan_enabled) {
		struct spf_scan_session_output output;
		struct spf_scan_terminal terminal;

		(void)iiod_buffer_metadata_scan_cancel(ctx);
		pthread_mutex_lock(&ctx->scan_lock);
		while (!spf_scan_session_take_output(ctx->scan_session, &output))
			(void)spf_scan_session_complete_output(ctx->scan_session,
						       output.record.visit);
		(void)spf_scan_session_terminal(ctx->scan_session, &terminal);
		(void)spf_scan_session_destroy(ctx->scan_session);
		pthread_mutex_unlock(&ctx->scan_lock);
		pthread_mutex_destroy(&ctx->scan_lock);
		close(ctx->counter_fd);
		free(ctx);
		return;
	}
	if (ctx->counter_only) {
		close(ctx->counter_fd);
		free(ctx);
		return;
	}
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_session_initialized) {
		pthread_mutex_lock(&ctx->hop_lock);
		(void)hop_cancel(ctx, SPF_HOP_REASON_CLIENT_CLOSE);
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
	if (ctx->hop_device_opened) {
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
		if (ctx->hop_adaptive) spf_hop_device_userspace_v2_destroy(ctx->hop_device_context);
		else
#endif
			spf_hop_device_v1_destroy(ctx->hop_device_context);
	}
	if (ctx->tandem_lock_initialized)
		pthread_mutex_destroy(&ctx->tandem_lock);
	if (ctx->hop_lock_initialized)
		pthread_mutex_destroy(&ctx->hop_lock);
#endif
#ifdef IIOD_HAS_SCANNER_GLRT
	spf_scanner_glrt_close(ctx->glrt);
	free(ctx->glrt_legacy_metadata);
#endif
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	/* Both hop thread and acquisition owner/worker are stopped before free. */
	spf_hop_adaptive_policy_destroy(ctx->policy);
#endif
	free(ctx);
}

static void *scan_scheduler(void *opaque)
{
	struct spf_iiod_metadata_context *ctx = opaque;
	uint64_t boundary = 0, now = 0;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 500000 };
	int ret;

	pthread_mutex_lock(&ctx->scan_lock);
	ret = spf_scan_session_next_boundary(ctx->scan_session, &boundary);
	if (ret) {
		(void)spf_scan_session_fail(ctx->scan_session,
					    ctx->scan_counter_anchor, ret);
		ctx->scan_error = ret;
		ctx->scan_finished = true;
		pthread_mutex_unlock(&ctx->scan_lock);
		return NULL;
	}
	pthread_mutex_unlock(&ctx->scan_lock);

	for (;;) {
		pthread_mutex_lock(&ctx->scan_lock);
		ret = spf_scan_radio_snapshot(&ctx->scan_radio,
					      ctx->scan_counter_anchor, &now);
		if (!ret)
			ctx->scan_counter_anchor = now;
		if (ret) {
			(void)spf_scan_session_fail(ctx->scan_session,
						    ctx->scan_counter_anchor, ret);
			ctx->scan_error = ret;
			ctx->scan_finished = true;
			pthread_mutex_unlock(&ctx->scan_lock);
			break;
		}
		if (ctx->scan_cancel_requested) {
			ret = spf_scan_session_cancel(ctx->scan_session, now);
			ctx->scan_error = ret && ret != -EINVAL ? ret : -ECANCELED;
			ctx->scan_finished = true;
			pthread_mutex_unlock(&ctx->scan_lock);
			break;
		}
		if (!boundary || now >= boundary) {
			struct spf_scan_choice choice;

			ret = spf_scan_session_schedule(ctx->scan_session, now, now,
							&choice);
			if (ret == -ENODATA) {
				ret = spf_scan_session_stop(ctx->scan_session, now);
				ctx->scan_error = ret;
				ctx->scan_finished = true;
				pthread_mutex_unlock(&ctx->scan_lock);
				break;
			}
			if (ret) {
				ctx->scan_error = ret;
				ctx->scan_finished = true;
				pthread_mutex_unlock(&ctx->scan_lock);
				break;
			}
			ret = spf_scan_session_next_boundary(ctx->scan_session,
							     &boundary);
			if (ret) {
				(void)spf_scan_session_fail(ctx->scan_session, now, ret);
				ctx->scan_error = ret;
				ctx->scan_finished = true;
				pthread_mutex_unlock(&ctx->scan_lock);
				break;
			}
		}
		pthread_mutex_unlock(&ctx->scan_lock);
		(void)nanosleep(&pause, NULL);
	}
	return NULL;
}

bool iiod_buffer_metadata_scan_enabled(void *provider_context)
{
	const struct spf_iiod_metadata_context *ctx = provider_context;

	return ctx && ctx->scan_enabled;
}

int iiod_buffer_metadata_scan_start(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	if (ctx->scan_started) {
		pthread_mutex_unlock(&ctx->scan_lock);
		return -EALREADY;
	}
	ctx->scan_started = true;
	pthread_mutex_unlock(&ctx->scan_lock);
	return 0;
}

int iiod_buffer_metadata_scan_feed(void *provider_context,
		struct iio_buffer_block *block, size_t raw_bytes)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	struct spf_buffer_sequence_result sequence;
	struct spf_scan_choice first_choice;
	const uint8_t *raw;
	uint64_t first;
	bool sequence_committed = false;
	int ret;

	if (!ctx || !ctx->scan_enabled || !block)
		return -EINVAL;
	if (raw_bytes != ctx->layout.raw_bytes)
		return -EIO;
	raw = iio_buffer_block_start(block);
	if (!raw)
		return -EIO;
	first = spf_counter_read64(raw);
	if (first < 2)
		return -ERANGE;
	first -= 2;
	ret = spf_buffer_sequence_resolve(&ctx->sequence, first,
					  ctx->samples_per_channel, &sequence);
	if (ret)
		return ret;
	pthread_mutex_lock(&ctx->scan_lock);
	if (!ctx->scan_started ||
	    (ctx->scan_finished &&
	     spf_scan_session_capture_complete(ctx->scan_session)))
		ret = -ESHUTDOWN;
	else if (!ctx->scan_thread_live && !ctx->scan_finished) {
		/* The owner ioctls expose only the coherent low counter word.  The
		 * first completed DMA block supplies its unambiguous 64-bit epoch.
		 * Rebase and retune after that block, so pre-retune IQ can never be
		 * attributed to visit zero. */
		ret = spf_scan_session_rebase(ctx->scan_session,
				first + ctx->samples_per_channel);
		if (!ret)
			ret = spf_scan_session_counter(ctx->scan_session,
						       &ctx->scan_counter_anchor);
		if (!ret)
			ret = spf_scan_session_schedule(ctx->scan_session,
				ctx->scan_counter_anchor, ctx->scan_counter_anchor,
				&first_choice);
		if (!ret)
			ret = spf_scan_session_feed(ctx->scan_session,
				(uintptr_t)block, raw + 8, first,
				ctx->samples_per_channel);
		if (!ret) {
			spf_buffer_sequence_commit(&ctx->sequence, &sequence);
			sequence_committed = true;
			ret = pthread_create(&ctx->scan_thread, NULL,
					     scan_scheduler, ctx);
			if (!ret)
				ctx->scan_thread_live = true;
			else
				ret = -ret;
		}
		if (ret) {
			(void)spf_scan_session_fail(ctx->scan_session,
				ctx->scan_counter_anchor, ret);
			ctx->scan_error = ret;
			ctx->scan_finished = true;
		}
	} else
		ret = spf_scan_session_feed(ctx->scan_session, (uintptr_t)block,
				raw + 8, first, ctx->samples_per_channel);
	if (!ret && !sequence_committed)
		spf_buffer_sequence_commit(&ctx->sequence, &sequence);
	pthread_mutex_unlock(&ctx->scan_lock);
	return ret;
}

int iiod_buffer_metadata_scan_take(void *provider_context,
		struct spf_scan_session_output *output)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	int ret;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	ret = spf_scan_session_take_output(ctx->scan_session, output);
	pthread_mutex_unlock(&ctx->scan_lock);
	return ret;
}

int iiod_buffer_metadata_scan_complete(void *provider_context, uint64_t visit)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	int ret;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	ret = spf_scan_session_complete_output(ctx->scan_session, visit);
	pthread_mutex_unlock(&ctx->scan_lock);
	return ret;
}

int iiod_buffer_metadata_scan_abort(void *provider_context, uint64_t visit,
		int transport_error)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	int ret;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	ret = spf_scan_session_abort_output(ctx->scan_session, visit,
					     transport_error);
	ctx->scan_cancel_requested = true;
	pthread_mutex_unlock(&ctx->scan_lock);
	return ret;
}

enum spf_scan_feedback_result iiod_buffer_metadata_scan_feedback(
		void *provider_context, const struct spf_scan_feedback *feedback)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	enum spf_scan_feedback_result result;
	uint64_t now;
	int ret;

	if (!ctx || !ctx->scan_enabled)
		return SPF_SCAN_REJECTED;
	pthread_mutex_lock(&ctx->scan_lock);
	/* The scheduler and DMA feed already advance the session's coherent
	 * source-time watermark under this mutex. A second owner ioctl from the
	 * feedback TCP path races the active producer without adding temporal
	 * information: delivered feedback necessarily follows its closed dwell. */
	ret = spf_scan_session_counter(ctx->scan_session, &now);
	if (!ret)
		result = spf_scan_session_feedback(ctx->scan_session, feedback, now);
	else
		result = SPF_SCAN_REJECTED;
	pthread_mutex_unlock(&ctx->scan_lock);
	return result;
}

int iiod_buffer_metadata_scan_take_ack(void *provider_context,
		struct spf_scan_ack *ack)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	int ret;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	ret = spf_scan_session_take_ack(ctx->scan_session, ack);
	pthread_mutex_unlock(&ctx->scan_lock);
	return ret;
}

int iiod_buffer_metadata_scan_terminal(void *provider_context,
		struct spf_scan_terminal *terminal)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	int ret;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	ret = spf_scan_session_terminal(ctx->scan_session, terminal);
	pthread_mutex_unlock(&ctx->scan_lock);
	return ret;
}

int iiod_buffer_metadata_scan_cancel(void *provider_context)
{
	struct spf_iiod_metadata_context *ctx = provider_context;
	bool join;

	if (!ctx || !ctx->scan_enabled)
		return -EOPNOTSUPP;
	pthread_mutex_lock(&ctx->scan_lock);
	ctx->scan_cancel_requested = true;
	join = ctx->scan_thread_live;
	ctx->scan_thread_live = false;
	if (!ctx->scan_started || (!join && !ctx->scan_finished)) {
		(void)spf_scan_session_cancel(ctx->scan_session,
					      ctx->scan_counter_anchor);
		ctx->scan_finished = true;
	}
	pthread_mutex_unlock(&ctx->scan_lock);
	if (join)
		(void)pthread_join(ctx->scan_thread, NULL);
	return 0;
}

int iiod_buffer_metadata_scan_capabilities(void *wire, size_t bytes)
{
	struct spf_scan_caps caps;
	struct spf_scan_radio radio;
	int fd, ret;

	fd = open("/dev/tandem-agc-events", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ret = spf_scan_radio_init(&radio, fd, NULL);
	close(fd);
	if (ret)
		return ret;
	spf_scan_caps_default(&caps);
	return spf_scan_caps_encode(wire, bytes, &caps);
}

ssize_t iiod_buffer_metadata_get(void *provider_context,
		const struct iio_device *dev, const struct iio_buffer *buffer,
		size_t raw_bytes, void *metadata, size_t metadata_capacity,
		size_t *iq_offset, size_t *iq_bytes)
{
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
	uint64_t callback_start = scanner_callback_clock();
#endif
	struct spf_iiod_metadata_context *ctx = provider_context;
	spf_gain_observation_v3_t observations[SPF_IIOD_OBSERVATION_CAPACITY];
	struct adi_tandem_agc_event events[SPF_TANDEM_EVENT_QUEUE_CAPACITY];
	struct adi_tandem_agc_status tandem_status = {0};
	uint32_t observation_overflow_count = 0;
	uint64_t first_sample_sequence;
	struct spf_buffer_sequence_result sequence;
	spf_rssi_pair_t rssi_start;
	spf_rssi_pair_t rssi_end;
	uint32_t rssi_overflow_count = 0;
	uint16_t observation_count;
	size_t event_count = 0;
	spf_gain_frame_decision_t frame_decision;
	size_t header_bytes;
	size_t total_metadata_bytes;
	const uint8_t *raw;
#ifdef IIOD_HAS_SCANNER_GLRT
	void *frame_metadata = metadata;
	size_t frame_capacity = metadata_capacity;
#endif
	int ret;

	if (!ctx || dev != ctx->rx || !buffer || !metadata || !iq_offset ||
		!iq_bytes)
		return -EINVAL;
	if (ctx->scan_enabled)
		return -EOPNOTSUPP;
	if (ctx->counter_only) {
		if (raw_bytes != ctx->layout.raw_bytes)
			return -EIO;
		raw = iio_buffer_start(buffer);
		if (!raw)
			return -EIO;
		first_sample_sequence = spf_counter_read64(raw);
		/* util_cpack2 emits two RX0 samples after the counter advanced twice.
		 * SPFC1 defines the counter of the first sample, not the packing edge.
		 * Keep the paired ABI's existing timestamp interpretation unchanged. */
		if (first_sample_sequence < 2)
			return -ERANGE;
		first_sample_sequence -= 2;
		ret = spf_buffer_sequence_resolve(&ctx->sequence, first_sample_sequence,
						  ctx->samples_per_channel, &sequence);
		if (ret)
			return ret;
		ret = spf_counter_frame_build(metadata, metadata_capacity, ctx->stream_id,
					      sequence.buffer_sequence, first_sample_sequence,
					      sequence.missing_samples_before,
					      ctx->samples_per_channel, ctx->counter_rate);
		if (ret < 0)
			return ret;
		spf_buffer_sequence_commit(&ctx->sequence, &sequence);
		ctx->frames_emitted++;
		*iq_offset = 8;
		*iq_bytes = ctx->layout.iq_bytes;
		return ret;
	}
#ifdef IIOD_HAS_SCANNER_GLRT
	if (ctx->glrt) {
		if (metadata_capacity < ctx->glrt_legacy_capacity +
				LEO_SCANNER_GLRT_FRAME_MAX_OVERHEAD)
			return -ENOSPC;
		metadata = ctx->glrt_legacy_metadata;
		metadata_capacity = ctx->glrt_legacy_capacity;
	}
#endif
	header_bytes = spf_radio_frame_v5_header_bytes(
		(uint16_t)ctx->tandem.request.observation_capacity,
		(uint16_t)ctx->tandem.request.event_capacity);
	total_metadata_bytes = header_bytes;
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
	if (ctx->hop_enabled) {
		if (header_bytes > SIZE_MAX - hop_sidecar_capacity(ctx))
			return -EOVERFLOW;
		total_metadata_bytes += hop_sidecar_capacity(ctx);
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
		fprintf(stderr, "SPF metadata startup discard: first=%llu samples=%u\n",
			(unsigned long long)first_sample_sequence, ctx->samples_per_channel);
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
	ret = ctx->tandem_initialized ?
		tandem_collect(ctx, first_sample_sequence, events, &event_count,
			&tandem_status) : 0;
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
			.metadata_features = ctx->tandem_initialized ?
				SPF_META_REQUIRED_FEATURES_V6 : SPF_META_LEGACY_FEATURES_V6,
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
		.tandem_status = ctx->tandem_initialized ? &tandem_status : NULL,
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
		struct spf_hop_sidecar_v2 adaptive_sidecar;
		const struct spf_hop_sidecar_v1 *actual;
		int sidecar_bytes;

		pthread_mutex_lock(&ctx->hop_lock);
		if (hop_status_state(ctx)->state == SPF_HOP_STATE_ARMED) {
			/* This frame's IQ, timestamp, gain and RSSI have all passed
			 * validation. Start the device-local scheduler only now; its
			 * real transition counter remains the scan's time authority. */
			ret = ctx->hop_adaptive ? spf_hop_session_v2_start(&ctx->adaptive_session) :
				spf_hop_session_v1_start(&ctx->hop_session);
			fprintf(stderr, "SPF persistent-hop DMA-ready start: first=%llu "
				"discarded=%u error=%d\n", (unsigned long long)first_sample_sequence,
				ctx->startup_frames_discarded, ret);
			if (ret) {
				pthread_mutex_unlock(&ctx->hop_lock);
				return ret;
			}
		}
		if (ctx->hop_adaptive) {
			ret = spf_hop_session_v2_on_block(&ctx->adaptive_session,
				sequence.buffer_sequence, first_sample_sequence,
				first_sample_sequence + ctx->samples_per_channel, &adaptive_sidecar);
			actual = &adaptive_sidecar.geometry;
		} else {
			ret = spf_hop_session_v1_on_block(&ctx->hop_session,
				sequence.buffer_sequence, first_sample_sequence,
				first_sample_sequence + ctx->samples_per_channel, &sidecar);
			actual = &sidecar;
		}
		if (!ret && ctx->hop_adaptive)
			sidecar_bytes = (ctx->adaptive_request.host.enabled ?
				spf_hop_sidecar_v3_encode : spf_hop_sidecar_v2_encode)(
				(uint8_t *)metadata + header_bytes,
				metadata_capacity - header_bytes, &adaptive_sidecar);
		else if (!ret)
			sidecar_bytes = spf_hop_sidecar_v1_encode(
				(uint8_t *)metadata + header_bytes,
				metadata_capacity - header_bytes, &sidecar);
		else {
			const struct spf_hop_status_v1 *status = hop_status_state(ctx);
			fprintf(stderr,
				"SPF persistent-hop block failed: error=%d state=%u reason=%u "
				"session_error=%d block=%llu first=%llu end=%llu visits=%llu "
				"events=%llu\n", ret, status->state,
				status->terminal_reason,
				status->error_code,
				(unsigned long long)sequence.buffer_sequence,
				(unsigned long long)first_sample_sequence,
				(unsigned long long)(first_sample_sequence +
					ctx->samples_per_channel),
				(unsigned long long)status->visits_started,
				(unsigned long long)status->events_emitted);
			sidecar_bytes = ret;
		}
#ifdef IIOD_HAS_SCANNER_GLRT
		if (sidecar_bytes >= 0)
			spf_scanner_glrt_begin_frame(ctx->glrt);
#endif
		pthread_mutex_unlock(&ctx->hop_lock);
		if (sidecar_bytes < 0)
			return sidecar_bytes;
		total_metadata_bytes = header_bytes + (size_t)sidecar_bytes;
#ifdef IIOD_HAS_SCANNER_GLRT
		/* Sidecar events may refer to an earlier block. The bounded collector
		 * keeps original counter identity and never retains this DMA buffer. */
		spf_scanner_glrt_feed(ctx->glrt, actual,
			(const int16_t *)(raw + sizeof(first_sample_sequence)),
			ctx->samples_per_channel);
#endif
		(void)actual;
	} else
#endif
	{
		total_metadata_bytes = header_bytes;
	}

	spf_buffer_sequence_commit(&ctx->sequence, &sequence);
	ctx->frames_emitted++;
	*iq_offset = sizeof(first_sample_sequence);
	*iq_bytes = ctx->layout.iq_bytes;
#ifdef IIOD_HAS_SCANNER_GLRT
	if (ctx->glrt) {
		ssize_t bytes = spf_scanner_glrt_frame(ctx->glrt, metadata, total_metadata_bytes,
			frame_metadata, frame_capacity);
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
		uint64_t callback_end = scanner_callback_clock();
		spf_scanner_glrt_capture_budget(ctx->glrt,
			callback_start == UINT64_MAX || callback_end == UINT64_MAX || callback_end < callback_start ?
				UINT64_MAX : callback_end - callback_start, sequence.missing_samples_before);
#endif
		return bytes;
	}
#endif
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
	hop_status = *hop_status_state(ctx);
	ret = ctx->hop_adaptive ? (ctx->adaptive_request.host.enabled ?
		spf_hop_status_v3_encode : spf_hop_status_v2_encode)(status, status_capacity, &hop_status) :
		spf_hop_status_v1_encode(status, status_capacity, &hop_status);
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
	ret = hop_cancel(ctx, SPF_HOP_REASON_CLIENT_CLOSE);
	pthread_mutex_unlock(&ctx->hop_lock);
#ifdef IIOD_HAS_SCANNER_GLRT
	spf_scanner_glrt_finish(ctx->glrt, 1);
#endif
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

#ifdef IIOD_HAS_SCANNER_GLRT
	if (ctx && ctx->glrt) {
		const uint8_t *legacy;
		int ret = leo_scanner_glrt_legacy_view(metadata, metadata_bytes,
			&legacy, &metadata_bytes);
		if (ret)
			return ret;
		metadata = legacy;
		record = metadata;
	}
#endif

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
		struct spf_hop_sidecar_v2 adaptive_sidecar;
		const struct spf_hop_sidecar_v1 *actual;
		int ret;

		if (record->header_bytes == metadata_bytes)
			return -EBADMSG;
		if (ctx->hop_adaptive) {
			ret = (ctx->adaptive_request.host.enabled ?
				spf_hop_sidecar_v3_decode : spf_hop_sidecar_v2_decode)(&adaptive_sidecar,
				(const uint8_t *)metadata + record->header_bytes,
				metadata_bytes - record->header_bytes);
			if (!ret) {
				for (unsigned i = 0; i < adaptive_sidecar.geometry.event_count; ++i)
					if (adaptive_sidecar.choices[i].generation != ctx->adaptive_request.policy.generation ||
						adaptive_sidecar.choices[i].mode != ctx->adaptive_request.policy.mode)
						return -EBADMSG;
			}
			actual = &adaptive_sidecar.geometry;
		} else {
			ret = spf_hop_sidecar_v1_decode(&sidecar,
				(const uint8_t *)metadata + record->header_bytes,
				metadata_bytes - record->header_bytes);
			actual = &sidecar;
		}
		if (ret || actual->session_id != ctx->hop_request.session_id ||
			actual->buffer_sequence != record->buffer_sequence ||
			actual->block_first_sample != record->first_sample_sequence ||
			actual->block_end_sample != record->first_sample_sequence +
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
	if (ctx && ctx->counter_only)
		return spf_counter_frame_describe(metadata, metadata_bytes, info);
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

	if (ctx && ctx->counter_only)
		return spf_counter_frame_rebase(metadata, metadata_bytes, previous_frame_end);
	ret = spf_exact_gap_header(ctx, metadata, metadata_bytes, &const_header);
	if (ret)
		return ret;
	return spf_radio_frame_v6_rebase_gap((void *)const_header, const_header->header_bytes,
		previous_frame_end) ? 0 : -ERANGE;
}

int iiod_buffer_metadata_feedback(void *provider_context, const void *feedback, size_t bytes)
{
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	struct spf_iiod_metadata_context *ctx=provider_context;
	struct spf_hop_host_feedback_v1 f;
	int ret;
	if (!ctx || !ctx->hop_adaptive || !ctx->adaptive_request.host.enabled) return -ENOTSUP;
	ret=bytes>=4 && !memcmp(feedback,"HFB2",4) ?
		spf_hop_host_feedback_v2_decode(&f,feedback,bytes) :
		spf_hop_host_feedback_v1_decode(&f,feedback,bytes);
	if (ret) return ret;
	pthread_mutex_lock(&ctx->hop_lock);
	const struct spf_hop_status_v1 *status=hop_status_state(ctx);
	if (status->state!=SPF_HOP_STATE_RUNNING) ret=-ESHUTDOWN;
	else ret=spf_hop_adaptive_policy_offer_host(ctx->policy,&f,ctx->stream_id,status->last_block_end);
	pthread_mutex_unlock(&ctx->hop_lock);
	return ret;
#else
	(void)provider_context; (void)feedback; (void)bytes;
	return -ENOTSUP;
#endif
}
