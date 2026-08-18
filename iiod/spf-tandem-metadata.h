/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_TANDEM_METADATA_H__
#define __SPF_TANDEM_METADATA_H__

#include "spf-tandem-session.h"

#include <spf_radio_frame_v3.h>

#define SPF_GAIN_META_VERSION_V4 UINT16_C(4)
#define SPF_META_FEATURE_TANDEM_AGC_SESSION (UINT32_C(1) << 8)
#define SPF_META_TANDEM_VALID (UINT32_C(1) << 22)
#define SPF_RADIO_META_V4_EXTENSION_BYTES UINT16_C(56)
#define SPF_RADIO_META_V4_PREFIX_BYTES \
	(SPF_RADIO_META_V3_PREFIX_BYTES + SPF_RADIO_META_V4_EXTENSION_BYTES)
#define SPF_META_REQUIRED_FEATURES_V4 \
	(SPF_META_REQUIRED_FEATURES_V3 | SPF_META_FEATURE_FPGA_GAIN_EVENTS | \
	 SPF_META_FEATURE_TANDEM_AGC_SESSION)

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
	uint32_t reserved[4];
} spf_radio_meta_v4_extension_t;
#pragma pack(pop)

_Static_assert(sizeof(spf_radio_meta_v4_extension_t) ==
	SPF_RADIO_META_V4_EXTENSION_BYTES,
	"SPF radio metadata v4 extension must be 56 bytes");

typedef struct {
	spf_radio_frame_v3_args_t frame;
	const struct adi_tandem_agc_status *tandem_status;
} spf_radio_frame_v4_args_t;

size_t spf_radio_frame_v4_header_bytes(uint16_t observation_capacity,
	uint16_t event_capacity);
uint16_t spf_tandem_compact_coherent_observations(
	spf_gain_observation_v3_t *observations, uint16_t count);
bool spf_radio_frame_v4_build(void *destination, size_t destination_bytes,
	const spf_radio_frame_v4_args_t *args);

#endif
