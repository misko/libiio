/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "spf-hop-device.h"
#include "spf-hop-scheduler.h"
#include "spf-tandem-session.h"

#include <errno.h>
#include <fcntl.h>
#include <iio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct spf_local_hop_io {
	struct spf_tandem_session *tandem;
};

static uint32_t crc32_bytes(const uint8_t *bytes, size_t count)
{
	uint32_t crc = UINT32_MAX;
	size_t i;
	unsigned int bit;

	for (i = 0; i < count; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
			crc = crc >> 1 ^ (UINT32_C(0xedb88320) &
				(uint32_t)-(int32_t)(crc & 1U));
	}
	return ~crc;
}

static int parse_fastlock_profile(const char *text, uint32_t expected_slot,
	uint8_t values[16])
{
	char *end;
	unsigned long value;
	unsigned int i;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || end == text || value != expected_slot || *end != ' ')
		return -EBADMSG;
	text = end + 1;
	for (i = 0; i < 16; i++) {
		errno = 0;
		value = strtoul(text, &end, 10);
		if (errno || end == text || value > UINT8_MAX)
			return -EBADMSG;
		values[i] = (uint8_t)value;
		if (i != 15) {
			if (*end != ',')
				return -EBADMSG;
			text = end + 1;
		} else {
			while (*end == '\n' || *end == '\r')
				end++;
			if (*end)
				return -EBADMSG;
		}
	}
	return 0;
}

static int validate_profiles(const struct iio_device *phy,
	const struct spf_hop_request_v1 *request)
{
	struct iio_channel *lo;
	uint8_t values[16];
	char saved[256];
	long long actual_lo;
	unsigned int i;
	ssize_t bytes;
	int ret;

	lo = iio_device_find_channel(phy, "altvoltage0", true);
	if (!lo || !iio_channel_find_attr(lo, "frequency") ||
		!iio_channel_find_attr(lo, "fastlock_save") ||
		!iio_channel_find_attr(lo, "fastlock_recall"))
		return -EOPNOTSUPP;
	ret = iio_channel_attr_read_longlong(lo, "frequency", &actual_lo);
	if (ret || actual_lo <= 0 || (uint64_t)actual_lo !=
			request->profiles[0].lo_frequency_hz)
		return ret ? ret : -ESTALE;
	/* Profile preparation is host-side and bufferless. This validation runs
	 * before tandem acquisition, while selecting fastlock_save is still legal. */
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		ret = iio_channel_attr_write_longlong(lo, "fastlock_save",
			request->profiles[i].fastlock_slot);
		if (ret < 0)
			return ret;
		bytes = iio_channel_attr_read(lo, "fastlock_save", saved,
			sizeof(saved) - 1U);
		if (bytes <= 0 || (size_t)bytes >= sizeof(saved))
			return bytes < 0 ? (int)bytes : -EMSGSIZE;
		saved[bytes] = '\0';
		ret = parse_fastlock_profile(saved,
			request->profiles[i].fastlock_slot, values);
		if (ret)
			return ret;
		if (crc32_bytes(values, sizeof(values)) !=
				request->profiles[i].profile_crc32)
			return -ESTALE;
	}
	return 0;
}

static int local_start(void *opaque, uint64_t expected_original_lo_hz)
{
	struct spf_local_hop_io *io = opaque;

	return spf_tandem_session_hop_start(io->tandem,
		expected_original_lo_hz);
}

static int local_get_counter(void *opaque, uint64_t *sample_counter)
{
	struct spf_local_hop_io *io = opaque;

	return spf_tandem_session_hop_get_counter(io->tandem, sample_counter);
}

static int local_recall(void *opaque, uint32_t profile,
	uint64_t expected_lo_hz,
	struct spf_hop_scheduler_transition_v1 *transition)
{
	struct adi_persistent_hop_transition_v1 kernel_transition;
	struct spf_local_hop_io *io = opaque;
	int ret;

	ret = spf_tandem_session_hop_recall(io->tandem, profile,
		expected_lo_hz, &kernel_transition);
	if (ret)
		return ret;
	transition->transition_before = kernel_transition.transition_before;
	transition->transition_after = kernel_transition.transition_after;
	transition->actual_lo_frequency_hz = kernel_transition.actual_lo_hz;
	transition->device_event_id = kernel_transition.device_event_id;
	transition->active_profile = kernel_transition.active_profile;
	return 0;
}

static int local_restore(void *opaque, uint64_t expected_original_lo_hz,
	struct spf_hop_scheduler_restore_v1 *restore)
{
	struct adi_persistent_hop_restore_v1 kernel_restore;
	struct spf_local_hop_io *io = opaque;
	int ret;

	ret = spf_tandem_session_hop_restore(io->tandem,
		expected_original_lo_hz, &kernel_restore);
	if (ret)
		return ret;
	restore->transition_before = kernel_restore.transition_before;
	restore->transition_after = kernel_restore.transition_after;
	restore->actual_lo_frequency_hz = kernel_restore.actual_lo_hz;
	restore->active_profile = kernel_restore.active_profile;
	return 0;
}

static int local_sleep_ns(void *opaque, uint64_t nanoseconds)
{
	struct timespec duration;

	(void)opaque;
	duration.tv_sec = (time_t)(nanoseconds / UINT64_C(1000000000));
	duration.tv_nsec = (long)(nanoseconds % UINT64_C(1000000000));
	while (nanosleep(&duration, &duration) < 0)
		if (errno != EINTR)
			return -errno;
	return 0;
}

static void local_destroy(void *opaque)
{
	free(opaque);
}

static const struct spf_hop_scheduler_io_v1 local_io = {
	.start = local_start,
	.get_counter = local_get_counter,
	.recall = local_recall,
	.restore = local_restore,
	.sleep_ns = local_sleep_ns,
	.destroy = local_destroy,
};

bool spf_hop_device_v1_capable(void)
{
	struct adi_persistent_hop_caps_v1 caps = {0};
	bool capable = false;
	unsigned int i;
	int fd;

	fd = open(SPF_TANDEM_DEVICE, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return false;
	if (ioctl(fd, ADI_PERSISTENT_HOP_IOC_GET_CAPS, &caps) == 0 &&
		caps.version == ADI_PERSISTENT_HOP_ABI_VERSION &&
		caps.size == sizeof(caps) &&
		caps.features == ADI_PERSISTENT_HOP_REQUIRED_FEATURES &&
		caps.maximum_profiles == SPF_HOP_PROFILE_COUNT &&
		caps.fpga_identity == UINT32_C(0x54414732) && caps.fpga_abi == 2U) {
		capable = true;
		for (i = 0; i < sizeof(caps.reserved) / sizeof(caps.reserved[0]);
				i++)
			if (caps.reserved[i])
				capable = false;
	}
	if (close(fd))
		return false;
	return capable;
}

int spf_hop_device_v1_open(const struct iio_device *rx,
	const struct iio_device *phy, struct spf_tandem_session *tandem,
	pthread_mutex_t *tandem_lock,
	const struct spf_hop_request_v1 *request, void **device_context,
	const struct spf_hop_device_ops_v1 **ops)
{
	const struct iio_context *context;
	const char *context_name;
	struct spf_local_hop_io *io;
	int ret;

	if (!rx || !phy || !tandem || !tandem_lock || !request ||
		!device_context || !ops)
		return -EINVAL;
	(void)tandem_lock;
	context = iio_device_get_context(rx);
	context_name = context ? iio_context_get_name(context) : NULL;
	if (!context_name || strcmp(context_name, "local"))
		return -EOPNOTSUPP;
	ret = validate_profiles(phy, request);
	if (ret)
		return ret;
	io = calloc(1, sizeof(*io));
	if (!io)
		return -ENOMEM;
	io->tandem = tandem;
	ret = spf_hop_scheduler_v1_create(request, &local_io, io,
		device_context, ops);
	if (ret)
		free(io);
	return ret;
}

void spf_hop_device_v1_destroy(void *device_context)
{
	spf_hop_scheduler_v1_destroy(device_context);
}
