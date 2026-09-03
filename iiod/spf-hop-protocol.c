/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-hop-protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_le64(const uint8_t *p)
{
	return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

static void put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *p, uint64_t value)
{
	put_le32(p, (uint32_t)value);
	put_le32(p + 4, (uint32_t)(value >> 32));
}

static bool all_zero(const uint8_t *p, size_t bytes)
{
	while (bytes--)
		if (*p++)
			return false;
	return true;
}

static bool center_matches_lo(uint64_t center, uint64_t lo, int64_t offset)
{
	uint64_t magnitude;

	if (offset >= 0) {
		magnitude = (uint64_t)offset;
		return lo <= UINT64_MAX - magnitude && center == lo + magnitude;
	}
	magnitude = (uint64_t)(-(offset + 1)) + UINT64_C(1);
	return lo >= magnitude && center == lo - magnitude;
}

static int validate_request(const struct spf_hop_request_v1 *request)
{
	uint16_t slots = 0;
	unsigned int i;

	if (!request || request->required_features != SPF_HOP_REQUIRED_FEATURES_V1 ||
		request->flags != SPF_HOP_REQUEST_FLAGS_V1 ||
		!request->session_id || !request->sample_rate_hz ||
		!request->rf_bandwidth_hz ||
		request->rf_bandwidth_hz > request->sample_rate_hz ||
		!request->dwell_samples || !request->dwell_count ||
		request->capture_span_samples < request->dwell_samples ||
		request->dwell_count > UINT64_MAX / request->dwell_samples ||
		request->capture_span_samples >
			request->dwell_count * request->dwell_samples ||
		request->transition_guard_samples >= request->dwell_samples ||
		request->initial_profile != 0)
		return -EINVAL;
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		const struct spf_hop_profile_v1 *profile = &request->profiles[i];

		if (profile->profile_id != i ||
			profile->fastlock_slot >= SPF_HOP_PROFILE_COUNT ||
			(slots & (UINT16_C(1) << profile->fastlock_slot)) ||
			!profile->center_frequency_hz || !profile->lo_frequency_hz ||
			!profile->profile_crc32 ||
			!center_matches_lo(profile->center_frequency_hz,
				profile->lo_frequency_hz, request->if_offset_hz))
			return -EINVAL;
		slots |= UINT16_C(1) << profile->fastlock_slot;
	}
	return slots == UINT16_C(0xff) ? 0 : -EINVAL;
}

int spf_hop_request_v1_decode(struct spf_hop_request_v1 *request,
	const void *wire, size_t wire_bytes)
{
	const uint8_t *p = wire;
	unsigned int i;

	if (!request || !wire)
		return -EINVAL;
	if (wire_bytes != SPF_HOP_REQUEST_BYTES)
		return -EMSGSIZE;
	if (get_le32(p) != SPF_HOP_REQUEST_MAGIC ||
		get_le16(p + 4) != SPF_HOP_PROTOCOL_VERSION ||
		get_le16(p + 6) != SPF_HOP_REQUEST_BYTES)
		return -EPROTONOSUPPORT;
	if (get_le16(p + 72) != SPF_HOP_PROFILE_COUNT || p[75] ||
		get_le16(p + 76) != SPF_HOP_EVENT_BYTES ||
		get_le16(p + 78) != SPF_HOP_STATUS_BYTES ||
		!all_zero(p + 80, 8))
		return -EBADMSG;
	memset(request, 0, sizeof(*request));
	request->required_features = get_le32(p + 8);
	request->flags = get_le32(p + 12);
	request->session_id = get_le64(p + 16);
	request->sample_rate_hz = get_le64(p + 24);
	request->rf_bandwidth_hz = get_le64(p + 32);
	request->if_offset_hz = (int64_t)get_le64(p + 40);
	request->dwell_samples = get_le64(p + 48);
	request->transition_guard_samples = get_le64(p + 56);
	request->dwell_count = get_le64(p + 64);
	request->capture_span_samples = get_le64(p + 88);
	request->initial_profile = p[74];
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		const uint8_t *entry = p + 96 + i * 24;
		struct spf_hop_profile_v1 *profile = &request->profiles[i];

		if (get_le16(entry + 2))
			return -EBADMSG;
		profile->profile_id = entry[0];
		profile->fastlock_slot = entry[1];
		profile->center_frequency_hz = get_le64(entry + 4);
		profile->lo_frequency_hz = get_le64(entry + 12);
		profile->profile_crc32 = get_le32(entry + 20);
	}
	return validate_request(request);
}

int spf_hop_request_v1_encode(void *wire, size_t wire_bytes,
	const struct spf_hop_request_v1 *request)
{
	uint8_t *p = wire;
	unsigned int i;
	int ret;

	if (!wire)
		return -EINVAL;
	if (wire_bytes < SPF_HOP_REQUEST_BYTES)
		return -ENOSPC;
	ret = validate_request(request);
	if (ret)
		return ret;
	memset(p, 0, SPF_HOP_REQUEST_BYTES);
	put_le32(p, SPF_HOP_REQUEST_MAGIC);
	put_le16(p + 4, SPF_HOP_PROTOCOL_VERSION);
	put_le16(p + 6, SPF_HOP_REQUEST_BYTES);
	put_le32(p + 8, request->required_features);
	put_le32(p + 12, request->flags);
	put_le64(p + 16, request->session_id);
	put_le64(p + 24, request->sample_rate_hz);
	put_le64(p + 32, request->rf_bandwidth_hz);
	put_le64(p + 40, (uint64_t)request->if_offset_hz);
	put_le64(p + 48, request->dwell_samples);
	put_le64(p + 56, request->transition_guard_samples);
	put_le64(p + 64, request->dwell_count);
	put_le64(p + 88, request->capture_span_samples);
	put_le16(p + 72, SPF_HOP_PROFILE_COUNT);
	p[74] = request->initial_profile;
	put_le16(p + 76, SPF_HOP_EVENT_BYTES);
	put_le16(p + 78, SPF_HOP_STATUS_BYTES);
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		uint8_t *entry = p + 96 + i * 24;
		const struct spf_hop_profile_v1 *profile = &request->profiles[i];

		entry[0] = profile->profile_id;
		entry[1] = profile->fastlock_slot;
		put_le64(entry + 4, profile->center_frequency_hz);
		put_le64(entry + 12, profile->lo_frequency_hz);
		put_le32(entry + 20, profile->profile_crc32);
	}
	return 0;
}

static int validate_event(const struct spf_hop_event_v1 *event)
{
	const struct spf_hop_device_event_v1 *device;

	if (!event)
		return -EINVAL;
	device = &event->device;
	if (device->flags != SPF_HOP_EVENT_FLAGS_V1 ||
		(device->kind != SPF_HOP_EVENT_STARTUP &&
		 device->kind != SPF_HOP_EVENT_RETUNE) ||
		device->to_profile >= SPF_HOP_PROFILE_COUNT ||
		device->fastlock_slot >= SPF_HOP_PROFILE_COUNT ||
		!device->device_event_id ||
		device->transition_before > device->transition_after ||
		event->invalid_start != device->transition_before ||
		event->invalid_end < device->transition_after)
		return -EINVAL;
	return 0;
}

static int decode_event(struct spf_hop_event_v1 *event, const uint8_t *p)
{
	event->device.event_sequence = get_le64(p);
	event->device.dwell_index = get_le64(p + 8);
	event->device.transition_before = get_le64(p + 16);
	event->device.transition_after = get_le64(p + 24);
	event->invalid_start = get_le64(p + 32);
	event->invalid_end = get_le64(p + 40);
	event->device.from_profile = p[48];
	event->device.to_profile = p[49];
	event->device.kind = p[50];
	event->device.flags = p[51];
	event->device.fastlock_slot = get_le16(p + 52);
	if (get_le16(p + 54))
		return -EBADMSG;
	event->device.actual_lo_frequency_hz = get_le64(p + 56);
	event->device.actual_if_offset_hz = (int64_t)get_le64(p + 64);
	event->device.device_event_id = get_le64(p + 72);
	return validate_event(event);
}

static void encode_event(uint8_t *p, const struct spf_hop_event_v1 *event)
{
	const struct spf_hop_device_event_v1 *device = &event->device;

	put_le64(p, device->event_sequence);
	put_le64(p + 8, device->dwell_index);
	put_le64(p + 16, device->transition_before);
	put_le64(p + 24, device->transition_after);
	put_le64(p + 32, event->invalid_start);
	put_le64(p + 40, event->invalid_end);
	p[48] = device->from_profile;
	p[49] = device->to_profile;
	p[50] = device->kind;
	p[51] = device->flags;
	put_le16(p + 52, device->fastlock_slot);
	put_le64(p + 56, device->actual_lo_frequency_hz);
	put_le64(p + 64, (uint64_t)device->actual_if_offset_hz);
	put_le64(p + 72, device->device_event_id);
}

static int validate_state(uint16_t state, uint16_t reason, int32_t error,
	uint32_t flags)
{
	if (state > SPF_HOP_STATE_FAILED || reason > SPF_HOP_REASON_RESTORE_ERROR ||
		(flags & ~SPF_HOP_STATUS_FLAGS_V1) ||
		!(flags & SPF_HOP_STATUS_RESTORE_REQUIRED) ||
		((flags & SPF_HOP_STATUS_RESTORE_SUCCEEDED) &&
		 !(flags & SPF_HOP_STATUS_RESTORE_ATTEMPTED)))
		return -EINVAL;
	if ((state >= SPF_HOP_STATE_COMPLETED) !=
		!!(flags & SPF_HOP_STATUS_TERMINAL))
		return -EINVAL;
	if (state < SPF_HOP_STATE_COMPLETED &&
		(reason != SPF_HOP_REASON_NONE || error))
		return -EINVAL;
	if (state == SPF_HOP_STATE_COMPLETED &&
		(reason != SPF_HOP_REASON_PLAN_COMPLETE || error))
		return -EINVAL;
	if (state == SPF_HOP_STATE_CANCELLED &&
		(reason != SPF_HOP_REASON_CLIENT_CLOSE &&
		 reason != SPF_HOP_REASON_CLIENT_DISCONNECT))
		return -EINVAL;
	if (state == SPF_HOP_STATE_CANCELLED && error)
		return -EINVAL;
	if (state == SPF_HOP_STATE_FAILED &&
		(reason == SPF_HOP_REASON_NONE ||
		 reason == SPF_HOP_REASON_PLAN_COMPLETE ||
		 reason == SPF_HOP_REASON_CLIENT_CLOSE ||
		 reason == SPF_HOP_REASON_CLIENT_DISCONNECT || error >= 0))
		return -EINVAL;
	return 0;
}

int spf_hop_sidecar_v1_decode(struct spf_hop_sidecar_v1 *sidecar,
	const void *wire, size_t wire_bytes)
{
	const uint8_t *p = wire;
	uint32_t record_bytes;
	unsigned int i;
	int ret;

	if (!sidecar || !wire)
		return -EINVAL;
	if (wire_bytes < SPF_HOP_SIDECAR_HEADER_BYTES)
		return -EMSGSIZE;
	if (get_le32(p) != SPF_HOP_SIDECAR_MAGIC ||
		get_le16(p + 4) != SPF_HOP_PROTOCOL_VERSION ||
		get_le16(p + 6) != SPF_HOP_SIDECAR_HEADER_BYTES)
		return -EPROTONOSUPPORT;
	record_bytes = get_le32(p + 8);
	if (record_bytes != wire_bytes || get_le32(p + 12) !=
		SPF_HOP_REQUIRED_FEATURES_V1 ||
		get_le16(p + 20) > SPF_HOP_EVENT_CAPACITY ||
		get_le16(p + 22) != SPF_HOP_EVENT_CAPACITY ||
		record_bytes != (uint32_t)SPF_HOP_SIDECAR_HEADER_BYTES +
			(uint32_t)get_le16(p + 20) * SPF_HOP_EVENT_BYTES)
		return -EBADMSG;
	memset(sidecar, 0, sizeof(*sidecar));
	sidecar->flags = get_le32(p + 16);
	sidecar->event_count = get_le16(p + 20);
	sidecar->session_id = get_le64(p + 24);
	sidecar->buffer_sequence = get_le64(p + 32);
	sidecar->block_first_sample = get_le64(p + 40);
	sidecar->block_end_sample = get_le64(p + 48);
	sidecar->state = get_le16(p + 56);
	sidecar->terminal_reason = get_le16(p + 58);
	sidecar->error_code = (int32_t)get_le32(p + 60);
	ret = validate_state(sidecar->state, sidecar->terminal_reason,
		sidecar->error_code, sidecar->flags);
	if (ret || !sidecar->session_id ||
		sidecar->block_first_sample >= sidecar->block_end_sample)
		return -EBADMSG;
	for (i = 0; i < sidecar->event_count; i++) {
		ret = decode_event(&sidecar->events[i],
			p + SPF_HOP_SIDECAR_HEADER_BYTES + i * SPF_HOP_EVENT_BYTES);
		if (ret)
			return ret;
	}
	return 0;
}

int spf_hop_sidecar_v1_encode(void *wire, size_t wire_bytes,
	const struct spf_hop_sidecar_v1 *sidecar)
{
	uint8_t *p = wire;
	size_t record_bytes;
	unsigned int i;
	int ret;

	if (!wire || !sidecar || sidecar->event_count > SPF_HOP_EVENT_CAPACITY)
		return -EINVAL;
	record_bytes = SPF_HOP_SIDECAR_HEADER_BYTES +
		sidecar->event_count * SPF_HOP_EVENT_BYTES;
	if (wire_bytes < record_bytes)
		return -ENOSPC;
	ret = validate_state(sidecar->state, sidecar->terminal_reason,
		sidecar->error_code, sidecar->flags);
	if (ret || !sidecar->session_id ||
		sidecar->block_first_sample >= sidecar->block_end_sample)
		return ret ? ret : -EINVAL;
	for (i = 0; i < sidecar->event_count; i++) {
		ret = validate_event(&sidecar->events[i]);
		if (ret)
			return ret;
	}
	memset(p, 0, record_bytes);
	put_le32(p, SPF_HOP_SIDECAR_MAGIC);
	put_le16(p + 4, SPF_HOP_PROTOCOL_VERSION);
	put_le16(p + 6, SPF_HOP_SIDECAR_HEADER_BYTES);
	put_le32(p + 8, (uint32_t)record_bytes);
	put_le32(p + 12, SPF_HOP_REQUIRED_FEATURES_V1);
	put_le32(p + 16, sidecar->flags);
	put_le16(p + 20, sidecar->event_count);
	put_le16(p + 22, SPF_HOP_EVENT_CAPACITY);
	put_le64(p + 24, sidecar->session_id);
	put_le64(p + 32, sidecar->buffer_sequence);
	put_le64(p + 40, sidecar->block_first_sample);
	put_le64(p + 48, sidecar->block_end_sample);
	put_le16(p + 56, sidecar->state);
	put_le16(p + 58, sidecar->terminal_reason);
	put_le32(p + 60, (uint32_t)sidecar->error_code);
	for (i = 0; i < sidecar->event_count; i++)
		encode_event(p + SPF_HOP_SIDECAR_HEADER_BYTES +
			i * SPF_HOP_EVENT_BYTES, &sidecar->events[i]);
	return (int)record_bytes;
}

int spf_hop_status_v1_decode(struct spf_hop_status_v1 *status,
	const void *wire, size_t wire_bytes)
{
	const uint8_t *p = wire;
	int ret;

	if (!status || !wire)
		return -EINVAL;
	if (wire_bytes != SPF_HOP_STATUS_BYTES)
		return -EMSGSIZE;
	if (get_le32(p) != SPF_HOP_STATUS_MAGIC ||
		get_le16(p + 4) != SPF_HOP_PROTOCOL_VERSION ||
		get_le16(p + 6) != SPF_HOP_STATUS_BYTES)
		return -EPROTONOSUPPORT;
	if (get_le32(p + 8) != SPF_HOP_REQUIRED_FEATURES_V1 ||
		get_le16(p + 126) || get_le64(p + 152))
		return -EBADMSG;
	memset(status, 0, sizeof(*status));
	status->state = get_le16(p + 12);
	status->terminal_reason = get_le16(p + 14);
	status->error_code = (int32_t)get_le32(p + 16);
	status->flags = get_le32(p + 20);
	status->session_id = get_le64(p + 24);
	status->planned_dwells = get_le64(p + 32);
	status->visits_started = get_le64(p + 40);
	status->events_emitted = get_le64(p + 48);
	status->next_event_sequence = get_le64(p + 56);
	status->last_block_sequence = get_le64(p + 64);
	status->last_block_end = get_le64(p + 72);
	status->first_counter = get_le64(p + 80);
	status->final_counter = get_le64(p + 88);
	status->restore_before = get_le64(p + 96);
	status->restore_after = get_le64(p + 104);
	status->restored_lo_frequency_hz = get_le64(p + 112);
	status->restore_error = (int32_t)get_le32(p + 120);
	status->active_profile = p[124];
	status->restored_profile = p[125];
	status->startup_invalid_start = get_le64(p + 128);
	status->startup_invalid_end = get_le64(p + 136);
	status->device_dropped_events = get_le64(p + 144);
	ret = validate_state(status->state, status->terminal_reason,
		status->error_code, status->flags);
	if (!ret && (!status->session_id || !status->planned_dwells ||
		(status->active_profile != SPF_HOP_PROFILE_NONE &&
		 status->active_profile >= SPF_HOP_PROFILE_COUNT) ||
		(status->restored_profile != SPF_HOP_PROFILE_NONE &&
		 status->restored_profile >= SPF_HOP_PROFILE_COUNT)))
		ret = -EINVAL;
	return ret ? -EBADMSG : 0;
}

int spf_hop_status_v1_encode(void *wire, size_t wire_bytes,
	const struct spf_hop_status_v1 *status)
{
	uint8_t *p = wire;
	int ret;

	if (!wire || !status)
		return -EINVAL;
	if (wire_bytes < SPF_HOP_STATUS_BYTES)
		return -ENOSPC;
	ret = validate_state(status->state, status->terminal_reason,
		status->error_code, status->flags);
	if (!ret && (!status->session_id || !status->planned_dwells ||
		(status->active_profile != SPF_HOP_PROFILE_NONE &&
		 status->active_profile >= SPF_HOP_PROFILE_COUNT) ||
		(status->restored_profile != SPF_HOP_PROFILE_NONE &&
		 status->restored_profile >= SPF_HOP_PROFILE_COUNT)))
		ret = -EINVAL;
	if (ret)
		return ret;
	memset(p, 0, SPF_HOP_STATUS_BYTES);
	put_le32(p, SPF_HOP_STATUS_MAGIC);
	put_le16(p + 4, SPF_HOP_PROTOCOL_VERSION);
	put_le16(p + 6, SPF_HOP_STATUS_BYTES);
	put_le32(p + 8, SPF_HOP_REQUIRED_FEATURES_V1);
	put_le16(p + 12, status->state);
	put_le16(p + 14, status->terminal_reason);
	put_le32(p + 16, (uint32_t)status->error_code);
	put_le32(p + 20, status->flags);
	put_le64(p + 24, status->session_id);
	put_le64(p + 32, status->planned_dwells);
	put_le64(p + 40, status->visits_started);
	put_le64(p + 48, status->events_emitted);
	put_le64(p + 56, status->next_event_sequence);
	put_le64(p + 64, status->last_block_sequence);
	put_le64(p + 72, status->last_block_end);
	put_le64(p + 80, status->first_counter);
	put_le64(p + 88, status->final_counter);
	put_le64(p + 96, status->restore_before);
	put_le64(p + 104, status->restore_after);
	put_le64(p + 112, status->restored_lo_frequency_hz);
	put_le32(p + 120, (uint32_t)status->restore_error);
	p[124] = status->active_profile;
	p[125] = status->restored_profile;
	put_le64(p + 128, status->startup_invalid_start);
	put_le64(p + 136, status->startup_invalid_end);
	put_le64(p + 144, status->device_dropped_events);
	return 0;
}
