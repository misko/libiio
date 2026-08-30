/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-tandem-metadata.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_golden_layout_and_crc(void)
{
	uint8_t output[512];
	spf_gain_observation_v3_t observation = {
		.sample_sequence_before = 95,
		.sample_sequence_after = 99,
		.read_duration_ns = 1200,
		.flags = SPF_GAIN_OBSERVATION_VALID |
			SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID,
		.rx1_gain_index = 20,
		.rx2_gain_index = 20,
		.rx1_gain_db = 10,
		.rx2_gain_db = 10,
	};
	struct adi_tandem_agc_event event = {
		.sample_sequence = UINT64_C(123),
		.event_sequence = UINT32_C(0x11223344),
		.flags = UINT16_C(0x13),
		.rx1_gain_index = 21,
		.rx2_gain_index = 21,
	};
	struct adi_tandem_agc_status status = {
		.version = ADI_TANDEM_AGC_ABI_VERSION,
		.size = sizeof(status),
		.state = ADI_TANDEM_AGC_STATE_ARMED_AUTO,
		.ownership_epoch = UINT32_C(0x10203040),
		.transition_count = 9,
		.minimum_gain_db = -10,
		.maximum_gain_db = 71,
		.initial_gain_db = 10,
		.minimum_gain_index = 0,
		.maximum_gain_index = 76,
		.rx1_gain_index = 21,
		.rx2_gain_index = 21,
		.gain_table_id = ADI_TANDEM_AGC_GAIN_TABLE_1300_4000_MHZ,
		.threshold_provenance = UINT32_C(0x30313a14),
	};
	const spf_radio_frame_v5_args_t args = {
		.frame = {
			.metadata_features = SPF_META_REQUIRED_FEATURES_V5,
			.stream_id = 1,
			.buffer_sequence = 2,
			.first_sample_sequence = 100,
			.samples_per_channel = 100,
			.iq_payload_bytes = 800,
			.enabled_scan_mask = 0x0f,
			.gain_observation_interval_samples = 25,
			.gain_observations = &observation,
			.gain_observation_count = 1,
			.gain_observation_capacity = 1,
			.gain_events = (const spf_gain_event_v3_t *)&event,
			.gain_event_count = 1,
			.gain_event_capacity = 1,
			.rssi_start = {.rx1_qdb = 10, .rx2_qdb = 11, .valid = true},
			.rssi_end = {.rx1_qdb = 12, .rx2_qdb = 13, .valid = true},
		},
		.tandem_status = &status,
		.ad9361_temperature_mdeg_c = 43860,
	};
	spf_radio_meta_v3_prefix_t *prefix = (void *)output;
	spf_radio_meta_v5_extension_t *extension =
		(void *)(output + SPF_RADIO_META_V3_PREFIX_BYTES);
	const size_t expected_bytes = SPF_RADIO_META_V5_PREFIX_BYTES +
		SPF_GAIN_OBSERVATION_BYTES + SPF_GAIN_EVENT_BYTES + sizeof(uint32_t);
	uint32_t received_crc;

	memset(output, 0xa5, sizeof(output));
	assert(spf_radio_frame_v5_header_bytes(1, 1) == expected_bytes);
	assert(spf_radio_frame_v5_build(output, sizeof(output), &args));
	assert(prefix->version == SPF_GAIN_META_VERSION_V5);
	assert(prefix->header_bytes == expected_bytes);
	assert(prefix->features == SPF_META_REQUIRED_FEATURES_V5);
	assert(prefix->flags & SPF_META_TANDEM_VALID);
	assert(extension->ownership_epoch == UINT32_C(0x10203040));
	assert(extension->gain_table_id ==
		ADI_TANDEM_AGC_GAIN_TABLE_1300_4000_MHZ);
	assert(extension->threshold_provenance == UINT32_C(0x30313a14));
	assert(extension->ad9361_temperature_mdeg_c == 43860);
	assert(extension->reserved[0] == 0 && extension->reserved[1] == 0 &&
		extension->reserved[2] == 0);
	assert(memcmp(output + SPF_RADIO_META_V5_PREFIX_BYTES +
		SPF_GAIN_OBSERVATION_BYTES, &event, sizeof(event)) == 0);
	memcpy(&received_crc, output + expected_bytes - sizeof(received_crc),
		sizeof(received_crc));
	memset(output + expected_bytes - sizeof(received_crc), 0,
		sizeof(received_crc));
	assert(received_crc == spf_gain_meta_crc32(output, expected_bytes));
}

static void test_invalid_temperature_is_serialized_without_header_growth(void)
{
	uint8_t output[512];
	spf_gain_observation_v3_t observation = {
		.sample_sequence_before = 1,
		.sample_sequence_after = 2,
		.flags = SPF_GAIN_OBSERVATION_VALID |
			SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID,
		.rx1_gain_index = 30,
		.rx2_gain_index = 30,
		.rx1_gain_db = 30,
		.rx2_gain_db = 30,
	};
	struct adi_tandem_agc_status status = {
		.version = ADI_TANDEM_AGC_ABI_VERSION,
		.size = sizeof(status),
		.state = ADI_TANDEM_AGC_STATE_ARMED_HOLD,
		.ownership_epoch = 1,
		.minimum_gain_db = 0,
		.maximum_gain_db = 62,
		.initial_gain_db = 30,
		.rx1_gain_index = 30,
		.rx2_gain_index = 30,
		.gain_table_id = ADI_TANDEM_AGC_GAIN_TABLE_1300_4000_MHZ,
	};
	const spf_radio_frame_v5_args_t args = {
		.frame = {
			.metadata_features = SPF_META_REQUIRED_FEATURES_V5,
			.stream_id = 1,
			.samples_per_channel = 1,
			.iq_payload_bytes = 8,
			.enabled_scan_mask = 0x0f,
			.gain_observation_interval_samples = 1,
			.gain_observations = &observation,
			.gain_observation_count = 1,
			.gain_observation_capacity = 1,
			.gain_event_capacity = 0,
		},
		.tandem_status = &status,
		.ad9361_temperature_mdeg_c = INT32_MIN,
	};
	spf_radio_meta_v5_extension_t *extension =
		(void *)(output + SPF_RADIO_META_V3_PREFIX_BYTES);

	/* The v5 extension consumes one old reserved word and remains 56 bytes. */
	assert(SPF_RADIO_META_V5_EXTENSION_BYTES == 56);
	assert(spf_radio_frame_v5_build(output, sizeof(output), &args));
	assert(extension->ad9361_temperature_mdeg_c == INT32_MIN);
}

static void test_auto_observations_drop_torn_pair(void)
{
	spf_gain_observation_v3_t observations[] = {
		{.sample_sequence_before = 100, .sample_sequence_after = 110,
		 .rx1_gain_index = 40, .rx2_gain_index = 40},
		/* A paired FPGA step can land between the two sequential SPI reads. */
		{.sample_sequence_before = 200, .sample_sequence_after = 210,
		 .rx1_gain_index = 40, .rx2_gain_index = 41},
		{.sample_sequence_before = 300, .sample_sequence_after = 310,
		 .rx1_gain_index = 41, .rx2_gain_index = 41},
	};

	assert(spf_tandem_compact_coherent_observations(observations, 3) == 2);
	assert(observations[0].sample_sequence_before == 100);
	assert(observations[1].sample_sequence_before == 300);
}

static void test_v7_observations_keep_only_canonical_overlap(void)
{
	const uint16_t valid = SPF_GAIN_OBSERVATION_VALID |
		SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID;
	spf_gain_observation_v3_t observations[] = {
		{.sample_sequence_before = 90, .sample_sequence_after = 100,
		 .flags = valid, .rx1_gain_index = 20, .rx2_gain_index = 20,
		 .rx1_gain_db = 10, .rx2_gain_db = 10},
		{.sample_sequence_before = 110, .sample_sequence_after = 120,
		 .flags = SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID,
		 .rx1_gain_index = 20, .rx2_gain_index = 20,
		 .rx1_gain_db = 10, .rx2_gain_db = 10},
		{.sample_sequence_before = 120, .sample_sequence_after = 130,
		 .flags = valid, .rx1_gain_index = 20, .rx2_gain_index = 21,
		 .rx1_gain_db = 10, .rx2_gain_db = 11},
		{.sample_sequence_before = 130, .sample_sequence_after = 140,
		 .flags = valid, .rx1_gain_index = 21, .rx2_gain_index = 21,
		 .rx1_gain_db = 11, .rx2_gain_db = 11, .reserved1 = 1},
		{.sample_sequence_before = 140, .sample_sequence_after = 150,
		 .flags = valid, .rx1_gain_index = 21, .rx2_gain_index = 21,
		 .rx1_gain_db = 11, .rx2_gain_db = 11},
		{.sample_sequence_before = 200, .sample_sequence_after = 210,
		 .flags = valid, .rx1_gain_index = 21, .rx2_gain_index = 21,
		 .rx1_gain_db = 11, .rx2_gain_db = 11},
	};

	assert(spf_tandem_compact_v7_observations(observations, 6,
		100, 100) == 2);
	assert(observations[0].sample_sequence_before == 90);
	assert(observations[1].sample_sequence_before == 140);
}

static void test_v6_single_rx_and_exact_gap(void)
{
	uint8_t output[512];
	spf_gain_observation_v3_t observation = {
		.sample_sequence_before = UINT64_C(0x100000001),
		.sample_sequence_after = UINT64_C(0x100000011),
		.flags = SPF_GAIN_OBSERVATION_VALID |
			SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID,
		.rx1_gain_index = 20,
		.rx2_gain_index = 20,
		.rx1_gain_db = 10,
		.rx2_gain_db = 10,
	};
	struct adi_tandem_agc_status status = {
		.version = ADI_TANDEM_AGC_ABI_VERSION,
		.size = sizeof(status),
		.state = ADI_TANDEM_AGC_STATE_ARMED_AUTO,
		.ownership_epoch = 7,
		.minimum_gain_db = -10,
		.maximum_gain_db = 71,
		.initial_gain_db = 10,
		.minimum_gain_index = 0,
		.maximum_gain_index = 76,
		.rx1_gain_index = 20,
		.rx2_gain_index = 20,
		.gain_table_id = ADI_TANDEM_AGC_GAIN_TABLE_1300_4000_MHZ,
	};
	spf_radio_frame_v6_args_t args = {
		.frame = {
			.metadata_features = SPF_META_REQUIRED_FEATURES_V6,
			.stream_id = 1,
			.buffer_sequence = 3,
			.first_sample_sequence = UINT64_C(0x100000020),
			.samples_per_channel = 1024,
			.iq_payload_bytes = 4096,
			.enabled_scan_mask = UINT32_C(0x03),
			.gain_observation_interval_samples = 256,
			.gain_observations = &observation,
			.gain_observation_count = 1,
			.gain_observation_capacity = 1,
			.gain_event_capacity = 0,
		},
		.tandem_status = &status,
		.ad9361_temperature_mdeg_c = 42000,
		.missing_samples_before = UINT64_C(0x100000002),
	};
	spf_radio_meta_v3_prefix_t *prefix = (void *)output;
	uint32_t stored_crc;
	const size_t bytes = spf_radio_frame_v5_header_bytes(1, 0);

	assert(spf_radio_frame_v6_build(output, sizeof(output), &args));
	assert(prefix->version == SPF_GAIN_META_VERSION_V6);
	assert(prefix->header_bytes == bytes);
	assert(prefix->channel_count == 1);
	assert(prefix->enabled_scan_mask == UINT32_C(0x03));
	assert(prefix->iq_payload_bytes == 4096);
	assert(prefix->flags & SPF_META_SAMPLE_GAP_BEFORE);
	assert(spf_radio_meta_v6_missing_samples_before(prefix) ==
		UINT64_C(0x100000002));
	memcpy(&stored_crc, output + bytes - sizeof(stored_crc),
		sizeof(stored_crc));
	memset(output + bytes - sizeof(stored_crc), 0, sizeof(stored_crc));
	assert(stored_crc == spf_gain_meta_crc32(output, bytes));

	args.frame.enabled_scan_mask = UINT32_C(0x0c);
	args.missing_samples_before = 0;
	assert(spf_radio_frame_v6_build(output, sizeof(output), &args));
	args.frame.enabled_scan_mask = UINT32_C(0x0f);
	args.frame.iq_payload_bytes = 8192;
	assert(spf_radio_frame_v6_build(output, sizeof(output), &args));
	args.frame.enabled_scan_mask = UINT32_C(0x05);
	assert(!spf_radio_frame_v6_build(output, sizeof(output), &args));
}

int main(void)
{
	test_golden_layout_and_crc();
	test_invalid_temperature_is_serialized_without_header_growth();
	test_auto_observations_drop_torn_pair();
	test_v7_observations_keep_only_canonical_overlap();
	test_v6_single_rx_and_exact_gap();
	puts("SPF tandem metadata tests passed");
	return 0;
}
