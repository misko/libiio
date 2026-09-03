/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "spf-hop-device.h"
#include "spf-hop-scheduler.h"
#include "spf-tandem-session.h"

#include <spf_gain_metadata.h>
#include <spf_gain_sampler.h>

#include <errno.h>
#include <fcntl.h>
#include <iio.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SPF_USERSPACE_HOP_LO_SEARCH_HZ 16

struct spf_userspace_hop_io {
	struct iio_device *rx;
	struct iio_channel *lo;
	struct spf_tandem_session *tandem;
	pthread_mutex_t *tandem_lock;
	uint64_t counter_reference;
	uint64_t next_device_event_id;
	bool counter_valid;
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
	const struct spf_hop_request_v1 *request, struct iio_channel **lo_out)
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
		!iio_channel_find_attr(lo, "fastlock_recall")) {
		fprintf(stderr, "SPF userspace hop lacks required RX LO attributes\n");
		return -EOPNOTSUPP;
	}
	ret = iio_channel_attr_read_longlong(lo, "frequency", &actual_lo);
	if (ret || actual_lo <= 0 || (uint64_t)actual_lo !=
			request->profiles[0].lo_frequency_hz) {
		fprintf(stderr,
			"SPF userspace hop initial LO mismatch: actual=%lld expected=%llu "
			"error=%d\n", actual_lo,
			(unsigned long long)request->profiles[0].lo_frequency_hz, ret);
		return ret ? ret : -ESTALE;
	}
	for (i = 0; i < SPF_HOP_PROFILE_COUNT; i++) {
		ret = iio_channel_attr_write_longlong(lo, "fastlock_save",
			request->profiles[i].fastlock_slot);
		if (ret < 0) {
			fprintf(stderr,
				"SPF userspace hop Fast Lock save %u write failed: %d\n",
				i, ret);
			return ret;
		}
		bytes = iio_channel_attr_read(lo, "fastlock_save", saved,
			sizeof(saved) - 1U);
		if (bytes <= 0 || (size_t)bytes >= sizeof(saved)) {
			fprintf(stderr,
				"SPF userspace hop Fast Lock save %u read failed: %zd\n",
				i, bytes);
			return bytes < 0 ? (int)bytes : -EMSGSIZE;
		}
		saved[bytes] = '\0';
		ret = parse_fastlock_profile(saved,
			request->profiles[i].fastlock_slot, values);
		if (ret) {
			fprintf(stderr,
				"SPF userspace hop Fast Lock save %u parse failed: %d\n",
				i, ret);
			return ret;
		}
		if (crc32_bytes(values, sizeof(values)) !=
				request->profiles[i].profile_crc32) {
			fprintf(stderr,
				"SPF userspace hop Fast Lock save %u CRC mismatch: "
				"actual=%08x expected=%08x\n", i,
				crc32_bytes(values, sizeof(values)),
				request->profiles[i].profile_crc32);
			return -ESTALE;
		}
	}
	*lo_out = lo;
	return 0;
}

static uint64_t extend_counter_near(uint64_t reference, uint32_t low)
{
	uint64_t candidate = (reference & UINT64_C(0xffffffff00000000)) | low;

	if (candidate < reference &&
			reference - candidate > UINT64_C(0x80000000))
		candidate += UINT64_C(0x100000000);
	else if (candidate > reference &&
			candidate - reference > UINT64_C(0x80000000) &&
			candidate >= UINT64_C(0x100000000))
		candidate -= UINT64_C(0x100000000);
	return candidate;
}

static int read_counter(struct spf_userspace_hop_io *io, uint64_t *counter)
{
	uint32_t low;
	uint64_t extended;

	if (!io || !counter || iio_device_reg_read(io->rx,
			SPF_ADC_SAMPLE_COUNTER_LOW_REG, &low) != 0)
		return -EIO;
	extended = io->counter_valid ?
		extend_counter_near(io->counter_reference, low) : (uint64_t)low;
	if (io->counter_valid && extended < io->counter_reference)
		return -EILSEQ;
	io->counter_reference = extended;
	io->counter_valid = true;
	*counter = extended;
	return 0;
}

static int read_lo(struct spf_userspace_hop_io *io, uint64_t expected,
	uint64_t *actual)
{
	long long value;
	int ret;

	ret = iio_channel_attr_read_longlong(io->lo, "frequency", &value);
	if (ret < 0) {
		fprintf(stderr, "SPF userspace hop LO read failed: error=%d\n", ret);
		return ret;
	}
	if (value <= 0 || (uint64_t)value != expected) {
		fprintf(stderr,
			"SPF userspace hop LO mismatch: actual=%lld expected=%llu\n",
			value, (unsigned long long)expected);
		return -ESTALE;
	}
	*actual = (uint64_t)value;
	return 0;
}

static int read_active_profile(struct spf_userspace_hop_io *io,
	uint32_t expected)
{
	long long active;
	int ret;

	ret = iio_channel_attr_read_longlong(io->lo, "fastlock_recall", &active);
	if (ret < 0) {
		fprintf(stderr,
			"SPF userspace hop active-profile read failed: expected=%u error=%d\n",
			expected, ret);
		return ret;
	}
	if (active != (long long)expected) {
		fprintf(stderr,
			"SPF userspace hop active-profile mismatch: actual=%lld expected=%u\n",
			active, expected);
		return -ESTALE;
	}
	return 0;
}

static int require_inactive_profile(struct spf_userspace_hop_io *io)
{
	long long active;
	int ret;

	ret = iio_channel_attr_read_longlong(io->lo, "fastlock_recall", &active);
	if (ret == -EINVAL)
		return 0;
	return ret < 0 ? ret : -ESTALE;
}

static int write_exact_conventional_lo(struct spf_userspace_hop_io *io,
	uint64_t expected)
{
	long long requested;
	long long actual;
	int step;
	int ret;

	if (expected > (uint64_t)LLONG_MAX)
		return -ERANGE;
	/* clk_set_rate may elide a write equal to its cached pre-Fast-Lock rate,
	 * leaving Fast Lock active.  First force one distinct conventional rate;
	 * capture has already stopped, so this transient tune cannot label IQ. */
	requested = (long long)expected;
	requested += requested <= LLONG_MAX -
		(SPF_USERSPACE_HOP_LO_SEARCH_HZ + 1) ?
		SPF_USERSPACE_HOP_LO_SEARCH_HZ + 1 :
		-(SPF_USERSPACE_HOP_LO_SEARCH_HZ + 1);
	ret = iio_channel_attr_write_longlong(io->lo, "frequency", requested);
	if (ret < 0)
		return ret;
	for (step = 0; step <= SPF_USERSPACE_HOP_LO_SEARCH_HZ; step++) {
		int direction;

		for (direction = step ? -1 : 0; direction <= (step ? 1 : 0);
				direction += 2) {
			requested = (long long)expected + direction * step;
			ret = iio_channel_attr_write_longlong(io->lo, "frequency",
				requested);
			if (ret < 0)
				return ret;
			ret = iio_channel_attr_read_longlong(io->lo, "frequency",
				&actual);
			if (ret < 0)
				return ret;
			if (actual == (long long)expected)
				return 0;
			if (!step)
				break;
		}
	}
	return -ESTALE;
}

static int userspace_start(void *opaque, uint64_t expected_original_lo_hz)
{
	struct spf_userspace_hop_io *io = opaque;
	uint64_t actual;
	uint64_t counter;
	int ret;

	ret = read_lo(io, expected_original_lo_hz, &actual);
	if (ret)
		return ret;
	ret = require_inactive_profile(io);
	if (ret)
		return ret;
	ret = read_counter(io, &counter);
	if (ret)
		return ret;
	io->next_device_event_id = 1;
	return 0;
}

static int userspace_get_counter(void *opaque, uint64_t *sample_counter)
{
	return read_counter(opaque, sample_counter);
}

static int userspace_recall(void *opaque, uint32_t profile,
	uint64_t expected_lo_hz,
	struct spf_hop_scheduler_transition_v1 *transition)
{
	struct spf_userspace_hop_io *io = opaque;
	uint64_t actual_lo = 0;
	uint64_t before = 0;
	uint64_t after = 0;
	int lock_ret;
	int ret;

	if (!transition || profile >= SPF_HOP_PROFILE_COUNT)
		return -EINVAL;
	lock_ret = pthread_mutex_lock(io->tandem_lock);
	if (lock_ret)
		return -lock_ret;
	ret = read_counter(io, &before);
	if (!ret) {
		spf_tandem_session_close(io->tandem);
		ret = iio_channel_attr_write_longlong(io->lo, "fastlock_recall",
			profile);
	}
	if (!ret)
		ret = read_active_profile(io, profile);
	/* The AD9361 clock framework caches the last conventional LO rate and its
	 * frequency attribute is therefore stale while Fast Lock is active.  OPEN
	 * already CRC-attested every volatile slot, and fastlock_recall readback
	 * attests which one the driver activated.  Together those are the truthful
	 * userspace proof of the requested LO; do not compare against the stale
	 * conventional-rate cache. */
	if (!ret)
		actual_lo = expected_lo_hz;
	if (!ret)
		ret = spf_tandem_session_acquire(io->tandem);
	if (!ret)
		ret = read_counter(io, &after);
	lock_ret = pthread_mutex_unlock(io->tandem_lock);
	if (!ret && lock_ret)
		ret = -lock_ret;
	if (ret) {
		fprintf(stderr,
			"SPF userspace hop recall failed: profile=%u expected_lo=%llu "
			"before=%llu after=%llu actual_lo=%llu error=%d\n",
			profile, (unsigned long long)expected_lo_hz,
			(unsigned long long)before, (unsigned long long)after,
			(unsigned long long)actual_lo, ret);
		return ret;
	}
	transition->transition_before = before;
	transition->transition_after = after;
	transition->actual_lo_frequency_hz = actual_lo;
	transition->device_event_id = io->next_device_event_id++;
	transition->active_profile = profile;
	return 0;
}

static int userspace_restore(void *opaque, uint64_t expected_original_lo_hz,
	struct spf_hop_scheduler_restore_v1 *restore)
{
	struct spf_userspace_hop_io *io = opaque;
	uint64_t actual_lo = 0;
	uint64_t before = 0;
	uint64_t after = 0;
	int after_ret;
	int before_ret;
	int lock_ret;
	int restore_ret;
	int ret;

	if (!restore)
		return -EINVAL;
	lock_ret = pthread_mutex_lock(io->tandem_lock);
	if (lock_ret)
		return -lock_ret;
	before_ret = read_counter(io, &before);
	spf_tandem_session_close(io->tandem);
	/* Counter evidence and hardware restoration are separate obligations.  A
	 * failed pre-restore counter read must fail the receipt, but must never
	 * prevent the best-effort conventional LO restore on disconnect/error. */
	restore_ret = write_exact_conventional_lo(io, expected_original_lo_hz);
	if (!restore_ret)
		restore_ret = read_lo(io, expected_original_lo_hz, &actual_lo);
	if (!restore_ret)
		restore_ret = require_inactive_profile(io);
	after_ret = read_counter(io, &after);
	ret = restore_ret ? restore_ret : (before_ret ? before_ret : after_ret);
	lock_ret = pthread_mutex_unlock(io->tandem_lock);
	if (!ret && lock_ret)
		ret = -lock_ret;
	restore->transition_before = before;
	restore->transition_after = after;
	restore->actual_lo_frequency_hz = actual_lo;
	restore->active_profile = restore_ret ? 0 : UINT32_MAX;
	if (ret)
		return ret;
	return 0;
}

static int userspace_sleep_ns(void *opaque, uint64_t nanoseconds)
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

static void userspace_destroy(void *opaque)
{
	free(opaque);
}

static const struct spf_hop_scheduler_io_v1 userspace_io = {
	.start = userspace_start,
	.get_counter = userspace_get_counter,
	.recall = userspace_recall,
	.restore = userspace_restore,
	.sleep_ns = userspace_sleep_ns,
	.destroy = userspace_destroy,
};

bool spf_hop_device_v1_capable(void)
{
	struct iio_context *context;
	struct iio_device *rx;
	struct iio_device *phy;
	struct iio_channel *lo;
	struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
	uint32_t first;
	uint32_t second;
	bool capable = false;
	int tandem_fd = -1;

	context = iio_create_local_context();
	if (!context)
		return false;
	rx = iio_context_find_device(context, "cf-ad9361-lpc");
	phy = iio_context_find_device(context, "ad9361-phy");
	lo = phy ? iio_device_find_channel(phy, "altvoltage0", true) : NULL;
	if (!rx || !lo || !iio_channel_find_attr(lo, "frequency") ||
		!iio_channel_find_attr(lo, "fastlock_save") ||
		!iio_channel_find_attr(lo, "fastlock_recall"))
		goto out;
	tandem_fd = open(SPF_TANDEM_DEVICE, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (tandem_fd < 0 || iio_device_reg_read(rx,
			SPF_ADC_SAMPLE_COUNTER_LOW_REG, &first) != 0)
		goto out;
	(void)nanosleep(&delay, NULL);
	if (iio_device_reg_read(rx, SPF_ADC_SAMPLE_COUNTER_LOW_REG, &second) != 0 ||
		second == first)
		goto out;
	capable = true;

out:
	if (tandem_fd >= 0 && close(tandem_fd) != 0)
		capable = false;
	iio_context_destroy(context);
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
	struct spf_userspace_hop_io *io;
	struct iio_channel *lo = NULL;
	int ret;

	if (!rx || !phy || !tandem || !tandem_lock || !request ||
		!device_context || !ops)
		return -EINVAL;
	context = iio_device_get_context(rx);
	context_name = context ? iio_context_get_name(context) : NULL;
	if (!context_name || strcmp(context_name, "local"))
		return -EOPNOTSUPP;
	ret = validate_profiles(phy, request, &lo);
	if (ret)
		return ret;
	io = calloc(1, sizeof(*io));
	if (!io)
		return -ENOMEM;
	io->rx = (struct iio_device *)rx;
	io->lo = lo;
	io->tandem = tandem;
	io->tandem_lock = tandem_lock;
	ret = spf_hop_scheduler_v1_create(request, &userspace_io, io,
		device_context, ops);
	if (ret)
		free(io);
	return ret;
}

void spf_hop_device_v1_destroy(void *device_context)
{
	spf_hop_scheduler_v1_destroy(device_context);
}
