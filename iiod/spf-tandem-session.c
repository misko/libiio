/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "spf-tandem-session.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SPF_TANDEM_STATUS_CATCHUP_ATTEMPTS 4
#define SPF_TANDEM_TRANSITION_COUNTER_BITS 8U
#define SPF_TANDEM_TRANSITION_COUNTER_MASK \
	((1U << SPF_TANDEM_TRANSITION_COUNTER_BITS) - 1U)
#define SPF_TANDEM_TRANSITION_COUNTER_HALF_RANGE \
	(1U << (SPF_TANDEM_TRANSITION_COUNTER_BITS - 1U))
#define SPF_TANDEM_HOLD_OBSERVATIONS_PER_FRAME 4U
#define SPF_TANDEM_OBSERVATION_INTERVAL_MIN 1024U
#define SPF_TANDEM_AUTO_OBSERVATION_INTERVAL_MAX 32768U

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

int spf_tandem_request_validate_event_window(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel)
{
	uint64_t minimum_transition_samples;
	uint64_t retention_samples;
	uint64_t maximum_retained_events;

	if (!request || !samples_per_channel ||
		!request->power_measurement_samples || !request->event_capacity)
		return -EINVAL;
	if (request->mode == ADI_TANDEM_AGC_MODE_HOLD)
		return 0;
	if (request->mode != ADI_TANDEM_AGC_MODE_AUTO)
		return -EINVAL;
	minimum_transition_samples =
		(uint64_t)request->power_measurement_samples *
		((uint64_t)request->cooldown_periods + 1U);
	retention_samples = (uint64_t)samples_per_channel *
		SPF_TANDEM_EVENT_RETENTION_FRAMES;
	maximum_retained_events = 1U +
		(retention_samples - 1U) / minimum_transition_samples;
	return maximum_retained_events <= request->event_capacity ? 0 : -ENOSPC;
}

int spf_tandem_request_observation_interval(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel, uint32_t *interval_samples)
{
	uint32_t interval;

	if (!request || !samples_per_channel || !interval_samples)
		return -EINVAL;
	if (request->mode != ADI_TANDEM_AGC_MODE_HOLD &&
		request->mode != ADI_TANDEM_AGC_MODE_AUTO)
		return -EINVAL;

	interval = samples_per_channel /
		SPF_TANDEM_HOLD_OBSERVATIONS_PER_FRAME;
	if (interval < SPF_TANDEM_OBSERVATION_INTERVAL_MIN)
		interval = SPF_TANDEM_OBSERVATION_INTERVAL_MIN;
	/* AUTO retains the original high-resolution observation cadence for
	 * transient gain/RSSI evidence. HOLD cannot produce gain transitions, so
	 * four counter-bounded observations per frame preserve endpoint coverage
	 * without spending a core repeatedly proving the same held gain. */
	if (request->mode == ADI_TANDEM_AGC_MODE_AUTO &&
		interval > SPF_TANDEM_AUTO_OBSERVATION_INTERVAL_MAX)
		interval = SPF_TANDEM_AUTO_OBSERVATION_INTERVAL_MAX;
	*interval_samples = interval;
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

static int refresh_status_at_least(struct spf_tandem_session *session,
	uint32_t transition_count)
{
	unsigned int attempt;
	int ret;

	/*
	 * Event FIFO data and the coherent status snapshot cross independently
	 * into AXI.  A newly visible event can therefore precede the matching
	 * transition counter by one snapshot.  The public AXI status counter is
	 * only eight bits wide even though the kernel ABI zero-extends it into a
	 * u32.  Compare it in its native modulo-256 sequence space while keeping
	 * the software count cumulative for metadata consumers.
	 *
	 * At most 64 events can be outstanding (the hardware FIFO depth), so a
	 * modular delta in the forward half-range is unambiguously at or after
	 * the consumed event count.  Accept only bounded convergence; a
	 * persistent disagreement remains a fail-closed EILSEQ.
	 */
	for (attempt = 0; attempt < SPF_TANDEM_STATUS_CATCHUP_ATTEMPTS;
			++attempt) {
		uint32_t delta;

		ret = refresh_status(session);
		if (ret)
			return ret;
		delta = (session->status.transition_count - transition_count) &
			SPF_TANDEM_TRANSITION_COUNTER_MASK;
		if (delta < SPF_TANDEM_TRANSITION_COUNTER_HALF_RANGE)
			return 0;
	}
	/* The event itself is authoritative for the single in-flight status
	 * snapshot.  A larger disagreement can indicate lost FIFO data. */
	if (((session->status.transition_count - transition_count) &
			SPF_TANDEM_TRANSITION_COUNTER_MASK) ==
		SPF_TANDEM_TRANSITION_COUNTER_MASK)
		return 0;
	fprintf(stderr,
		"SPF tandem status did not catch up: expected=%" PRIu32
		" observed=%" PRIu32 " attempts=%u epoch=%" PRIu32 "\n",
		transition_count, session->status.transition_count,
		SPF_TANDEM_STATUS_CATCHUP_ATTEMPTS,
		session->status.ownership_epoch);
	return -EILSEQ;
}

int spf_tandem_session_heartbeat(struct spf_tandem_session *session)
{
	if (!session || !session->acquired)
		return -EINVAL;
	return refresh_status(session);
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
process_queue:
	consumed = 0;
	while (consumed < session->queue_count) {
		const struct adi_tandem_agc_event *event =
			&session->queue[consumed];
		if (event->sample_sequence >= end)
			break;
		if (session->sequence_valid &&
			event->event_sequence != session->expected_event_sequence) {
			fprintf(stderr,
				"SPF tandem event sequence gap: expected=%" PRIu32
				" observed=%" PRIu32 " sample=%" PRIu64 "\n",
				session->expected_event_sequence, event->event_sequence,
				(uint64_t)event->sample_sequence);
			return -EILSEQ;
		}
		if (session->sample_sequence_valid && event->sample_sequence <
			session->last_event_sample_sequence) {
			fprintf(stderr,
				"SPF tandem event sample regression: previous=%" PRIu64
				" observed=%" PRIu64 " event=%" PRIu32 "\n",
				session->last_event_sample_sequence,
				(uint64_t)event->sample_sequence, event->event_sequence);
			return -EILSEQ;
		}
		if (event->rx1_gain_index != event->rx2_gain_index ||
			event->rx1_gain_index > 0x7f || event->flags & 0xffc0 ||
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
	if (!session->queue_count) {
		ret = fill_queue(session);
		if (ret)
			return ret;
		if (session->queue_count)
			goto process_queue;
	}
	ret = refresh_status_at_least(session, session->frame_transition_count);
	if (ret)
		return ret;
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
