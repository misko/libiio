/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L

#include "spf-tandem-session.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SPF_TANDEM_STATUS_CATCHUP_ATTEMPTS 4
#define SPF_TANDEM_EVENT_DRAIN_TIMEOUT_MS 20
#define SPF_TANDEM_FENCE_TIMEOUT_MS 20
#define SPF_TANDEM_FENCE_RETRY_NS 1000000L
#define SPF_TANDEM_AUTHORITATIVE_FPGA_ABI 2U
#define SPF_TANDEM_TRANSITION_COUNTER_BITS 8U
#define SPF_TANDEM_TRANSITION_COUNTER_MASK \
	((1U << SPF_TANDEM_TRANSITION_COUNTER_BITS) - 1U)
#define SPF_TANDEM_TRANSITION_COUNTER_HALF_RANGE \
	(1U << (SPF_TANDEM_TRANSITION_COUNTER_BITS - 1U))
#define SPF_TANDEM_REFILL_OBSERVATIONS_PER_FRAME 1U
#define SPF_TANDEM_OBSERVATION_INTERVAL_MIN 1024U

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

static int system_wait_readable(int fd, int timeout_ms, void *opaque)
{
	struct pollfd descriptor = {
		.fd = fd,
		.events = POLLIN,
	};

	(void)opaque;
	int ret = poll(&descriptor, 1, timeout_ms);
	if (ret > 0 && !(descriptor.revents & POLLIN)) {
		errno = EIO;
		return -1;
	}
	return ret;
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
	if (destination->required_features !=
			(ADI_TANDEM_AGC_FEATURE_EVENTS |
			 ADI_TANDEM_AGC_FEATURE_FAIL_CLOSED |
			 ADI_TANDEM_AGC_FEATURE_PAIRED_GAIN))
		return -EINVAL;
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

int spf_tandem_request_validate_event_window_depth(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel, unsigned int kernel_buffers_count)
{
	uint64_t minimum_transition_samples;
	uint64_t retention_frames;
	uint64_t retention_samples;
	uint64_t maximum_retained_events;

	if (!request || !samples_per_channel || !kernel_buffers_count ||
		!request->power_measurement_samples || !request->event_capacity)
		return -EINVAL;
	if (request->mode == ADI_TANDEM_AGC_MODE_HOLD)
		return 0;
	if (request->mode != ADI_TANDEM_AGC_MODE_AUTO)
		return -EINVAL;
	minimum_transition_samples =
		(uint64_t)request->power_measurement_samples *
		((uint64_t)request->cooldown_periods + 1U);
	retention_frames = (uint64_t)kernel_buffers_count + 1U;
	if (retention_frames > UINT64_MAX / samples_per_channel)
		return -ENOSPC;
	retention_samples = (uint64_t)samples_per_channel * retention_frames;
	maximum_retained_events = 1U +
		(retention_samples - 1U) / minimum_transition_samples;
	return maximum_retained_events <= request->event_capacity ? 0 : -ENOSPC;
}

int spf_tandem_validate_sample_fence_window(uint32_t samples_per_channel,
	unsigned int kernel_buffers_count)
{
	const uint64_t retention_frames =
		(uint64_t)kernel_buffers_count + UINT64_C(1);
	const uint64_t half_range = UINT64_C(1) << 31;

	if (!samples_per_channel || !kernel_buffers_count)
		return -EINVAL;
	if ((uint64_t)samples_per_channel >
		(UINT64_MAX / retention_frames))
		return -EOVERFLOW;
	return (uint64_t)samples_per_channel * retention_frames < half_range ?
		0 : -ERANGE;
}

int spf_tandem_sample_fence_at_or_after(uint32_t observed_fence,
	uint32_t target_fence, bool *at_or_after)
{
	const uint32_t delta = observed_fence - target_fence;
	const uint32_t half_range = UINT32_C(1) << 31;

	if (!at_or_after)
		return -EINVAL;
	if (delta == half_range)
		return -ERANGE;
	*at_or_after = delta < half_range;
	return 0;
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
		SPF_TANDEM_REFILL_OBSERVATIONS_PER_FRAME;
	if (interval < SPF_TANDEM_OBSERVATION_INTERVAL_MIN)
		interval = SPF_TANDEM_OBSERVATION_INTERVAL_MIN;
	/* Every refill after the first is fenced by one real gain/RSSI observation
	 * whose synchronized counter interval brackets the DMA refill. The first
	 * refill is guarded by the sampler's bounded startup-discard contract.
	 * HOLD cannot transition. AUTO's authoritative FPGA event records retain
	 * every intra-frame gain transition and exact sample sequence, so polling
	 * the SPI-backed AD936x state between refill fences adds CPU load without
	 * adding transition evidence. Keep the observation interval honest at one
	 * refill; do not manufacture denser observations than the sampler makes. */
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
		session->syscalls.wait_readable = system_wait_readable;
		session->syscalls.close_device = system_close;
	}
	if (!session->syscalls.open_device || !session->syscalls.ioctl_device ||
		!session->syscalls.read_device || !session->syscalls.wait_readable ||
		!session->syscalls.close_device)
		return -EINVAL;
	return 0;
}

int spf_tandem_session_enable_authoritative_timeline(
	struct spf_tandem_session *session)
{
	if (!session || session->fd >= 0 || session->acquired)
		return -EINVAL;
	session->authoritative_timeline = true;
	return 0;
}

static int read_status(struct spf_tandem_session *session,
	struct adi_tandem_agc_status *result)
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
	if (status.overflow_count != session->initial_overflow_count)
		return -EOVERFLOW;
	if (status.fault_flags)
		return -EIO;
	if ((session->request.mode == ADI_TANDEM_AGC_MODE_HOLD &&
		 status.state != ADI_TANDEM_AGC_STATE_ARMED_HOLD) ||
		(session->request.mode == ADI_TANDEM_AGC_MODE_AUTO &&
		 status.state != ADI_TANDEM_AGC_STATE_ARMED_AUTO))
		return -EPROTO;
	*result = status;
	return 0;
}

static int refresh_status(struct spf_tandem_session *session)
{
	struct adi_tandem_agc_status status;
	int ret = read_status(session, &status);

	if (!ret)
		session->status = status;
	return ret;
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
		(session->authoritative_timeline &&
		 (!(caps.features & ADI_TANDEM_AGC_FEATURE_SAMPLE_FENCE) ||
		  caps.fpga_abi != SPF_TANDEM_AUTHORITATIVE_FPGA_ABI)) ||
		caps.event_size != sizeof(struct adi_tandem_agc_event) ||
		session->request.event_capacity > caps.fifo_depth) {
		ret = -EPROTONOSUPPORT;
		goto fail;
	}
	memset(&acquire, 0, sizeof(acquire));
	acquire.request = session->request;
	if (session->authoritative_timeline &&
		session->request.mode == ADI_TANDEM_AGC_MODE_AUTO)
		acquire.request.mode = ADI_TANDEM_AGC_MODE_HOLD;
	if (session->syscalls.ioctl_device(session->fd,
			ADI_TANDEM_AGC_IOC_ACQUIRE, &acquire,
			session->syscalls.opaque) < 0) {
		ret = -errno;
		goto fail;
	}
	if (acquire.status.version != ADI_TANDEM_AGC_ABI_VERSION ||
		acquire.status.size != sizeof(acquire.status) ||
		!acquire.status.ownership_epoch || acquire.status.fault_flags ||
		acquire.status.rx1_gain_index != acquire.status.rx2_gain_index ||
		acquire.status.rx1_gain_index > UINT8_C(0x7f) ||
		(session->authoritative_timeline &&
			(acquire.status.transition_count != 0 ||
			 acquire.status.overflow_count != 0)) ||
		((session->request.mode == ADI_TANDEM_AGC_MODE_HOLD ||
		  session->authoritative_timeline) &&
			acquire.status.state != ADI_TANDEM_AGC_STATE_ARMED_HOLD) ||
		(!session->authoritative_timeline &&
		 session->request.mode == ADI_TANDEM_AGC_MODE_AUTO &&
			acquire.status.state != ADI_TANDEM_AGC_STATE_ARMED_AUTO)) {
		ret = -EPROTO;
		goto fail;
	}
	session->status = acquire.status;
	session->initial_overflow_count = acquire.status.overflow_count;
	session->queue_count = 0;
	session->expected_event_sequence = 0;
	session->last_event_sample_sequence = 0;
	session->sequence_valid = false;
	session->sample_sequence_valid = false;
	session->pending_watermark_valid = false;
	session->auto_started = false;
	session->generation++;
	session->frame_transition_count = acquire.status.transition_count;
	session->fifo_depth = caps.fifo_depth;
	session->frame_rx1_gain_index = acquire.status.rx1_gain_index;
	session->frame_rx2_gain_index = acquire.status.rx2_gain_index;
	session->timeline_state = (spf_gain_timeline_state_t){
		.gain = {
			.rx1_gain_index = acquire.status.rx1_gain_index,
			.rx2_gain_index = acquire.status.rx2_gain_index,
		},
		.transition_count = acquire.status.transition_count,
		.next_event_sequence = 0,
		.event_sequence_valid = session->authoritative_timeline,
	};
	session->acquired = true;
	return 0;

fail:
	(void)session->syscalls.close_device(session->fd,
		session->syscalls.opaque);
	session->fd = -1;
	return ret;
}

int spf_tandem_session_start_auto(struct spf_tandem_session *session)
{
	struct adi_tandem_agc_status status;
	int ret;

	if (!session || !session->acquired || !session->authoritative_timeline)
		return -EINVAL;
	if (session->request.mode == ADI_TANDEM_AGC_MODE_HOLD)
		return session->auto_started ? -EINVAL : 0;
	if (session->request.mode != ADI_TANDEM_AGC_MODE_AUTO ||
		session->auto_started)
		return -EINVAL;
	if (session->syscalls.ioctl_device(session->fd,
			ADI_TANDEM_AGC_IOC_START_AUTO, NULL,
			session->syscalls.opaque) < 0)
		return -errno;
	ret = read_status(session, &status);
	if (ret)
		return ret;
	if (status.state != ADI_TANDEM_AGC_STATE_ARMED_AUTO)
		return -EPROTO;
	session->auto_started = true;
	return 0;
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

static spf_gain_event_v7_t tandem_event_to_v7(
	const struct adi_tandem_agc_event *event)
{
	return (spf_gain_event_v7_t){
		.sample_sequence = event->sample_sequence,
		.event_sequence = event->event_sequence,
		.flags = event->flags,
		.rx1_gain_index = event->rx1_gain_index,
		.rx2_gain_index = event->rx2_gain_index,
	};
}

static int timeline_result_error(spf_gain_timeline_result_t result)
{
	switch (result) {
	case SPF_GAIN_TIMELINE_OK:
		return 0;
	case SPF_GAIN_TIMELINE_RANGE_ERROR:
		return -ERANGE;
	case SPF_GAIN_TIMELINE_EVENT_SEQUENCE_GAP:
	case SPF_GAIN_TIMELINE_SAMPLE_REGRESSION:
		return -EILSEQ;
	case SPF_GAIN_TIMELINE_INVALID_GAIN_PAIR:
	case SPF_GAIN_TIMELINE_UNKNOWN_EVENT_FLAGS:
		return -EPROTO;
	case SPF_GAIN_TIMELINE_INVALID_ARGUMENT:
	default:
		return -EINVAL;
	}
}

static int validate_queued_events(const struct spf_tandem_session *session)
{
	spf_gain_timeline_state_t state = session->timeline_state;
	size_t index;

	for (index = 0; index < session->queue_count; ++index) {
		const spf_gain_event_v7_t event =
			tandem_event_to_v7(&session->queue[index]);

		if (!spf_gain_event_v7_flags_valid(event.flags) ||
			!spf_gain_event_v7_pair_valid(&event))
			return -EPROTO;
		if (state.event_sequence_valid &&
			event.event_sequence != state.next_event_sequence)
			return -EILSEQ;
		if (state.sample_sequence_valid &&
			event.sample_sequence < state.last_event_sample_sequence)
			return -EILSEQ;
		if (state.transition_count == UINT64_MAX)
			return -ERANGE;
		state.gain.rx1_gain_index = event.rx1_gain_index;
		state.gain.rx2_gain_index = event.rx2_gain_index;
		state.next_event_sequence = event.event_sequence + 1U;
		state.event_sequence_valid = true;
		state.last_event_sample_sequence = event.sample_sequence;
		state.sample_sequence_valid = true;
		state.transition_count++;
	}
	return 0;
}

static int monotonic_milliseconds(uint64_t *milliseconds)
{
	struct timespec now = {0, 0};

	if (!milliseconds)
		return -EINVAL;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -errno;
	*milliseconds = (uint64_t)now.tv_sec * UINT64_C(1000) +
		(uint64_t)now.tv_nsec / UINT64_C(1000000);
	return 0;
}

int spf_tandem_session_snapshot_frame_watermark(
	struct spf_tandem_session *session, uint64_t first_sample_sequence,
	uint32_t samples_per_channel)
{
	struct adi_tandem_agc_status status;
	const struct timespec retry = {.tv_nsec = SPF_TANDEM_FENCE_RETRY_NS};
	uint64_t frame_end;
	uint64_t deadline;
	uint64_t now = 0;
	uint32_t committed_low;
	uint32_t delta;
	uint64_t watermark;
	bool fence_reached;
	int ret;

	if (!session || !session->acquired || !session->authoritative_timeline ||
		!samples_per_channel ||
		(session->request.mode == ADI_TANDEM_AGC_MODE_AUTO &&
		 !session->auto_started))
		return -EINVAL;
	frame_end = first_sample_sequence + samples_per_channel;
	if (frame_end < first_sample_sequence)
		return -ERANGE;
	ret = monotonic_milliseconds(&now);
	if (ret)
		return ret;
	if (now > UINT64_MAX - SPF_TANDEM_FENCE_TIMEOUT_MS)
		return -ERANGE;
	deadline = now + SPF_TANDEM_FENCE_TIMEOUT_MS;
	for (;;) {
		ret = read_status(session, &status);
		if (ret)
			return ret;
		ret = spf_tandem_sample_fence_at_or_after(
			status.sample_counter_fence_low, (uint32_t)frame_end,
			&fence_reached);
		if (ret)
			return ret;
		if (fence_reached)
			break;
		ret = monotonic_milliseconds(&now);
		if (ret)
			return ret;
		if (now >= deadline)
			return -ETIMEDOUT;
		(void)nanosleep(&retry, NULL);
	}
	committed_low = (uint32_t)session->timeline_state.transition_count &
		SPF_TANDEM_TRANSITION_COUNTER_MASK;
	delta = (status.transition_count - committed_low) &
		SPF_TANDEM_TRANSITION_COUNTER_MASK;
	if (delta >= SPF_TANDEM_TRANSITION_COUNTER_HALF_RANGE ||
		delta > session->fifo_depth ||
		delta > SPF_TANDEM_EVENT_QUEUE_CAPACITY ||
		session->queue_count > delta)
		return -EILSEQ;
	if (session->timeline_state.transition_count > UINT64_MAX - delta)
		return -ERANGE;
	watermark = session->timeline_state.transition_count + delta;
	if (session->pending_watermark_valid &&
		watermark < session->pending_transition_watermark)
		return -EILSEQ;
	session->pending_transition_watermark = watermark;
	session->pending_status = status;
	session->pending_watermark_valid = true;
	return 0;
}

static int drain_to_watermark(struct spf_tandem_session *session)
{
	size_t required;
	bool timeout_recheck = false;

	if (!session->pending_watermark_valid ||
		session->pending_transition_watermark <
			session->timeline_state.transition_count)
		return -EINVAL;
	required = (size_t)(session->pending_transition_watermark -
		session->timeline_state.transition_count);
	if (required > session->fifo_depth ||
		required > SPF_TANDEM_EVENT_QUEUE_CAPACITY ||
		session->queue_count > required)
		return -EILSEQ;
	while (session->queue_count < required) {
		const size_t missing = required - session->queue_count;
		ssize_t bytes = session->syscalls.read_device(session->fd,
			&session->queue[session->queue_count],
			missing * sizeof(session->queue[0]),
			session->syscalls.opaque);

		if (bytes < 0) {
			int ready;

			if (errno != EAGAIN && errno != EWOULDBLOCK)
				return -errno;
			if (timeout_recheck)
				return -EILSEQ;
			ready = session->syscalls.wait_readable(session->fd,
				SPF_TANDEM_EVENT_DRAIN_TIMEOUT_MS,
				session->syscalls.opaque);
			if (ready < 0)
				return -errno;
			/* The current tandem driver poll callback does not register a
			 * waitqueue, so timeout is a bounded delay, not proof that the
			 * independently synchronized FIFO is still empty. Always perform
			 * one final read at the deadline before failing closed. */
			timeout_recheck = !ready;
			continue;
		}
		if (!bytes)
			return -EPIPE;
		if ((size_t)bytes % sizeof(session->queue[0]) ||
			(size_t)bytes > missing * sizeof(session->queue[0]))
			return -EPROTO;
		session->queue_count +=
			(size_t)bytes / sizeof(session->queue[0]);
		timeout_recheck = false;
	}
	return validate_queued_events(session);
}

int spf_tandem_session_preview(struct spf_tandem_session *session,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	spf_gain_event_v7_t *events, size_t event_capacity,
	struct spf_tandem_frame_preview *preview)
{
	spf_gain_event_v7_t queued[SPF_TANDEM_EVENT_QUEUE_CAPACITY];
	spf_gain_timeline_frame_t frame;
	spf_gain_timeline_state_t next_state;
	spf_gain_timeline_result_t result;
	size_t index;
	int ret;

	if (!session || !session->acquired || !session->authoritative_timeline ||
		!samples_per_channel ||
		(!events && event_capacity) || !preview ||
		!session->pending_watermark_valid)
		return -EINVAL;
	ret = drain_to_watermark(session);
	if (ret)
		return ret;
	for (index = 0; index < session->queue_count; ++index)
		queued[index] = tandem_event_to_v7(&session->queue[index]);
	result = spf_gain_timeline_resolve(&session->timeline_state,
		first_sample_sequence, samples_per_channel, queued,
		session->queue_count, &frame, &next_state);
	ret = timeline_result_error(result);
	if (ret)
		return ret;
	if (frame.frame_event_count > event_capacity)
		return -ENOSPC;
	if (frame.frame_event_count)
		memcpy(events, &queued[frame.frame_event_offset],
			frame.frame_event_count * sizeof(events[0]));
	memset(preview, 0, sizeof(*preview));
	preview->status = session->pending_status;
	preview->status.transition_count =
		(uint32_t)frame.transition_count_end;
	preview->status.rx1_gain_index = frame.gain_end.rx1_gain_index;
	preview->status.rx2_gain_index = frame.gain_end.rx2_gain_index;
	preview->timeline = frame;
	preview->next_state = next_state;
	preview->generation = session->generation;
	preview->transition_watermark =
		session->pending_transition_watermark;
	preview->first_sample_sequence = first_sample_sequence;
	preview->samples_per_channel = samples_per_channel;
	preview->event_count = frame.frame_event_count;
	if (frame.frame_event_count) {
		preview->event_sequence_start =
			queued[frame.frame_event_offset].event_sequence;
		preview->event_sequence_start_valid = true;
	} else if (next_state.event_sequence_valid) {
		preview->event_sequence_start = next_state.next_event_sequence;
		preview->event_sequence_start_valid = true;
	}
	return 0;
}

int spf_tandem_session_commit(struct spf_tandem_session *session,
	const struct spf_tandem_frame_preview *preview)
{
	size_t consumed;

	if (!session || !preview || !session->acquired ||
		!session->pending_watermark_valid ||
		preview->generation != session->generation ||
		preview->transition_watermark !=
			session->pending_transition_watermark ||
		preview->timeline.consumed_event_count > session->queue_count ||
		preview->next_state.transition_count >
			session->pending_transition_watermark)
		return -EINVAL;
	consumed = preview->timeline.consumed_event_count;
	if (consumed) {
		memmove(session->queue, &session->queue[consumed],
			(session->queue_count - consumed) * sizeof(session->queue[0]));
		session->queue_count -= consumed;
	}
	session->timeline_state = preview->next_state;
	session->status = preview->status;
	session->frame_transition_count = preview->status.transition_count;
	session->frame_rx1_gain_index = preview->status.rx1_gain_index;
	session->frame_rx2_gain_index = preview->status.rx2_gain_index;
	session->expected_event_sequence =
		preview->next_state.next_event_sequence;
	session->sequence_valid = preview->next_state.event_sequence_valid;
	session->last_event_sample_sequence =
		preview->next_state.last_event_sample_sequence;
	session->sample_sequence_valid =
		preview->next_state.sample_sequence_valid;
	session->pending_watermark_valid = false;
	session->generation++;
	return 0;
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
	session->pending_watermark_valid = false;
	session->auto_started = false;
}
