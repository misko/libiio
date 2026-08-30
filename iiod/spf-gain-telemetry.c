/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "spf-gain-telemetry.h"

#include <errno.h>
#include <string.h>

void spf_gain_telemetry_init(struct spf_gain_telemetry *telemetry,
	bool optional)
{
	if (!telemetry)
		return;
	memset(telemetry, 0, sizeof(*telemetry));
	telemetry->optional = optional;
}

int spf_gain_telemetry_degrade(struct spf_gain_telemetry *telemetry,
	enum spf_gain_telemetry_failure failure, int error)
{
	if (!telemetry || failure == SPF_GAIN_TELEMETRY_OK || error >= 0)
		return -EINVAL;
	if (!telemetry->optional)
		return error;
	if (telemetry->failure == SPF_GAIN_TELEMETRY_OK) {
		telemetry->failure = failure;
		telemetry->failure_error = error;
	}
	telemetry->usable = false;
	return 0;
}

int spf_gain_telemetry_start(struct spf_gain_telemetry *telemetry,
	uint32_t interval_samples)
{
	if (!telemetry || !interval_samples)
		return -EINVAL;
	if (!spf_gain_sampler_start(&telemetry->sampler, interval_samples))
		return spf_gain_telemetry_degrade(telemetry,
			SPF_GAIN_TELEMETRY_START_FAILED, -EIO);
	telemetry->thread_started = true;
	telemetry->usable = true;
	return 0;
}

bool spf_gain_telemetry_is_usable(
	const struct spf_gain_telemetry *telemetry)
{
	return telemetry && telemetry->usable;
}

bool spf_gain_telemetry_take_diagnostic(struct spf_gain_telemetry *telemetry,
	enum spf_gain_telemetry_failure *failure, int *error)
{
	if (!telemetry || !failure || !error || telemetry->diagnostic_reported ||
		telemetry->failure == SPF_GAIN_TELEMETRY_OK)
		return false;
	*failure = telemetry->failure;
	*error = telemetry->failure_error;
	telemetry->diagnostic_reported = true;
	return true;
}

const char *spf_gain_telemetry_failure_name(
	enum spf_gain_telemetry_failure failure)
{
	switch (failure) {
	case SPF_GAIN_TELEMETRY_START_FAILED:
		return "sampler-start";
	case SPF_GAIN_TELEMETRY_COVERAGE_UNAVAILABLE:
		return "coverage-admission";
	case SPF_GAIN_TELEMETRY_START_FENCE_TIMEOUT:
		return "refill-start-fence";
	case SPF_GAIN_TELEMETRY_FINISH_FENCE_TIMEOUT:
		return "refill-finish-fence";
	case SPF_GAIN_TELEMETRY_OK:
	default:
		return "none";
	}
}

void spf_gain_telemetry_limit(struct spf_gain_telemetry *telemetry,
	uint64_t samples)
{
	if (spf_gain_telemetry_is_usable(telemetry))
		spf_gain_sampler_limit(&telemetry->sampler, samples);
}

void spf_gain_telemetry_unlimit(struct spf_gain_telemetry *telemetry)
{
	if (spf_gain_telemetry_is_usable(telemetry))
		spf_gain_sampler_unlimit(&telemetry->sampler);
}

int spf_gain_telemetry_wait_started(struct spf_gain_telemetry *telemetry,
	uint32_t timeout_ms)
{
	if (!telemetry)
		return -EINVAL;
	if (!telemetry->usable)
		return telemetry->optional ? 0 : -EIO;
	if (spf_gain_sampler_wait_started(&telemetry->sampler, timeout_ms))
		return 0;
	return spf_gain_telemetry_degrade(telemetry,
		SPF_GAIN_TELEMETRY_START_FENCE_TIMEOUT, -ETIMEDOUT);
}

int spf_gain_telemetry_limit_and_wait_started(
	struct spf_gain_telemetry *telemetry, uint64_t samples,
	uint32_t timeout_ms)
{
	if (!telemetry)
		return -EINVAL;
	if (!telemetry->usable)
		return telemetry->optional ? 0 : -EIO;
	if (!samples)
		return -EINVAL;
	if (spf_gain_sampler_limit_and_wait_started(&telemetry->sampler,
			samples, timeout_ms))
		return 0;
	return spf_gain_telemetry_degrade(telemetry,
		SPF_GAIN_TELEMETRY_START_FENCE_TIMEOUT, -ETIMEDOUT);
}

int spf_gain_telemetry_finish_capture(struct spf_gain_telemetry *telemetry,
	uint32_t timeout_ms)
{
	if (!telemetry)
		return -EINVAL;
	if (!telemetry->usable)
		return telemetry->optional ? 0 : -EIO;
	if (spf_gain_sampler_finish_capture(&telemetry->sampler, timeout_ms))
		return 0;
	return spf_gain_telemetry_degrade(telemetry,
		SPF_GAIN_TELEMETRY_FINISH_FENCE_TIMEOUT, -ETIMEDOUT);
}

uint16_t spf_gain_telemetry_collect(struct spf_gain_telemetry *telemetry,
	uint64_t frame_start, uint32_t samples,
	spf_gain_observation_v3_t *destination, uint16_t capacity,
	uint32_t *overflow_count)
{
	if (!telemetry || !overflow_count)
		return 0;
	if (!telemetry->usable) {
		*overflow_count = 0;
		return 0;
	}
	return spf_gain_sampler_collect(&telemetry->sampler, frame_start,
		samples, destination, capacity, overflow_count);
}

bool spf_gain_telemetry_collect_rssi(struct spf_gain_telemetry *telemetry,
	uint64_t frame_start, uint32_t samples, spf_rssi_pair_t *rssi_start,
	spf_rssi_pair_t *rssi_end, uint32_t *overflow_count)
{
	if (!telemetry || !rssi_start || !rssi_end || !overflow_count)
		return false;
	if (!telemetry->usable) {
		memset(rssi_start, 0, sizeof(*rssi_start));
		memset(rssi_end, 0, sizeof(*rssi_end));
		*overflow_count = 0;
		return false;
	}
	return spf_gain_sampler_collect_rssi(&telemetry->sampler, frame_start,
		samples, rssi_start, rssi_end, overflow_count);
}

void spf_gain_telemetry_stop(struct spf_gain_telemetry *telemetry)
{
	if (!telemetry)
		return;
	if (telemetry->thread_started)
		spf_gain_sampler_stop(&telemetry->sampler);
	telemetry->thread_started = false;
	telemetry->usable = false;
}
