/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_SPF_METADATA_REQUEST_H__
#define __IIOD_SPF_METADATA_REQUEST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPF_METADATA_REQUEST_MAGIC UINT32_C(0x31524d53) /* SMR1 */
#define SPF_METADATA_REQUEST_VERSION UINT16_C(1)
#define SPF_METADATA_REQUEST_HEADER_BYTES UINT16_C(32)
#define SPF_METADATA_REQUEST_TANDEM_BYTES UINT16_C(104)
#define SPF_METADATA_RECORD_VERSION UINT16_C(7)

#define SPF_METADATA_FEATURE_FPGA_GAIN_TIMELINE (UINT32_C(1) << 0)
#define SPF_METADATA_FEATURE_EXACT_EVENT_SEQUENCE (UINT32_C(1) << 1)
#define SPF_METADATA_FEATURE_OPTIONAL_RSSI_TELEMETRY (UINT32_C(1) << 2)
#define SPF_METADATA_FEATURE_TYPED_CAPTURE_ERRORS (UINT32_C(1) << 3)
#define SPF_METADATA_REQUIRED_FEATURES \
	(SPF_METADATA_FEATURE_FPGA_GAIN_TIMELINE | \
	 SPF_METADATA_FEATURE_EXACT_EVENT_SEQUENCE | \
	 SPF_METADATA_FEATURE_OPTIONAL_RSSI_TELEMETRY | \
	 SPF_METADATA_FEATURE_TYPED_CAPTURE_ERRORS)

enum spf_metadata_transport_kind {
	SPF_METADATA_TRANSPORT_ORDINARY = 0,
	SPF_METADATA_TRANSPORT_BURST = 1,
	SPF_METADATA_TRANSPORT_RING = 2,
};

struct spf_metadata_request {
	uint32_t required_features;
	uint16_t record_version;
	uint16_t transport_kind;
	const uint8_t *tandem_request;
	size_t tandem_request_bytes;
	const uint8_t *transport_request;
	size_t transport_request_bytes;
};

bool spf_metadata_request_is_envelope(const void *wire_request,
	size_t wire_bytes);
int spf_metadata_request_decode(struct spf_metadata_request *destination,
	const void *wire_request, size_t wire_bytes);

#endif
