/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-scan-radio.h"
#include "adi-rx-counter.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static int system_ioctl(int fd, unsigned long request, void *argument)
{
	return ioctl(fd, request, argument);
}

static bool frequency_matches(uint64_t actual, uint64_t expected)
{
	return actual >= expected ? actual - expected <= 2 : expected - actual <= 2;
}

static int extend_counter(uint64_t anchor, uint32_t low, uint64_t *extended)
{
	uint64_t candidate = (anchor & ~UINT64_C(0xffffffff)) | low;

	if (candidate < anchor) {
		if (candidate > UINT64_MAX - (UINT64_C(1) << 32))
			return -ERANGE;
		candidate += UINT64_C(1) << 32;
	}
	if (candidate - anchor > UINT32_MAX)
		return -ERANGE;
	*extended = candidate;
	return 0;
}

int spf_scan_radio_init(struct spf_scan_radio *radio, int fd,
			spf_scan_radio_ioctl_fn call_ioctl)
{
	struct adi_rx_counter_scan_caps caps = { 0 };
	int ret;

	if (!radio || fd < 0)
		return -EINVAL;
	memset(radio, 0, sizeof(*radio));
	radio->fd = fd;
	radio->call_ioctl = call_ioctl ? call_ioctl : system_ioctl;
	ret = radio->call_ioctl(fd, ADI_RX_COUNTER_IOC_GET_SCAN_CAPS, &caps);
	if (ret < 0)
		return -errno;
	if (caps.magic != ADI_RX_COUNTER_MAGIC ||
	    caps.version != ADI_RX_COUNTER_SCAN_VERSION ||
	    caps.size != sizeof(caps) ||
	    caps.features != ADI_RX_COUNTER_SCAN_FEATURES ||
	    caps.maximum_profiles != SPF_SCAN_RADIO_MAX_PROFILES ||
	    caps.source_counter_bits != 32 || caps.frequency_resolution_hz != 2 ||
	    caps.reserved[0] || caps.reserved[1])
		return -EPROTO;
	radio->features = caps.features;
	return 0;
}

int spf_scan_radio_configure(struct spf_scan_radio *radio,
			     const struct spf_scan_radio_profile *profiles,
			     size_t profile_count)
{
	struct adi_rx_counter_scan_config config = { 0 };
	size_t i;

	if (!radio || !profiles || !profile_count ||
	    profile_count > SPF_SCAN_RADIO_MAX_PROFILES || radio->configured ||
	    radio->faulted)
		return -EINVAL;
	config.magic = ADI_RX_COUNTER_MAGIC;
	config.version = ADI_RX_COUNTER_SCAN_VERSION;
	config.size = sizeof(config);
	for (i = 0; i < profile_count; i++) {
		uint32_t profile = profiles[i].profile;

		if (profile >= SPF_SCAN_RADIO_MAX_PROFILES ||
		    !profiles[i].frequency_hz || config.profile_mask & (1U << profile))
			return -EINVAL;
		config.profile_mask |= 1U << profile;
		config.profiles[profile].frequency_hz = profiles[i].frequency_hz;
		config.profiles[profile].crc32 = profiles[i].crc32;
	}
	if (radio->call_ioctl(radio->fd, ADI_RX_COUNTER_IOC_CONFIGURE_SCAN,
			      &config) < 0)
		return -errno;

	for (i = 0; i < SPF_SCAN_RADIO_MAX_PROFILES; i++) {
		radio->frequency_hz[i] = config.profiles[i].frequency_hz;
		radio->profile_crc32[i] = config.profiles[i].crc32;
	}
	radio->profile_mask = config.profile_mask;
	radio->configured = true;
	return 0;
}

int spf_scan_radio_recall(struct spf_scan_radio *radio, uint32_t profile,
			  uint64_t counter_anchor,
			  struct spf_scan_radio_receipt *receipt)
{
	struct adi_rx_counter_scan_recall recall = { 0 };
	uint64_t before, after;
	int ret;

	if (!radio || !receipt || !radio->configured || radio->faulted ||
	    radio->released ||
	    profile >= SPF_SCAN_RADIO_MAX_PROFILES ||
	    !(radio->profile_mask & (1U << profile)))
		return -EINVAL;
	recall.magic = ADI_RX_COUNTER_MAGIC;
	recall.version = ADI_RX_COUNTER_SCAN_VERSION;
	recall.size = sizeof(recall);
	recall.profile = profile;
	if (radio->call_ioctl(radio->fd, ADI_RX_COUNTER_IOC_RECALL, &recall) < 0) {
		radio->faulted = true;
		return -errno;
	}
	if (!frequency_matches(recall.frequency_hz,
			       radio->frequency_hz[profile])) {
		radio->faulted = true;
		return -EPROTO;
	}
	ret = extend_counter(counter_anchor, recall.counter_before, &before);
	if (!ret)
		ret = extend_counter(before, recall.counter_after, &after);
	if (ret) {
		radio->faulted = true;
		return ret;
	}

	radio->profile_crc32[profile] = recall.profile_crc32;
	receipt->profile = profile;
	receipt->frequency_hz = recall.frequency_hz;
	receipt->profile_crc32 = recall.profile_crc32;
	receipt->counter_before = before;
	receipt->counter_after = after;
	return 0;
}

int spf_scan_radio_release(struct spf_scan_radio *radio, uint64_t counter_anchor,
			   struct spf_scan_radio_release_receipt *receipt)
{
	struct adi_rx_counter_scan_release release = { 0 };
	uint64_t before, after;
	int ret;

	if (!radio || !receipt || !radio->configured || radio->released)
		return -EINVAL;
	release.magic = ADI_RX_COUNTER_MAGIC;
	release.version = ADI_RX_COUNTER_SCAN_VERSION;
	release.size = sizeof(release);
	if (radio->call_ioctl(radio->fd, ADI_RX_COUNTER_IOC_RELEASE_SCAN,
			      &release) < 0) {
		radio->faulted = true;
		return -errno;
	}
	ret = extend_counter(counter_anchor, release.counter_before, &before);
	if (!ret)
		ret = extend_counter(before, release.counter_after, &after);
	if (ret || !release.frequency_hz) {
		radio->faulted = true;
		return ret ? ret : -EPROTO;
	}
	receipt->frequency_hz = release.frequency_hz;
	receipt->counter_before = before;
	receipt->counter_after = after;
	radio->released = true;
	return 0;
}
