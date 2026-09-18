/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SPF_SCAN_RADIO_H
#define SPF_SCAN_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPF_SCAN_RADIO_MAX_PROFILES 8U

struct spf_scan_radio_profile {
	uint32_t profile;
	uint64_t frequency_hz;
	uint32_t crc32;
};

struct spf_scan_radio_receipt {
	uint32_t profile;
	uint64_t frequency_hz;
	uint32_t profile_crc32;
	uint64_t counter_before;
	uint64_t counter_after;
};
struct spf_scan_radio_release_receipt {
	uint64_t frequency_hz, counter_before, counter_after;
};

typedef int (*spf_scan_radio_ioctl_fn)(int fd, unsigned long request,
					      void *argument);

struct spf_scan_radio {
	int fd;
	spf_scan_radio_ioctl_fn call_ioctl;
	uint32_t profile_mask;
	uint64_t frequency_hz[SPF_SCAN_RADIO_MAX_PROFILES];
	uint32_t profile_crc32[SPF_SCAN_RADIO_MAX_PROFILES];
	uint32_t features;
	uint32_t scan_mask;
	bool acquired;
	bool configured;
	bool faulted;
	bool released;
};

int spf_scan_radio_init(struct spf_scan_radio *radio, int fd,
			spf_scan_radio_ioctl_fn call_ioctl);
int spf_scan_radio_acquire(struct spf_scan_radio *radio, uint32_t source_rate_hz,
			   uint32_t samples_per_block, uint32_t scan_mask);
int spf_scan_radio_configure(struct spf_scan_radio *radio,
			     const struct spf_scan_radio_profile *profiles,
			     size_t profile_count);
int spf_scan_radio_recall(struct spf_scan_radio *radio, uint32_t profile,
			  uint64_t counter_anchor,
			  struct spf_scan_radio_receipt *receipt);
int spf_scan_radio_snapshot(struct spf_scan_radio *radio,
			    uint64_t counter_anchor, uint64_t *counter);
int spf_scan_radio_release(struct spf_scan_radio *radio, uint64_t counter_anchor,
			   struct spf_scan_radio_release_receipt *receipt);

#endif
