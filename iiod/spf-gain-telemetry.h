/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_GAIN_TELEMETRY_H
#define SPF_GAIN_TELEMETRY_H

#include <spf_gain_sampler.h>

#include <stdbool.h>
#include <stdint.h>

enum spf_gain_telemetry_failure {
	SPF_GAIN_TELEMETRY_OK = 0,
	SPF_GAIN_TELEMETRY_START_FAILED,
	SPF_GAIN_TELEMETRY_COVERAGE_UNAVAILABLE,
	SPF_GAIN_TELEMETRY_START_FENCE_TIMEOUT,
	SPF_GAIN_TELEMETRY_FINISH_FENCE_TIMEOUT,
};

struct spf_gain_telemetry {
	spf_gain_sampler_t sampler;
	enum spf_gain_telemetry_failure failure;
	int failure_error;
	bool optional;
	bool thread_started;
	bool usable;
	bool diagnostic_reported;
};

void spf_gain_telemetry_init(struct spf_gain_telemetry *telemetry,
	bool optional);
int spf_gain_telemetry_start(struct spf_gain_telemetry *telemetry,
	uint32_t interval_samples);
int spf_gain_telemetry_degrade(struct spf_gain_telemetry *telemetry,
	enum spf_gain_telemetry_failure failure, int error);
bool spf_gain_telemetry_is_usable(
	const struct spf_gain_telemetry *telemetry);
bool spf_gain_telemetry_take_diagnostic(struct spf_gain_telemetry *telemetry,
	enum spf_gain_telemetry_failure *failure, int *error);
const char *spf_gain_telemetry_failure_name(
	enum spf_gain_telemetry_failure failure);

void spf_gain_telemetry_limit(struct spf_gain_telemetry *telemetry,
	uint64_t samples);
void spf_gain_telemetry_unlimit(struct spf_gain_telemetry *telemetry);
int spf_gain_telemetry_wait_started(struct spf_gain_telemetry *telemetry,
	uint32_t timeout_ms);
int spf_gain_telemetry_limit_and_wait_started(
	struct spf_gain_telemetry *telemetry, uint64_t samples,
	uint32_t timeout_ms);
int spf_gain_telemetry_finish_capture(struct spf_gain_telemetry *telemetry,
	uint32_t timeout_ms);

uint16_t spf_gain_telemetry_collect(struct spf_gain_telemetry *telemetry,
	uint64_t frame_start, uint32_t samples,
	spf_gain_observation_v3_t *destination, uint16_t capacity,
	uint32_t *overflow_count);
bool spf_gain_telemetry_collect_rssi(struct spf_gain_telemetry *telemetry,
	uint64_t frame_start, uint32_t samples, spf_rssi_pair_t *rssi_start,
	spf_rssi_pair_t *rssi_end, uint32_t *overflow_count);
void spf_gain_telemetry_stop(struct spf_gain_telemetry *telemetry);

#endif
