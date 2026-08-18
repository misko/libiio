/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-tandem-metadata.h"

#include <limits.h>
#include <string.h>

uint16_t spf_tandem_compact_coherent_observations(
	spf_gain_observation_v3_t *observations, uint16_t count)
{
	uint16_t read_index;
	uint16_t write_index = 0;

	if (!observations)
		return 0;
	for (read_index = 0; read_index < count; ++read_index) {
		/*
		 * The AD9361 exposes RX1 and RX2 gain through separate SPI reads.  In
		 * AUTO, an atomic paired FPGA step can land between those reads and
		 * produce a torn 40/41 observation even though the channels never
		 * diverged.  Exact sample-aligned tandem events retain that transition;
		 * do not publish the non-atomic diagnostic observation as radio state.
		 */
		if (observations[read_index].rx1_gain_index !=
			observations[read_index].rx2_gain_index)
			continue;
		if (write_index != read_index)
			observations[write_index] = observations[read_index];
		write_index++;
	}
	return write_index;
}

size_t spf_radio_frame_v4_header_bytes(uint16_t observation_capacity,
	uint16_t event_capacity)
{
	const size_t v3_bytes = spf_radio_frame_v3_header_bytes(
		observation_capacity, event_capacity);

	if (!v3_bytes || v3_bytes > UINT16_MAX - SPF_RADIO_META_V4_EXTENSION_BYTES)
		return 0;
	return v3_bytes + SPF_RADIO_META_V4_EXTENSION_BYTES;
}

bool spf_radio_frame_v4_build(void *destination, size_t destination_bytes,
	const spf_radio_frame_v4_args_t *args)
{
	spf_radio_meta_v3_prefix_t *prefix = destination;
	spf_radio_meta_v4_extension_t *extension;
	const struct adi_tandem_agc_status *status;
	size_t v3_bytes;
	size_t v4_bytes;
	size_t variable_bytes;
	uint32_t *crc;

	if (!destination || !args || !args->tandem_status)
		return false;
	status = args->tandem_status;
	if (status->version != ADI_TANDEM_AGC_ABI_VERSION ||
		status->size != sizeof(*status) || !status->ownership_epoch ||
		status->fault_flags ||
		(status->state != ADI_TANDEM_AGC_STATE_ARMED_HOLD &&
		 status->state != ADI_TANDEM_AGC_STATE_ARMED_AUTO) ||
		!status->gain_table_id)
		return false;
	v3_bytes = spf_radio_frame_v3_header_bytes(
		args->frame.gain_observation_capacity,
		args->frame.gain_event_capacity);
	v4_bytes = spf_radio_frame_v4_header_bytes(
		args->frame.gain_observation_capacity,
		args->frame.gain_event_capacity);
	if (!v3_bytes || !v4_bytes || destination_bytes < v4_bytes ||
		(args->frame.metadata_features & SPF_META_REQUIRED_FEATURES_V4) !=
			SPF_META_REQUIRED_FEATURES_V4)
		return false;
	if (!spf_radio_frame_v3_build(destination, destination_bytes, &args->frame))
		return false;

	variable_bytes = v3_bytes - SPF_RADIO_META_V3_PREFIX_BYTES;
	memmove((uint8_t *)destination + SPF_RADIO_META_V4_PREFIX_BYTES,
		(uint8_t *)destination + SPF_RADIO_META_V3_PREFIX_BYTES,
		variable_bytes);
	extension = (spf_radio_meta_v4_extension_t *)
		((uint8_t *)destination + SPF_RADIO_META_V3_PREFIX_BYTES);
	memset(extension, 0, sizeof(*extension));
	extension->ownership_epoch = status->ownership_epoch;
	extension->tandem_state = status->state;
	extension->tandem_fault_flags = status->fault_flags;
	extension->tandem_transition_count = status->transition_count;
	extension->gain_table_id = status->gain_table_id;
	extension->threshold_provenance = status->threshold_provenance;
	extension->minimum_gain_db = status->minimum_gain_db;
	extension->maximum_gain_db = status->maximum_gain_db;
	extension->initial_gain_db = status->initial_gain_db;
	extension->minimum_gain_index = status->minimum_gain_index;
	extension->maximum_gain_index = status->maximum_gain_index;
	extension->rx1_gain_index = status->rx1_gain_index;
	extension->rx2_gain_index = status->rx2_gain_index;

	prefix->version = SPF_GAIN_META_VERSION_V4;
	prefix->header_bytes = (uint16_t)v4_bytes;
	prefix->features |= SPF_META_FEATURE_TANDEM_AGC_SESSION;
	prefix->flags |= SPF_META_TANDEM_VALID;
	crc = (uint32_t *)((uint8_t *)destination + v4_bytes - sizeof(*crc));
	*crc = 0;
	*crc = spf_gain_meta_crc32(destination, v4_bytes);
	return true;
}
