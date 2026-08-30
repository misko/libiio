/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-tandem-session.h"

#include <spf_radio_frame_v3.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct mock_device {
	struct adi_tandem_agc_event events[64];
	size_t event_count;
	uint32_t epoch;
	uint32_t faults;
	uint32_t overflow;
	uint32_t acquire_transitions;
	uint32_t transitions;
	uint32_t state;
	uint32_t fence;
	uint32_t lagged_transitions;
	uint32_t lagged_fence;
	unsigned int lagged_status_reads;
	unsigned int lagged_fence_reads;
	bool bad_fpga_abi;
	bool missing_fence_feature;
	unsigned int read_eagain_count;
	unsigned int wait_timeout_count;
	size_t read_batch;
	unsigned int read_count;
	unsigned int wait_count;
	int open_count;
	int close_count;
	int acquire_count;
	int start_auto_count;
	uint32_t last_acquire_mode;
	int status_count;
	int release_count;
};

static void put_le16(uint8_t *destination, uint16_t value)
{
	destination[0] = value;
	destination[1] = value >> 8;
}

static void put_le32(uint8_t *destination, uint32_t value)
{
	destination[0] = value;
	destination[1] = value >> 8;
	destination[2] = value >> 16;
	destination[3] = value >> 24;
}

static void valid_request(uint8_t request[104])
{
	memset(request, 0, 104);
	put_le32(request + 0, ADI_TANDEM_AGC_REQUEST_MAGIC);
	put_le16(request + 4, ADI_TANDEM_AGC_ABI_VERSION);
	put_le16(request + 6, 104);
	put_le32(request + 8, ADI_TANDEM_AGC_FEATURE_EVENTS |
		ADI_TANDEM_AGC_FEATURE_FAIL_CLOSED |
		ADI_TANDEM_AGC_FEATURE_PAIRED_GAIN);
	put_le32(request + 12, ADI_TANDEM_AGC_MODE_AUTO);
	put_le32(request + 16, 64);
	put_le32(request + 20, 16);
	put_le32(request + 24, (uint32_t)-20);
	put_le32(request + 28, 71);
	put_le32(request + 32, 20);
	put_le32(request + 36, 1024);
	put_le32(request + 40, 3);
	put_le32(request + 44, 2);
	put_le32(request + 48, 4);
	put_le32(request + 52, 4);
	put_le32(request + 56, 8);
	request[60] = 20;
	request[61] = 58;
	request[62] = 49;
	request[63] = 48;
	put_le32(request + 64, ADI_TANDEM_AGC_POLICY_FAIL_SESSION);
	put_le32(request + 68, ADI_TANDEM_AGC_POLICY_FAIL_SESSION);
}

static void authoritative_request(uint8_t request[104])
{
	valid_request(request);
}

static int mock_open(const char *path, int flags, void *opaque)
{
	struct mock_device *mock = opaque;
	assert(strcmp(path, SPF_TANDEM_DEVICE) == 0);
	assert((flags & (O_RDWR | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK));
	mock->open_count++;
	return 23;
}

static void fill_status(struct mock_device *mock,
	struct adi_tandem_agc_status *status)
{
	memset(status, 0, sizeof(*status));
	status->version = ADI_TANDEM_AGC_ABI_VERSION;
	status->size = sizeof(*status);
	status->state = mock->state ? mock->state :
		ADI_TANDEM_AGC_STATE_ARMED_AUTO;
	status->ownership_epoch = mock->epoch;
	status->fault_flags = mock->faults;
	status->overflow_count = mock->overflow;
	status->fifo_level = (uint32_t)mock->event_count;
	status->transition_count = mock->acquire_count ? mock->transitions :
		mock->acquire_transitions;
	status->sample_counter_fence_low = mock->fence ? mock->fence : 100000;
	status->minimum_gain_db = -20;
	status->maximum_gain_db = 71;
	status->initial_gain_db = 20;
	status->rx1_gain_index = 19;
	status->rx2_gain_index = 19;
}

static int mock_ioctl(int fd, unsigned long request, void *argument,
	void *opaque)
{
	struct mock_device *mock = opaque;
	assert(fd == 23);
	if (request == ADI_TANDEM_AGC_IOC_GET_CAPS) {
		struct adi_tandem_agc_caps *caps = argument;
		memset(caps, 0, sizeof(*caps));
		caps->version = ADI_TANDEM_AGC_ABI_VERSION;
		caps->size = sizeof(*caps);
		caps->features = ADI_TANDEM_AGC_FEATURE_EVENTS |
			ADI_TANDEM_AGC_FEATURE_FAIL_CLOSED |
			ADI_TANDEM_AGC_FEATURE_PAIRED_GAIN |
			ADI_TANDEM_AGC_FEATURE_SAMPLE_FENCE;
		if (mock->missing_fence_feature)
			caps->features &= ~ADI_TANDEM_AGC_FEATURE_SAMPLE_FENCE;
		caps->fpga_abi = mock->bad_fpga_abi ? 1 : 2;
		caps->event_size = sizeof(struct adi_tandem_agc_event);
		caps->fifo_depth = 64;
		return 0;
	}
	if (request == ADI_TANDEM_AGC_IOC_ACQUIRE) {
		struct adi_tandem_agc_acquire *acquire = argument;
		assert(acquire->request.magic == ADI_TANDEM_AGC_REQUEST_MAGIC);
		assert(acquire->request.minimum_gain_db == -20);
		mock->last_acquire_mode = acquire->request.mode;
		fill_status(mock, &acquire->status);
		acquire->status.state = acquire->request.mode ==
			ADI_TANDEM_AGC_MODE_HOLD ?
			ADI_TANDEM_AGC_STATE_ARMED_HOLD :
			ADI_TANDEM_AGC_STATE_ARMED_AUTO;
		mock->acquire_count++;
		return 0;
	}
	if (request == ADI_TANDEM_AGC_IOC_START_AUTO) {
		assert(argument == NULL);
		mock->state = ADI_TANDEM_AGC_STATE_ARMED_AUTO;
		mock->start_auto_count++;
		return 0;
	}
	if (request == ADI_TANDEM_AGC_IOC_GET_STATUS) {
		mock->status_count++;
		fill_status(mock, argument);
		if (mock->lagged_status_reads) {
			((struct adi_tandem_agc_status *)argument)->transition_count =
				mock->lagged_transitions;
			mock->lagged_status_reads--;
		}
		if (mock->lagged_fence_reads) {
			((struct adi_tandem_agc_status *)argument)->
				sample_counter_fence_low = mock->lagged_fence;
			mock->lagged_fence_reads--;
		}
		return 0;
	}
	if (request == ADI_TANDEM_AGC_IOC_RELEASE) {
		assert(argument == NULL);
		mock->release_count++;
		return 0;
	}
	assert(!"unexpected ioctl");
	return -1;
}

static ssize_t mock_read(int fd, void *destination, size_t bytes, void *opaque)
{
	struct mock_device *mock = opaque;
	size_t count;
	assert(fd == 23);
	mock->read_count++;
	if (mock->read_eagain_count) {
		mock->read_eagain_count--;
		errno = EAGAIN;
		return -1;
	}
	if (!mock->event_count) {
		errno = EAGAIN;
		return -1;
	}
	count = bytes / sizeof(mock->events[0]);
	if (count > mock->event_count)
		count = mock->event_count;
	if (mock->read_batch && count > mock->read_batch)
		count = mock->read_batch;
	memcpy(destination, mock->events, count * sizeof(mock->events[0]));
	memmove(mock->events, &mock->events[count],
		(mock->event_count - count) * sizeof(mock->events[0]));
	mock->event_count -= count;
	return (ssize_t)(count * sizeof(mock->events[0]));
}

static int mock_close(int fd, void *opaque)
{
	struct mock_device *mock = opaque;
	assert(fd == 23);
	mock->close_count++;
	return 0;
}

static int mock_wait_readable(int fd, int timeout_ms, void *opaque)
{
	struct mock_device *mock = opaque;
	assert(fd == 23);
	assert(timeout_ms > 0);
	mock->wait_count++;
	if (mock->wait_timeout_count) {
		mock->wait_timeout_count--;
		return 0;
	}
	return 1;
}

static struct spf_tandem_syscalls mock_syscalls(struct mock_device *mock)
{
	const struct spf_tandem_syscalls calls = {
		.open_device = mock_open,
		.ioctl_device = mock_ioctl,
		.read_device = mock_read,
		.wait_readable = mock_wait_readable,
		.close_device = mock_close,
		.opaque = mock,
	};
	return calls;
}

static void test_request_decoder(void)
{
	uint8_t wire[104];
	struct adi_tandem_agc_request_v1 decoded;
	uint32_t interval = 0;
	bool reached = false;
	valid_request(wire);
	assert(spf_tandem_request_decode(&decoded, wire, sizeof(wire)) == 0);
	assert(decoded.magic == ADI_TANDEM_AGC_REQUEST_MAGIC);
	assert(decoded.minimum_gain_db == -20);
	assert(decoded.event_capacity == 16);
	assert(spf_tandem_request_validate_event_window(&decoded, 65U * 1024U) ==
		-ENOSPC);
	decoded.cooldown_periods = 8;
	assert(spf_tandem_request_validate_event_window(&decoded, 65U * 1024U) ==
		0);
	decoded.power_measurement_samples = 1024;
	decoded.cooldown_periods = 3;
	for (unsigned int kernel_buffers = 1; kernel_buffers <= 6;
			kernel_buffers += kernel_buffers == 1 ? 1 : 2) {
		decoded.event_capacity = kernel_buffers + 1;
		assert(spf_tandem_request_validate_event_window_depth(&decoded,
			4096, kernel_buffers) == 0);
		decoded.event_capacity--;
		assert(spf_tandem_request_validate_event_window_depth(&decoded,
			4096, kernel_buffers) == -ENOSPC);
	}
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(1073741823),
		1) == 0);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(1073741824),
		1) == -ERANGE);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(715827882),
		2) == 0);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(715827883),
		2) == -ERANGE);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(429496729),
		4) == 0);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(429496730),
		4) == -ERANGE);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(306783378),
		6) == 0);
	assert(spf_tandem_validate_sample_fence_window(UINT32_C(306783379),
		6) == -ERANGE);
	assert(spf_tandem_validate_sample_fence_window(0, 1) == -EINVAL);
	assert(spf_tandem_validate_sample_fence_window(1, 0) == -EINVAL);
	assert(spf_tandem_sample_fence_at_or_after(5, 5, &reached) == 0 &&
		reached);
	assert(spf_tandem_sample_fence_at_or_after(3, UINT32_MAX - 5U,
		&reached) == 0 && reached);
	assert(spf_tandem_sample_fence_at_or_after(UINT32_MAX - 5U, 3,
		&reached) == 0 && !reached);
	assert(spf_tandem_sample_fence_at_or_after(UINT32_C(0x80000000), 0,
		&reached) == -ERANGE);
	assert(spf_tandem_sample_fence_at_or_after(0, 0, NULL) == -EINVAL);
	decoded.event_capacity = 16;
	decoded.mode = ADI_TANDEM_AGC_MODE_HOLD;
	assert(spf_tandem_request_validate_event_window(&decoded, UINT32_MAX) == 0);
	assert(spf_tandem_request_observation_interval(&decoded, 1000000,
		&interval) == 0);
	assert(interval == 1000000);
	assert(spf_tandem_request_observation_interval(&decoded, 2048,
		&interval) == 0);
	assert(interval == 2048);
	decoded.mode = ADI_TANDEM_AGC_MODE_AUTO;
	assert(spf_tandem_request_observation_interval(&decoded, 1000000,
		&interval) == 0);
	assert(interval == 1000000);
	assert(spf_tandem_request_observation_interval(&decoded, 16384,
		&interval) == 0);
	assert(interval == 16384);
	decoded.mode = UINT32_MAX;
	assert(spf_tandem_request_observation_interval(&decoded, 1000000,
		&interval) == -EINVAL);
	assert(spf_tandem_request_observation_interval(NULL, 1000000,
		&interval) == -EINVAL);
	assert(spf_tandem_request_observation_interval(&decoded, 0,
		&interval) == -EINVAL);
	assert(spf_tandem_request_observation_interval(&decoded, 1000000,
		NULL) == -EINVAL);
	assert(spf_tandem_request_validate_event_window(&decoded, 0) == -EINVAL);
	assert(spf_tandem_request_decode(&decoded, wire, sizeof(wire) - 1) ==
		-EINVAL);
	wire[103] = 1;
	assert(spf_tandem_request_decode(&decoded, wire, sizeof(wire)) == -EINVAL);
}

static void test_transactional_watermark_and_cdc_delay(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 16};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct spf_tandem_frame_preview first;
	struct spf_tandem_frame_preview repeated;
	spf_gain_event_v7_t output[4];

	authoritative_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(mock.last_acquire_mode == ADI_TANDEM_AGC_MODE_HOLD);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){105, 0, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){205, 1, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 2;
	mock.read_eagain_count = 1;
	mock.wait_timeout_count = 1;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		100, 100) == 0);
	assert(spf_tandem_session_preview(&session, 100, 100,
		output, 4, &first) == 0);
	assert(mock.read_count == 2);
	assert(mock.wait_count == 1);
	assert(first.event_count == 1);
	assert(first.timeline.consumed_event_count == 1);
	assert(first.timeline.gain_start.rx1_gain_index == 19);
	assert(first.timeline.gain_end.rx1_gain_index == 20);
	assert(first.timeline.transition_count_start == 0);
	assert(first.timeline.transition_count_end == 1);
	assert(first.event_sequence_start_valid && first.event_sequence_start == 0);
	assert(session.timeline_state.transition_count == 0);
	assert(session.queue_count == 2);
	/* Serialization is deliberately outside the ledger transaction. A failed
	 * builder call leaves the preview retryable and the committed state intact. */
	uint8_t invalid_metadata[16] = {0};
	const spf_radio_frame_v7_args_t invalid_args = {0};
	assert(!spf_radio_frame_v7_base_build(invalid_metadata,
		sizeof(invalid_metadata), &invalid_args));
	assert(session.timeline_state.transition_count == 0);
	assert(session.queue_count == 2);
	mock.events[0] = (struct adi_tandem_agc_event){206, 2, 0x13, 22, 22};
	mock.event_count = 1;
	mock.transitions = 3;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		100, 100) == 0);
	assert(spf_tandem_session_commit(&session, &first) == -EINVAL);
	assert(spf_tandem_session_preview(&session, 100, 100,
		output, 4, &repeated) == 0);
	assert(repeated.generation == first.generation);
	assert(repeated.event_count == first.event_count);
	assert(repeated.transition_watermark == 3);
	assert(session.timeline_state.transition_count == 0);
	assert(spf_tandem_session_commit(&session, &repeated) == 0);
	assert(session.timeline_state.transition_count == 1);
	assert(session.queue_count == 2);
	assert(spf_tandem_session_commit(&session, &repeated) == -EINVAL);

	/* The future event already drained at the first fixed watermark remains
	 * the first event of the next frame; a fresh status snapshot accounts for
	 * it relative to the newly committed state. */
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		200, 100) == 0);
	assert(spf_tandem_session_preview(&session, 200, 100,
		output, 4, &first) == 0);
	assert(first.event_count == 2 && output[0].sample_sequence == 205 &&
		output[1].sample_sequence == 206);
	assert(first.timeline.gain_start.rx1_gain_index == 20);
	assert(first.timeline.gain_end.rx1_gain_index == 22);
	assert(spf_tandem_session_commit(&session, &first) == 0);
	assert(session.queue_count == 0);
	spf_tandem_session_close(&session);
}

static void test_fixed_watermark_wrap_and_retry_exhaustion(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 17};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct spf_tandem_frame_preview preview;
	spf_gain_event_v7_t output[4];

	authoritative_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	session.timeline_state.transition_count = 254;
	session.timeline_state.next_event_sequence = UINT32_MAX;
	mock.events[0] = (struct adi_tandem_agc_event){10, UINT32_MAX,
		0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){11, 0, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 0;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == 0);
	assert(session.pending_transition_watermark == 256);
	assert(spf_tandem_session_preview(&session, 0, 100,
		output, 4, &preview) == 0);
	assert(preview.event_count == 2);
	assert(preview.next_state.transition_count == 256);
	assert(spf_tandem_session_commit(&session, &preview) == 0);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 23;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 0, 0x13, 20, 20};
	mock.event_count = 1;
	mock.transitions = 1;
	mock.lagged_transitions = 0;
	mock.lagged_status_reads = 1;
	mock.lagged_fence = 99;
	mock.lagged_fence_reads = 1;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == 0);
	assert(mock.status_count == 3);
	assert(spf_tandem_session_preview(&session, 0, 100,
		output, 4, &preview) == 0);
	assert(preview.event_count == 1);
	assert(spf_tandem_session_commit(&session, &preview) == 0);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 18;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.transitions = 1;
	mock.read_eagain_count = 1;
	mock.wait_timeout_count = 1;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == 0);
	assert(spf_tandem_session_preview(&session, 0, 100,
		output, 4, &preview) == -EILSEQ);
	assert(session.timeline_state.transition_count == 0);
	assert(session.timeline_state.event_sequence_valid);
	assert(session.timeline_state.next_event_sequence == 0);
	spf_tandem_session_close(&session);
}

static void test_authoritative_fence_contract(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 24};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct spf_tandem_frame_preview preview;
	spf_gain_event_v7_t output[1];
	const uint64_t wrap_start = (uint64_t)UINT32_MAX - 50U;

	/* V4 keeps the inner ABI1 request byte-for-byte legacy. The outer V4
	 * envelope opts into authoritative semantics; caps prove fence support. */
	valid_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(mock.last_acquire_mode == ADI_TANDEM_AGC_MODE_HOLD);
	spf_tandem_session_close(&session);

	put_le32(wire + 8, ADI_TANDEM_AGC_FEATURE_EVENTS |
		ADI_TANDEM_AGC_FEATURE_FAIL_CLOSED |
		ADI_TANDEM_AGC_FEATURE_PAIRED_GAIN |
		ADI_TANDEM_AGC_FEATURE_SAMPLE_FENCE);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) ==
		-EINVAL);
	authoritative_request(wire);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 25;
	mock.bad_fpga_abi = true;
	calls = mock_syscalls(&mock);
	authoritative_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == -EPROTONOSUPPORT);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 26;
	mock.missing_fence_feature = true;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == -EPROTONOSUPPORT);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 30;
	mock.acquire_transitions = 1;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == -EPROTO);
	assert(mock.release_count == 0 && mock.close_count == 1);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 31;
	mock.overflow = 1;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == -EPROTO);
	assert(mock.release_count == 0 && mock.close_count == 1);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 27;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(mock.last_acquire_mode == ADI_TANDEM_AGC_MODE_HOLD);
	assert(spf_tandem_session_snapshot_frame_watermark(&session, 0, 100) ==
		-EINVAL);
	assert(spf_tandem_session_start_auto(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == -EINVAL);
	mock.fence = 60;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		wrap_start, 100) == 0);
	assert(spf_tandem_session_preview(&session, wrap_start, 100,
		output, 1, &preview) == 0);
	assert(preview.event_count == 0 && preview.event_sequence_start == 0);
	assert(spf_tandem_session_commit(&session, &preview) == 0);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 28;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.fence = UINT32_C(0x80000064);
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == -ERANGE);
	assert(!session.pending_watermark_valid);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 29;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.fence = 99;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == -ETIMEDOUT);
	assert(!session.pending_watermark_valid);
	spf_tandem_session_close(&session);
}

static void test_frame_boundaries_hold_and_sequence_gap(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 19};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct spf_tandem_frame_preview preview;
	spf_gain_event_v7_t output[4];

	authoritative_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	session.timeline_state.next_event_sequence = UINT32_MAX;
	mock.events[0] = (struct adi_tandem_agc_event){100, UINT32_MAX,
		0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){100, 0,
		0x13, 21, 21};
	mock.events[2] = (struct adi_tandem_agc_event){200, 1,
		0x13, 22, 22};
	mock.event_count = 3;
	mock.transitions = 3;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		100, 100) == 0);
	assert(spf_tandem_session_preview(&session, 100, 100,
		output, 4, &preview) == 0);
	assert(preview.event_count == 2);
	assert(output[0].event_sequence == UINT32_MAX);
	assert(output[1].event_sequence == 0);
	assert(preview.timeline.gain_start.rx1_gain_index == 21);
	assert(preview.timeline.gain_end.rx1_gain_index == 21);
	assert(preview.timeline.rx1_first_change_sample == 0);
	assert(preview.timeline.consumed_event_count == 2);
	assert(spf_tandem_session_commit(&session, &preview) == 0);
	assert(session.queue_count == 1);
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		200, 100) == 0);
	assert(spf_tandem_session_preview(&session, 200, 100,
		output, 4, &preview) == 0);
	assert(preview.event_count == 1 && output[0].sample_sequence == 200);
	assert(preview.timeline.gain_start.rx1_gain_index == 22);
	assert(spf_tandem_session_commit(&session, &preview) == 0);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 20;
	mock.state = ADI_TANDEM_AGC_STATE_ARMED_HOLD;
	calls = mock_syscalls(&mock);
	put_le32(wire + 12, ADI_TANDEM_AGC_MODE_HOLD);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	assert(mock.start_auto_count == 0);
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == 0);
	assert(spf_tandem_session_preview(&session, 0, 100,
		output, 4, &preview) == 0);
	assert(preview.event_count == 0);
	assert(preview.event_sequence_start_valid);
	assert(preview.event_sequence_start == 0);
	assert(preview.timeline.gain_start.rx1_gain_index == 19);
	assert(preview.timeline.gain_end.rx1_gain_index == 19);
	assert(spf_tandem_session_commit(&session, &preview) == 0);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 21;
	calls = mock_syscalls(&mock);
	authoritative_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 0, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){11, 2, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 2;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == 0);
	assert(spf_tandem_session_preview(&session, 0, 100,
		output, 4, &preview) == -EILSEQ);
	assert(session.timeline_state.transition_count == 0);
	assert(session.queue_count == 2);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 22;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_enable_authoritative_timeline(&session) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_start_auto(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 1, 0x13, 20, 20};
	mock.event_count = 1;
	mock.transitions = 1;
	assert(spf_tandem_session_snapshot_frame_watermark(&session,
		0, 100) == 0);
	assert(spf_tandem_session_preview(&session, 0, 100,
		output, 4, &preview) == -EILSEQ);
	assert(session.timeline_state.next_event_sequence == 0);
	spf_tandem_session_close(&session);
}

static void test_lifecycle_and_partition(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 7};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct adi_tandem_agc_event output[4];
	size_t count = 99;
	valid_request(wire);
	mock.events[0] = (struct adi_tandem_agc_event){90, 10, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){105, 11, 0x13, 21, 21};
	mock.events[2] = (struct adi_tandem_agc_event){199, 12, 0x13, 22, 22};
	mock.events[3] = (struct adi_tandem_agc_event){205, 13, 0x13, 23, 23};
	mock.event_count = 4;
	mock.transitions = 4;
	mock.read_batch = 2;
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(mock.open_count == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(mock.last_acquire_mode == ADI_TANDEM_AGC_MODE_AUTO);
	assert(spf_tandem_session_start_auto(&session) == -EINVAL);
	assert(mock.open_count == 1 && mock.acquire_count == 1);
	assert(spf_tandem_session_heartbeat(&session) == 0);
	assert(mock.status_count == 1);
	assert(spf_tandem_session_collect(&session, 100, 100,
		output, 4, &count) == 0);
	assert(count == 2);
	assert(output[0].sample_sequence == 105);
	assert(output[1].sample_sequence == 199);
	assert(session.queue_count == 1);
	assert(spf_tandem_session_collect(&session, 200, 100,
		output, 4, &count) == 0);
	assert(count == 1 && output[0].event_sequence == 13);
	spf_tandem_session_close(&session);
	spf_tandem_session_close(&session);
	assert(mock.release_count == 1 && mock.close_count == 1);
	assert(spf_tandem_session_heartbeat(&session) == -EINVAL);
}

static void test_sequence_and_status_faults_fail_closed(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 9};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct adi_tandem_agc_event output[4];
	size_t count;
	valid_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 3, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){11, 5, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 2;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == -EILSEQ);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 10;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.faults = 1;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == -EIO);
	spf_tandem_session_close(&session);
}

static void test_status_snapshot_catches_up_to_event_fifo(void)
{
	uint8_t wire[104];
	struct mock_device mock = {.epoch = 11};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct adi_tandem_agc_event output[4];
	size_t count;

	valid_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 3, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){11, 4, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 2;
	mock.lagged_transitions = 1;
	mock.lagged_status_reads = 1;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == 0);
	assert(count == 2 && mock.status_count == 2);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 12;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 3, 0x13, 20, 20};
	mock.event_count = 1;
	mock.transitions = 1;
	mock.lagged_status_reads = 5;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == 0);
	assert(count == 1 && mock.status_count == 4);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 13;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 3, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){11, 4, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 2;
	mock.lagged_status_reads = 5;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == -EILSEQ);
	assert(mock.status_count == 4);
	spf_tandem_session_close(&session);
}

static void test_status_counter_wraps_without_losing_continuity(void)
{
	uint8_t wire[104];
	struct mock_device mock = {
		.epoch = 14,
		.acquire_transitions = 254,
	};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	struct spf_tandem_session session;
	struct adi_tandem_agc_event output[4];
	size_t count;

	valid_request(wire);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 255, 0x13, 20, 20};
	mock.events[1] = (struct adi_tandem_agc_event){11, 256, 0x13, 21, 21};
	mock.event_count = 2;
	mock.transitions = 0;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == 0);
	assert(count == 2);
	assert(session.status.transition_count == 256);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 15;
	mock.acquire_transitions = 255;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	mock.events[0] = (struct adi_tandem_agc_event){10, 256, 0x13, 20, 20};
	mock.event_count = 1;
	mock.transitions = 255;
	mock.lagged_transitions = 255;
	mock.lagged_status_reads = 5;
	assert(spf_tandem_session_collect(&session, 0, 100,
		output, 4, &count) == 0);
	assert(count == 1 && mock.status_count == 4);
	assert(session.status.transition_count == 256);
	spf_tandem_session_close(&session);
}

int main(void)
{
	test_request_decoder();
	test_lifecycle_and_partition();
	test_sequence_and_status_faults_fail_closed();
	test_status_snapshot_catches_up_to_event_fifo();
	test_status_counter_wraps_without_losing_continuity();
	test_transactional_watermark_and_cdc_delay();
	test_fixed_watermark_wrap_and_retry_exhaustion();
	test_authoritative_fence_contract();
	test_frame_boundaries_hold_and_sequence_gap();
	puts("SPF tandem session tests passed");
	return 0;
}
