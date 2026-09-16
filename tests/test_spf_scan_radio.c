/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "adi-rx-counter.h"
#include "spf-scan-radio.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(struct adi_rx_counter_request) == 32, "acquire ABI");
_Static_assert(sizeof(struct adi_rx_counter_scan_profile) == 16, "profile ABI");
_Static_assert(sizeof(struct adi_rx_counter_scan_config) == 144, "config ABI");
_Static_assert(sizeof(struct adi_rx_counter_scan_recall) == 48, "recall ABI");
_Static_assert(sizeof(struct adi_rx_counter_scan_caps) == 32, "caps ABI");
_Static_assert(offsetof(struct adi_rx_counter_scan_recall, frequency_hz) == 16,
	       "recall alignment");

static struct adi_rx_counter_scan_config observed_config;
static uint32_t result_before = UINT32_C(0xfffffff0);
static uint32_t result_after = UINT32_C(0x20);
static uint64_t result_frequency = UINT64_C(2400000000);
static uint32_t result_crc = UINT32_C(0xa1b2c3d4);
static int fail_recall;

static int mock_ioctl(int fd, unsigned long request, void *argument)
{
	assert(fd == 17);
	if (request == ADI_RX_COUNTER_IOC_GET_SCAN_CAPS) {
		struct adi_rx_counter_scan_caps *caps = argument;

		memset(caps, 0, sizeof(*caps));
		caps->magic = ADI_RX_COUNTER_MAGIC;
		caps->version = ADI_RX_COUNTER_SCAN_VERSION;
		caps->size = sizeof(*caps);
		caps->features = ADI_RX_COUNTER_SCAN_FEATURES;
		caps->maximum_profiles = ADI_RX_COUNTER_SCAN_MAX_PROFILES;
		caps->source_counter_bits = 32;
		caps->frequency_resolution_hz = 2;
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_CONFIGURE_SCAN) {
		memcpy(&observed_config, argument, sizeof(observed_config));
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_RECALL) {
		struct adi_rx_counter_scan_recall *recall = argument;

		assert(recall->magic == ADI_RX_COUNTER_MAGIC);
		assert(recall->version == ADI_RX_COUNTER_SCAN_VERSION);
		assert(recall->size == sizeof(*recall));
		assert(recall->profile == 3);
		assert(!recall->flags && !recall->frequency_hz &&
		       !recall->profile_crc32 && !recall->counter_before &&
		       !recall->counter_after && !recall->reserved[0] &&
		       !recall->reserved[1] && !recall->reserved[2]);
		if (fail_recall) {
			errno = EIO;
			return -1;
		}
		recall->frequency_hz = result_frequency;
		recall->profile_crc32 = result_crc;
		recall->counter_before = result_before;
		recall->counter_after = result_after;
		return 0;
	}
	assert(!"unexpected ioctl");
	return -1;
}

int main(void)
{
	const struct spf_scan_radio_profile profiles[] = {
		{ .profile = 3, .frequency_hz = UINT64_C(2400000000),
		  .crc32 = UINT32_C(0x10203040) },
		{ .profile = 7, .frequency_hz = UINT64_C(2450000000),
		  .crc32 = UINT32_C(0x50607080) },
	};
	struct spf_scan_radio_receipt receipt;
	struct spf_scan_radio radio;
	struct spf_scan_radio_profile duplicate[2] = { profiles[0], profiles[0] };

	assert(spf_scan_radio_init(&radio, 17, mock_ioctl) == 0);
	assert(radio.features == ADI_RX_COUNTER_SCAN_FEATURES);
	assert(spf_scan_radio_configure(&radio, duplicate, 2) == -EINVAL);
	assert(spf_scan_radio_configure(&radio, profiles, 2) == 0);
	assert(observed_config.magic == ADI_RX_COUNTER_MAGIC);
	assert(observed_config.profile_mask == ((1U << 3) | (1U << 7)));
	assert(observed_config.profiles[3].frequency_hz == UINT64_C(2400000000));
	assert(observed_config.profiles[3].crc32 == UINT32_C(0x10203040));
	assert(!observed_config.profiles[0].frequency_hz);
	assert(spf_scan_radio_recall(&radio, 3, UINT64_C(0x1ffffffe0),
				     &receipt) == 0);
	assert(receipt.counter_before == UINT64_C(0x1fffffff0));
	assert(receipt.counter_after == UINT64_C(0x200000020));
	assert(receipt.frequency_hz == UINT64_C(2400000000));
	assert(receipt.profile_crc32 == result_crc);

	result_frequency += 10;
	assert(spf_scan_radio_recall(&radio, 3, receipt.counter_after,
				     &receipt) == -EPROTO);
	assert(radio.faulted);
	assert(spf_scan_radio_recall(&radio, 3, 0, &receipt) == -EINVAL);

	assert(spf_scan_radio_init(&radio, 17, mock_ioctl) == 0);
	assert(spf_scan_radio_configure(&radio, profiles, 2) == 0);
	result_frequency = profiles[0].frequency_hz;
	fail_recall = 1;
	assert(spf_scan_radio_recall(&radio, 3, 0, &receipt) == -EIO);
	assert(radio.faulted);

	puts("PASS: scan UAPI layout, whitelist, wrap receipts and fail-closed recall");
	return 0;
}
