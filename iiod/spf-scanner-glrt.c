/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scanner-glrt.h"
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
#include "spf-hop-adaptive-policy.h"
#endif
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
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
	uint64_t callback_budget_ns;
	uint64_t block_samples;
	int source_pressure;
#endif
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	struct spf_hop_adaptive_policy *policy;
	int acquisition_started;
#endif
};

#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
int spf_scanner_glrt_attach_policy(struct spf_scanner_glrt *s, struct spf_hop_adaptive_policy *p)
{
	int ret = 0;
	if (!s || !p) return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (s->policy || s->acquisition_started || s->finished) ret = -EBUSY;
	else s->policy = p;
	pthread_mutex_unlock(&s->lock);
	return ret;
}

/* Called under the acquisition-owner mutex, never on the scheduler thread.
 * A failed advisory queue does not change frame/IQ delivery. */
static void publish_observations(struct spf_scanner_glrt *s)
{
	unsigned i;
	if (!s->policy) return;
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; ++i) {
		leo_adaptive_observation_v1 observation;
		int ret = leo_scanner_glrt_observation(s->session, &observation);
		if (!ret || ret == -ENODATA) break;
		if (ret != 1 || spf_hop_adaptive_policy_offer(s->policy, &observation)) {
			spf_hop_adaptive_policy_fault(s->policy);
			break;
		}
	}
}
#else
static void publish_observations(struct spf_scanner_glrt *s) { (void)s; }
#endif

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
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
	if (!ret) {
		/* Engineering profile, not a claim of qualified live-duty headroom.
		 * The reviewed release identity must bind this explicit opt-in. */
		const leo_scanner_glrt_protection_v1 protection = {
			.max_occupied_slots = 2, .admission_age_ms = 250,
			.worker_timeout_ms = 500, .recovery_blocks = 4,
		};
		ret = leo_scanner_glrt_enable_protection(state->session, &protection);
		state->callback_budget_ns = (uint64_t)block_samples * UINT64_C(1000000000) /
			config.rate_hz * 4 / 5;
		state->block_samples = block_samples;
	}
#endif
#ifdef IIOD_SCANNER_GLRT_COOPERATIVE_SKIPS
	/* Startup-only opt-in, bound to a new reviewed bundle configuration. A
	 * genuine failure still faults; never reinterpret unavailable wire reasons. */
	if (!ret) ret = leo_scanner_glrt_enable_cooperative_skips(state->session);
#endif
	if (ret) {
		leo_scanner_glrt_close(state->session);
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

#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
void spf_scanner_glrt_capture_budget(struct spf_scanner_glrt *s,
	uint64_t callback_ns, uint64_t missing_samples)
{
	if (!s) return;
	pthread_mutex_lock(&s->lock);
	if (!s->finished) {
		(void)leo_scanner_glrt_capture_pressure(s->session,
			s->source_pressure || missing_samples || callback_ns >= s->callback_budget_ns);
		publish_observations(s);
	}
	pthread_mutex_unlock(&s->lock);
}

int spf_scanner_glrt_protection_stats(struct spf_scanner_glrt *s,
	leo_scanner_glrt_protection_stats_v1 *out)
{
	int ret;
	if (!s || !out) return -EINVAL;
	pthread_mutex_lock(&s->lock);
	ret = leo_scanner_glrt_protection_stats(s->session, out);
	pthread_mutex_unlock(&s->lock);
	return ret;
}

/* A hop timestamp newer than the delivered IQ is a lower bound on source
 * backlog, even when metadata callbacks themselves are fast. Observe the
 * already-attested event: no extra IIO read, clock epoch conversion or wait.
 * Empty carriers cannot prove recovery; wait for a fresh event below the low
 * watermark, then let the existing four healthy callbacks permit admission.
 * This is an advisory early warning, not a guarantee against DMA/network loss. */
static void observe_source_backlog(struct spf_scanner_glrt *s,
	const struct spf_hop_sidecar_v1 *sidecar)
{
	uint64_t latest = 0, lag;
	unsigned int j;
	if (!sidecar->event_count) return;
	for (j = 0; j < sidecar->event_count; ++j)
		if (sidecar->events[j].device.transition_after > latest)
			latest = sidecar->events[j].device.transition_after;
	lag = latest > sidecar->block_end_sample ? latest - sidecar->block_end_sample : 0;
	if (lag >= 2 * s->block_samples) {
		s->source_pressure = 1;
		/* Assert before visit/history collection for this very block. */
		(void)leo_scanner_glrt_capture_pressure(s->session, 1);
	} else if (lag <= s->block_samples) {
		s->source_pressure = 0;
	}
}
#endif

void spf_scanner_glrt_begin_frame(struct spf_scanner_glrt *state)
{
	if (!state)
		return;
	pthread_mutex_lock(&state->lock);
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	state->acquisition_started = 1;
#endif
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
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
		observe_source_backlog(state, sidecar);
#endif
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
	publish_observations(state);
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
	publish_observations(state);
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
