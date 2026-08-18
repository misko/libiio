/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "spf-tandem-session.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

_Static_assert(sizeof(struct adi_tandem_agc_request_v1) == 104,
	"unexpected tandem request ABI size");
_Static_assert(sizeof(struct adi_tandem_agc_caps) == 56,
	"unexpected tandem capabilities ABI size");
_Static_assert(sizeof(struct adi_tandem_agc_status) == 76,
	"unexpected tandem status ABI size");
_Static_assert(sizeof(struct adi_tandem_agc_acquire) == 180,
	"unexpected tandem acquire ABI size");
_Static_assert(sizeof(struct adi_tandem_agc_event) == 16,
	"unexpected tandem event ABI size");

static uint16_t get_le16(const uint8_t *source)
{
	return (uint16_t)source[0] | (uint16_t)source[1] << 8;
}

static uint32_t get_le32(const uint8_t *source)
{
	return (uint32_t)source[0] | (uint32_t)source[1] << 8 |
		(uint32_t)source[2] << 16 | (uint32_t)source[3] << 24;
}

static int system_open(const char *path, int flags, void *opaque)
{
	(void)opaque;
	return open(path, flags);
}

static int system_ioctl(int fd, unsigned long request, void *argument,
	void *opaque)
{
	(void)opaque;
	return ioctl(fd, request, argument);
}

static ssize_t system_read(int fd, void *destination, size_t bytes,
	void *opaque)
{
	(void)opaque;
	return read(fd, destination, bytes);
}

static int system_close(int fd, void *opaque)
{
	(void)opaque;
	return close(fd);
}

int spf_tandem_request_decode(struct adi_tandem_agc_request_v1 *destination,
	const void *wire_request, size_t wire_bytes)
{
	const uint8_t *wire = wire_request;
	uint32_t *reserved;
	unsigned int index;

	if (!destination || !wire || wire_bytes != 104)
		return -EINVAL;
	memset(destination, 0, sizeof(*destination));
	destination->magic = get_le32(wire + 0);
	destination->version = get_le16(wire + 4);
	destination->size = get_le16(wire + 6);
	destination->required_features = get_le32(wire + 8);
	destination->mode = get_le32(wire + 12);
	destination->observation_capacity = get_le32(wire + 16);
	destination->event_capacity = get_le32(wire + 20);
	destination->minimum_gain_db = (int32_t)get_le32(wire + 24);
	destination->maximum_gain_db = (int32_t)get_le32(wire + 28);
	destination->initial_gain_db = (int32_t)get_le32(wire + 32);
	destination->power_measurement_samples = get_le32(wire + 36);
	destination->low_power_dwell_periods = get_le32(wire + 40);
	destination->cooldown_periods = get_le32(wire + 44);
	destination->pulse_high_cycles = get_le32(wire + 48);
	destination->pulse_low_cycles = get_le32(wire + 52);
	destination->detector_blanking_cycles = get_le32(wire + 56);
	destination->low_power_threshold = wire[60];
	destination->large_lmt_overload_threshold = wire[61];
	destination->large_adc_overload_threshold = wire[62];
	destination->small_adc_overload_threshold = wire[63];
	destination->overflow_policy = get_le32(wire + 64);
	destination->sync_fault_policy = get_le32(wire + 68);
	reserved = destination->reserved;
	for (index = 0; index < 8; ++index)
		reserved[index] = get_le32(wire + 72 + index * 4);

	if (destination->magic != ADI_TANDEM_AGC_REQUEST_MAGIC ||
		destination->version != ADI_TANDEM_AGC_ABI_VERSION ||
		destination->size != sizeof(*destination))
		return -EPROTONOSUPPORT;
	for (index = 0; index < 8; ++index)
		if (reserved[index])
			return -EINVAL;
	return 0;
}

int spf_tandem_session_init(struct spf_tandem_session *session,
	const void *wire_request, size_t wire_bytes,
	const struct spf_tandem_syscalls *syscalls)
{
	int ret;

	if (!session)
		return -EINVAL;
	memset(session, 0, sizeof(*session));
	session->fd = -1;
	ret = spf_tandem_request_decode(&session->request,
		wire_request, wire_bytes);
	if (ret)
		return ret;
	if (syscalls)
		session->syscalls = *syscalls;
	else {
		session->syscalls.open_device = system_open;
		session->syscalls.ioctl_device = system_ioctl;
		session->syscalls.read_device = system_read;
		session->syscalls.close_device = system_close;
	}
	if (!session->syscalls.open_device || !session->syscalls.ioctl_device ||
		!session->syscalls.read_device || !session->syscalls.close_device)
		return -EINVAL;
	return 0;
}

static int refresh_status(struct spf_tandem_session *session)
{
	struct adi_tandem_agc_status status = {
		.version = ADI_TANDEM_AGC_ABI_VERSION,
		.size = sizeof(status),
	};

	if (session->syscalls.ioctl_device(session->fd,
			ADI_TANDEM_AGC_IOC_GET_STATUS, &status,
			session->syscalls.opaque) < 0)
		return -errno;
	if (status.version != ADI_TANDEM_AGC_ABI_VERSION ||
		status.size != sizeof(status) ||
		status.ownership_epoch != session->status.ownership_epoch)
		return -EPROTO;
	if (status.fault_flags)
		return -EIO;
	if (status.overflow_count != session->initial_overflow_count)
		return -EOVERFLOW;
	session->status = status;
	return 0;
}

int spf_tandem_session_acquire(struct spf_tandem_session *session)
{
	struct adi_tandem_agc_caps caps = {
		.version = ADI_TANDEM_AGC_ABI_VERSION,
		.size = sizeof(caps),
	};
	struct adi_tandem_agc_acquire acquire;
	int ret;

	if (!session || session->fd >= 0 || session->acquired)
		return -EINVAL;
	session->fd = session->syscalls.open_device(SPF_TANDEM_DEVICE,
		O_RDWR | O_CLOEXEC | O_NONBLOCK, session->syscalls.opaque);
	if (session->fd < 0)
		return -errno;
	if (session->syscalls.ioctl_device(session->fd,
			ADI_TANDEM_AGC_IOC_GET_CAPS, &caps,
			session->syscalls.opaque) < 0) {
		ret = -errno;
		goto fail;
	}
	if (caps.version != ADI_TANDEM_AGC_ABI_VERSION ||
		caps.size != sizeof(caps) ||
		(caps.features & session->request.required_features) !=
			session->request.required_features ||
		caps.event_size != sizeof(struct adi_tandem_agc_event) ||
		session->request.event_capacity > caps.fifo_depth) {
		ret = -EPROTONOSUPPORT;
		goto fail;
	}
	memset(&acquire, 0, sizeof(acquire));
	acquire.request = session->request;
	if (session->syscalls.ioctl_device(session->fd,
			ADI_TANDEM_AGC_IOC_ACQUIRE, &acquire,
			session->syscalls.opaque) < 0) {
		ret = -errno;
		goto fail;
	}
	if (acquire.status.version != ADI_TANDEM_AGC_ABI_VERSION ||
		acquire.status.size != sizeof(acquire.status) ||
		!acquire.status.ownership_epoch || acquire.status.fault_flags) {
		ret = -EPROTO;
		goto fail;
	}
	session->status = acquire.status;
	session->initial_overflow_count = acquire.status.overflow_count;
	session->frame_transition_count = acquire.status.transition_count;
	session->frame_rx1_gain_index = acquire.status.rx1_gain_index;
	session->frame_rx2_gain_index = acquire.status.rx2_gain_index;
	session->acquired = true;
	return 0;

fail:
	(void)session->syscalls.close_device(session->fd,
		session->syscalls.opaque);
	session->fd = -1;
	return ret;
}

static int fill_queue(struct spf_tandem_session *session)
{
	ssize_t bytes;
	size_t available;

	for (;;) {
		available = SPF_TANDEM_EVENT_QUEUE_CAPACITY - session->queue_count;
		if (!available)
			return -ENOSPC;
		bytes = session->syscalls.read_device(session->fd,
			&session->queue[session->queue_count],
			available * sizeof(session->queue[0]),
			session->syscalls.opaque);
		if (bytes < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return -errno;
		}
		if (!bytes)
			return -EPIPE;
		if ((size_t)bytes % sizeof(session->queue[0]))
			return -EPROTO;
		session->queue_count += (size_t)bytes / sizeof(session->queue[0]);
		if (session->queue_count == SPF_TANDEM_EVENT_QUEUE_CAPACITY)
			return 0;
	}
}

int spf_tandem_session_collect(struct spf_tandem_session *session,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	struct adi_tandem_agc_event *events, size_t event_capacity,
	size_t *event_count)
{
	uint64_t end;
	size_t consumed = 0;
	size_t produced = 0;
	int ret;

	if (!session || !session->acquired || !samples_per_channel ||
		(!events && event_capacity) || !event_count)
		return -EINVAL;
	end = first_sample_sequence + samples_per_channel;
	if (end < first_sample_sequence)
		return -ERANGE;
	ret = fill_queue(session);
	if (ret)
		return ret;
	while (consumed < session->queue_count) {
		const struct adi_tandem_agc_event *event =
			&session->queue[consumed];
		if (event->sample_sequence >= end)
			break;
		if (session->sequence_valid &&
			event->event_sequence != session->expected_event_sequence)
			return -EILSEQ;
		if (session->sample_sequence_valid && event->sample_sequence <
			session->last_event_sample_sequence)
			return -EILSEQ;
		if (event->rx1_gain_index != event->rx2_gain_index ||
			event->rx1_gain_index > 0x7f || event->flags & 0xff00 ||
			((event->flags >> 4) & 0x3) < 1 ||
			((event->flags >> 4) & 0x3) > 2 ||
			(event->flags & 0xf) > 6)
			return -EPROTO;
		session->expected_event_sequence = event->event_sequence + 1U;
		session->sequence_valid = true;
		session->last_event_sample_sequence = event->sample_sequence;
		session->sample_sequence_valid = true;
		session->frame_rx1_gain_index = event->rx1_gain_index;
		session->frame_rx2_gain_index = event->rx2_gain_index;
		session->frame_transition_count++;
		if (event->sample_sequence >= first_sample_sequence) {
			if (produced == event_capacity)
				return -ENOSPC;
			events[produced++] = *event;
		}
		consumed++;
	}
	if (consumed) {
		memmove(session->queue, &session->queue[consumed],
			(session->queue_count - consumed) * sizeof(session->queue[0]));
		session->queue_count -= consumed;
	}
	ret = refresh_status(session);
	if (ret)
		return ret;
	if (session->status.transition_count < session->frame_transition_count)
		return -EILSEQ;
	session->status.transition_count = session->frame_transition_count;
	session->status.rx1_gain_index = session->frame_rx1_gain_index;
	session->status.rx2_gain_index = session->frame_rx2_gain_index;
	*event_count = produced;
	return 0;
}

void spf_tandem_session_close(struct spf_tandem_session *session)
{
	if (!session || session->fd < 0)
		return;
	if (session->acquired)
		(void)session->syscalls.ioctl_device(session->fd,
			ADI_TANDEM_AGC_IOC_RELEASE, NULL, session->syscalls.opaque);
	(void)session->syscalls.close_device(session->fd,
		session->syscalls.opaque);
	session->fd = -1;
	session->acquired = false;
	session->queue_count = 0;
}
