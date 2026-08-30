/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_TANDEM_METADATA_H__
#define __SPF_TANDEM_METADATA_H__

#include "spf-tandem-session.h"

#include <spf_radio_frame_v3.h>

#define SPF_GAIN_META_VERSION_V5 UINT16_C(5)
#define SPF_META_FEATURE_TANDEM_AGC_SESSION (UINT32_C(1) << 8)
#define SPF_META_FEATURE_AD9361_TEMPERATURE (UINT32_C(1) << 9)
#define SPF_META_TANDEM_VALID (UINT32_C(1) << 22)
#define SPF_RADIO_META_V5_EXTENSION_BYTES UINT16_C(56)
#define SPF_RADIO_META_V5_PREFIX_BYTES \
	(SPF_RADIO_META_V3_PREFIX_BYTES + SPF_RADIO_META_V5_EXTENSION_BYTES)
#define SPF_META_REQUIRED_FEATURES_V5 \
	(SPF_META_REQUIRED_FEATURES_V3 | SPF_META_FEATURE_FPGA_GAIN_EVENTS | \
	 SPF_META_FEATURE_TANDEM_AGC_SESSION | \
	 SPF_META_FEATURE_AD9361_TEMPERATURE)
#define SPF_META_REQUIRED_FEATURES_V6 \
	(SPF_META_REQUIRED_FEATURES_V5 | \
	 SPF_META_FEATURE_CANONICAL_RX_LAYOUT | \
	 SPF_META_FEATURE_EXACT_GAP_ACCOUNTING)

#pragma pack(push, 1)
typedef struct {
	uint32_t ownership_epoch;
	uint32_t tandem_state;
	uint32_t tandem_fault_flags;
	uint32_t tandem_transition_count;
	uint32_t gain_table_id;
	uint32_t threshold_provenance;
	int32_t minimum_gain_db;
	int32_t maximum_gain_db;
	int32_t initial_gain_db;
	uint8_t minimum_gain_index;
	uint8_t maximum_gain_index;
	uint8_t rx1_gain_index;
	uint8_t rx2_gain_index;
	int32_t ad9361_temperature_mdeg_c;
	uint32_t reserved[3];
} spf_radio_meta_v5_extension_t;
#pragma pack(pop)

_Static_assert(sizeof(spf_radio_meta_v5_extension_t) ==
	SPF_RADIO_META_V5_EXTENSION_BYTES,
	"SPF radio metadata v5 extension must be 56 bytes");

typedef struct {
	spf_radio_frame_v3_args_t frame;
	const struct adi_tandem_agc_status *tandem_status;
	int32_t ad9361_temperature_mdeg_c;
} spf_radio_frame_v5_args_t;

typedef struct {
	spf_radio_frame_v3_args_t frame;
	const struct adi_tandem_agc_status *tandem_status;
	int32_t ad9361_temperature_mdeg_c;
	uint64_t missing_samples_before;
} spf_radio_frame_v6_args_t;

size_t spf_radio_frame_v5_header_bytes(uint16_t observation_capacity,
	uint16_t event_capacity);
uint16_t spf_tandem_compact_coherent_observations(
	spf_gain_observation_v3_t *observations, uint16_t count);
uint16_t spf_tandem_compact_v7_observations(
	spf_gain_observation_v3_t *observations, uint16_t count,
	uint64_t frame_start, uint32_t samples_per_channel);
int spf_tandem_transition_count_wire(uint64_t transition_count,
	uint32_t *wire_count);
int spf_tandem_radio_frame_v7_build(void *destination,
	size_t destination_bytes, const spf_radio_frame_v7_args_t *base_args,
	const struct spf_tandem_frame_preview *preview,
	int32_t ad9361_temperature_mdeg_c);
bool spf_radio_frame_v5_build(void *destination, size_t destination_bytes,
	const spf_radio_frame_v5_args_t *args);
bool spf_radio_frame_v6_build(void *destination, size_t destination_bytes,
	const spf_radio_frame_v6_args_t *args);

#endif
