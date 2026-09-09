/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scanner-glrt.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct spf_scanner_glrt {
	leo_scanner_glrt *session;
	pthread_mutex_t lock;
	uint64_t session_id, dwell_samples;
	int finished, frame_pending;
	atomic_int drain_ready;
};

static int digest_matches(const uint8_t digest[32], const char *hex)
{
	static const char digits[] = "0123456789abcdef";
	unsigned int j;
	if (strlen(hex) != 64)
		return 0;
	for (j = 0; j < 32; j++)
		if (hex[j * 2] != digits[digest[j] >> 4] ||
			hex[j * 2 + 1] != digits[digest[j] & 15])
			return 0;
	return 1;
}

int spf_scanner_glrt_validate(const leo_scanner_glrt_request_v1 *request,
	const struct spf_hop_request_v1 *hop, size_t block_samples)
{
	uint64_t maximum;
	unsigned int profile;

	if (!request || !hop || !request->generation || !hop->session_id || request->rx != 1 ||
		(hop->sample_rate_hz != 2500000 && hop->sample_rate_hz != 5000000) ||
		hop->rf_bandwidth_hz != hop->sample_rate_hz || hop->initial_profile != 0 ||
		hop->dwell_samples != hop->sample_rate_hz / 50 * 6 ||
		!hop->capture_span_samples ||
		hop->capture_span_samples > hop->sample_rate_hz * 300 ||
		!block_samples || block_samples > LEO_SCANNER_GLRT_MAX_BLOCK_SAMPLES)
		return -EINVAL;
	if (!digest_matches(request->algorithm_sha256, IIOD_SCANNER_GLRT_ALGORITHM_SHA256) ||
		!digest_matches(request->configuration_sha256, IIOD_SCANNER_GLRT_CONFIGURATION_SHA256))
		return -ESTALE;
	maximum = hop->capture_span_samples / hop->dwell_samples +
		!!(hop->capture_span_samples % hop->dwell_samples);
	if (maximum > hop->dwell_count)
		maximum = hop->dwell_count;
	if (!maximum || maximum > LEO_SCANNER_GLRT_MAX_VISITS)
		return -EINVAL;
	/* The currently supported 2.5/5MHz passbands remain pilot-centred.
	 * Validate the public scanner geometry before assigning channel labels;
	 * a generic profile order must not silently become Starlink evidence. */
	for (profile = 0; profile < SPF_HOP_PROFILE_COUNT; profile++) {
		uint64_t expected = (profile < 4 ? UINT64_C(959687500) :
			UINT64_C(1190312500)) + (profile % 4) * UINT64_C(250000000);
		if (hop->profiles[profile].profile_id != profile ||
			hop->profiles[profile].center_frequency_hz != expected)
			return -EINVAL;
	}
	return 0;
}

int spf_scanner_glrt_open(struct spf_scanner_glrt **output,
	const leo_scanner_glrt_request_v1 *request,
	const struct spf_hop_request_v1 *hop, size_t block_samples)
{
	leo_scanner_glrt_config_v1 config = {0};
	struct spf_scanner_glrt *state;
	uint64_t maximum;
	const char *templates;
	int ret;
	if (!output)
		return -EINVAL;
	ret = spf_scanner_glrt_validate(request, hop, block_samples);
	if (ret)
		return ret;
	maximum = hop->capture_span_samples / hop->dwell_samples +
		!!(hop->capture_span_samples % hop->dwell_samples);
	if (maximum > hop->dwell_count)
		maximum = hop->dwell_count;
	config.session = hop->session_id;
	config.generation = request->generation;
	config.rate_hz = (uint32_t)hop->sample_rate_hz;
	config.rx = request->rx;
	config.maximum_visits = (uint32_t)maximum;
	config.maximum_block_samples = (uint32_t)block_samples;
	memcpy(config.algorithm_sha256, request->algorithm_sha256, 32);
	memcpy(config.configuration_sha256, request->configuration_sha256, 32);
	templates = config.rate_hz == 2500000 ? IIOD_SCANNER_GLRT_TEMPLATES_2500000 :
		IIOD_SCANNER_GLRT_TEMPLATES_5000000;
	state = calloc(1, sizeof(*state));
	if (!state)
		return -ENOMEM;
	ret = pthread_mutex_init(&state->lock, NULL);
	if (ret) { free(state); return -ret; }
#ifdef IIOD_SCANNER_GLRT_POSITIVE_ONLY
	{
		const leo_scanner_glrt_positive_policy_v1 policy = {
			.minimum_exact_score = IIOD_SCANNER_GLRT_MINIMUM_EXACT_SCORE,
			.minimum_margin = IIOD_SCANNER_GLRT_MINIMUM_MARGIN,
		};
		ret = leo_scanner_glrt_open_positive(&state->session, &config,
			IIOD_SCANNER_GLRT_WORKER_PATH, templates, &policy);
	}
#else
	ret = leo_scanner_glrt_open(&state->session, &config,
		IIOD_SCANNER_GLRT_WORKER_PATH, templates);
#endif
	if (ret) {
		pthread_mutex_destroy(&state->lock);
		free(state);
		return ret;
	}
	state->session_id = hop->session_id;
	state->dwell_samples = hop->dwell_samples;
	atomic_init(&state->drain_ready, 0);
	*output = state;
	return 0;
}

void spf_scanner_glrt_begin_frame(struct spf_scanner_glrt *state)
{
	if (!state)
		return;
	pthread_mutex_lock(&state->lock);
	state->frame_pending = 1;
	atomic_store_explicit(&state->drain_ready, 0, memory_order_release);
	pthread_mutex_unlock(&state->lock);
}

void spf_scanner_glrt_feed(struct spf_scanner_glrt *state,
	const struct spf_hop_sidecar_v1 *sidecar, const int16_t *iq, size_t samples)
{
	unsigned int j;
	int ret = 0;
	if (!state)
		return;
	pthread_mutex_lock(&state->lock);
	if (!sidecar || state->finished || sidecar->session_id != state->session_id ||
		sidecar->event_count > SPF_HOP_EVENT_CAPACITY ||
		sidecar->block_end_sample <= sidecar->block_first_sample ||
		sidecar->block_end_sample - sidecar->block_first_sample != samples) {
		ret = -EINVAL;
	} else {
		for (j = 0; !ret && j < sidecar->event_count; j++) {
			const struct spf_hop_event_v1 *event = &sidecar->events[j];
			/* This opt-in scanner mode follows the published eight-profile
			 * CH1L..CH4L, CH1U..CH4U plan; generic hop sessions do not opt in. */
			if (event->device.to_profile >= 8 ||
				event->invalid_end > UINT64_MAX - state->dwell_samples) {
				ret = -EINVAL;
				break;
			}
			ret = leo_scanner_glrt_visit(state->session, event->device.dwell_index,
				event->invalid_end, event->invalid_end + state->dwell_samples,
				event->device.to_profile % 4 + 1, event->device.to_profile / 4);
		}
		if (!ret)
			ret = leo_scanner_glrt_block(state->session,
				sidecar->block_first_sample, iq, samples, 4, 2);
	}
	if (ret)
		leo_scanner_glrt_fail(state->session);
	if (sidecar && sidecar->state >= SPF_HOP_STATE_COMPLETED && !state->finished) {
		(void)leo_scanner_glrt_finish(state->session,
			sidecar->state != SPF_HOP_STATE_COMPLETED);
		state->finished = 1;
	}
	pthread_mutex_unlock(&state->lock);
}

void spf_scanner_glrt_finish(struct spf_scanner_glrt *state, int cancelled)
{
	if (!state)
		return;
	pthread_mutex_lock(&state->lock);
	(void)leo_scanner_glrt_finish(state->session, cancelled);
	state->finished = 1;
	if (!state->frame_pending)
		atomic_store_explicit(&state->drain_ready, 1, memory_order_release);
	pthread_mutex_unlock(&state->lock);
}

ssize_t spf_scanner_glrt_frame(struct spf_scanner_glrt *state, const void *legacy,
	size_t legacy_bytes, void *output, size_t capacity)
{
	ssize_t ret;
	if (!state)
		return -EINVAL;
	pthread_mutex_lock(&state->lock);
	ret = leo_scanner_glrt_frame(state->session, legacy, legacy_bytes, output, capacity);
	if (ret > 0) {
		state->frame_pending = 0;
		if (state->finished)
			atomic_store_explicit(&state->drain_ready, 1, memory_order_release);
	}
	pthread_mutex_unlock(&state->lock);
	return ret;
}

ssize_t spf_scanner_glrt_drain(struct spf_scanner_glrt *state, void *output, size_t capacity)
{
	ssize_t ret;
	if (!state)
		return -ENODATA;
	/* Active-capture drains do not acquire the collector mutex at all.
	 * In particular, FINAL may not overtake the terminal IQ envelope between
	 * feed() observing hop completion and frame() publishing its metadata. */
	if (!atomic_load_explicit(&state->drain_ready, memory_order_acquire))
		return -EBUSY;
	if (pthread_mutex_trylock(&state->lock))
		return -EBUSY;
	ret = leo_scanner_glrt_drain(state->session, output, capacity);
	pthread_mutex_unlock(&state->lock);
	return ret;
}

void spf_scanner_glrt_close(struct spf_scanner_glrt *state)
{
	if (state) {
		leo_scanner_glrt_close(state->session);
		pthread_mutex_destroy(&state->lock);
		free(state);
	}
}
