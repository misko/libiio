/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-tandem-metadata.h"

#include <errno.h>
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

uint16_t spf_tandem_compact_v7_observations(
	spf_gain_observation_v3_t *observations, uint16_t count,
	uint64_t frame_start, uint32_t samples_per_channel)
{
	const uint16_t required_flags = SPF_GAIN_OBSERVATION_VALID |
		SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID;
	uint64_t frame_end;
	uint64_t previous_before = 0;
	uint64_t previous_after = 0;
	uint16_t read_index;
	uint16_t write_index = 0;

	if (!observations || !samples_per_channel ||
		frame_start > UINT64_MAX - samples_per_channel)
		return 0;
	frame_end = frame_start + samples_per_channel;
	for (read_index = 0; read_index < count; ++read_index) {
		const spf_gain_observation_v3_t *observation =
			&observations[read_index];

		if (observation->flags != required_flags ||
			observation->reserved0 || observation->reserved1 ||
			observation->rx1_gain_index != observation->rx2_gain_index ||
			observation->rx1_gain_index > UINT8_C(0x7f) ||
			observation->rx1_gain_db != observation->rx2_gain_db ||
			observation->rx1_gain_db == SPF_GAIN_DB_INVALID ||
			observation->sample_sequence_after <
				observation->sample_sequence_before ||
			observation->sample_sequence_after < frame_start ||
			observation->sample_sequence_before >= frame_end ||
			(write_index &&
			 (observation->sample_sequence_before < previous_before ||
			  observation->sample_sequence_after < previous_after)))
			continue;
		previous_before = observation->sample_sequence_before;
		previous_after = observation->sample_sequence_after;
		if (write_index != read_index)
			observations[write_index] = *observation;
		write_index++;
	}
	return write_index;
}

int spf_tandem_transition_count_wire(uint64_t transition_count,
	uint32_t *wire_count)
{
	if (!wire_count)
		return -EINVAL;
	if (transition_count > UINT32_MAX)
		return -ERANGE;
	*wire_count = (uint32_t)transition_count;
	return 0;
}

int spf_tandem_radio_frame_v7_build(void *destination,
	size_t destination_bytes, const spf_radio_frame_v7_args_t *base_args,
	const struct spf_tandem_frame_preview *preview,
	int32_t ad9361_temperature_mdeg_c)
{
	spf_radio_frame_v7_args_t args;
	uint32_t transition_count_start;
	uint32_t transition_count_end;
	int ret;

	if (!destination || !base_args || !preview)
		return -EINVAL;
	if (!preview->event_sequence_start_valid ||
		preview->event_count > UINT16_MAX)
		return -EPROTO;
	ret = spf_tandem_transition_count_wire(
		preview->timeline.transition_count_start,
		&transition_count_start);
	if (!ret)
		ret = spf_tandem_transition_count_wire(
			preview->timeline.transition_count_end,
			&transition_count_end);
	if (ret)
		return ret;

	args = *base_args;
	args.gain_event_count = (uint16_t)preview->event_count;
	args.rx1_first_change_sample =
		preview->timeline.rx1_first_change_sample;
	args.rx2_first_change_sample =
		preview->timeline.rx2_first_change_sample;
	args.ownership_epoch = preview->status.ownership_epoch;
	args.tandem_state = preview->status.state;
	args.tandem_fault_flags = preview->status.fault_flags;
	args.tandem_transition_count_end = transition_count_end;
	args.gain_table_id = preview->status.gain_table_id;
	args.threshold_provenance = preview->status.threshold_provenance;
	args.minimum_gain_db = preview->status.minimum_gain_db;
	args.maximum_gain_db = preview->status.maximum_gain_db;
	args.initial_gain_db = preview->status.initial_gain_db;
	args.minimum_gain_index = preview->status.minimum_gain_index;
	args.maximum_gain_index = preview->status.maximum_gain_index;
	args.rx1_gain_index_start =
		preview->timeline.gain_start.rx1_gain_index;
	args.rx2_gain_index_start =
		preview->timeline.gain_start.rx2_gain_index;
	args.rx1_gain_index_end = preview->timeline.gain_end.rx1_gain_index;
	args.rx2_gain_index_end = preview->timeline.gain_end.rx2_gain_index;
	args.ad9361_temperature_mdeg_c = ad9361_temperature_mdeg_c;
	args.tandem_transition_count_start = transition_count_start;
	args.timeline_flags = SPF_FPGA_GAIN_TIMELINE_COMPLETE;
	args.event_sequence_start = preview->event_sequence_start;
	if (!spf_radio_frame_v7_base_build(destination, destination_bytes, &args))
		return -EPROTO;
	return 0;
}

size_t spf_radio_frame_v5_header_bytes(uint16_t observation_capacity,
	uint16_t event_capacity)
{
	const size_t v3_bytes = spf_radio_frame_v3_header_bytes(
		observation_capacity, event_capacity);

	if (!v3_bytes || v3_bytes > UINT16_MAX - SPF_RADIO_META_V5_EXTENSION_BYTES)
		return 0;
	return v3_bytes + SPF_RADIO_META_V5_EXTENSION_BYTES;
}

bool spf_radio_frame_v5_build(void *destination, size_t destination_bytes,
	const spf_radio_frame_v5_args_t *args)
{
	spf_radio_meta_v3_prefix_t *prefix = destination;
	spf_radio_meta_v5_extension_t *extension;
	const struct adi_tandem_agc_status *status;
	size_t v3_bytes;
	size_t v5_bytes;
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
	v5_bytes = spf_radio_frame_v5_header_bytes(
		args->frame.gain_observation_capacity,
		args->frame.gain_event_capacity);
	if (!v3_bytes || !v5_bytes || destination_bytes < v5_bytes ||
		(args->frame.metadata_features & SPF_META_REQUIRED_FEATURES_V5) !=
			SPF_META_REQUIRED_FEATURES_V5)
		return false;
	if (!spf_radio_frame_v3_build(destination, destination_bytes, &args->frame))
		return false;

	variable_bytes = v3_bytes - SPF_RADIO_META_V3_PREFIX_BYTES;
	memmove((uint8_t *)destination + SPF_RADIO_META_V5_PREFIX_BYTES,
		(uint8_t *)destination + SPF_RADIO_META_V3_PREFIX_BYTES,
		variable_bytes);
	extension = (spf_radio_meta_v5_extension_t *)
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
	extension->ad9361_temperature_mdeg_c = args->ad9361_temperature_mdeg_c;

	prefix->version = SPF_GAIN_META_VERSION_V5;
	prefix->header_bytes = (uint16_t)v5_bytes;
	prefix->features |= SPF_META_FEATURE_TANDEM_AGC_SESSION |
		SPF_META_FEATURE_AD9361_TEMPERATURE;
	prefix->flags |= SPF_META_TANDEM_VALID;
	crc = (uint32_t *)((uint8_t *)destination + v5_bytes - sizeof(*crc));
	*crc = 0;
	*crc = spf_gain_meta_crc32(destination, v5_bytes);
	return true;
}

bool spf_radio_frame_v6_build(void *destination, size_t destination_bytes,
	const spf_radio_frame_v6_args_t *args)
{
	spf_radio_meta_v3_prefix_t *prefix = destination;
	spf_radio_meta_v5_extension_t *extension;
	const struct adi_tandem_agc_status *status;
	size_t base_bytes;
	size_t v6_bytes;
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
	base_bytes = spf_radio_frame_v3_header_bytes(
		args->frame.gain_observation_capacity,
		args->frame.gain_event_capacity);
	v6_bytes = spf_radio_frame_v5_header_bytes(
		args->frame.gain_observation_capacity,
		args->frame.gain_event_capacity);
	if (!base_bytes || !v6_bytes || destination_bytes < v6_bytes ||
		(args->frame.metadata_features & SPF_META_REQUIRED_FEATURES_V6) !=
			SPF_META_REQUIRED_FEATURES_V6)
		return false;
	if (!spf_radio_frame_v6_base_build(destination, destination_bytes,
			&args->frame, args->missing_samples_before))
		return false;

	variable_bytes = base_bytes - SPF_RADIO_META_V3_PREFIX_BYTES;
	memmove((uint8_t *)destination + SPF_RADIO_META_V5_PREFIX_BYTES,
		(uint8_t *)destination + SPF_RADIO_META_V3_PREFIX_BYTES,
		variable_bytes);
	extension = (spf_radio_meta_v5_extension_t *)
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
	extension->ad9361_temperature_mdeg_c =
		args->ad9361_temperature_mdeg_c;

	prefix->header_bytes = (uint16_t)v6_bytes;
	prefix->features |= SPF_META_FEATURE_TANDEM_AGC_SESSION |
		SPF_META_FEATURE_AD9361_TEMPERATURE;
	prefix->flags |= SPF_META_TANDEM_VALID;
	crc = (uint32_t *)((uint8_t *)destination + v6_bytes - sizeof(*crc));
	*crc = 0;
	*crc = spf_gain_meta_crc32(destination, v6_bytes);
	return true;
}
