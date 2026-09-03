/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "spf-tandem-session.h"

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
	struct adi_tandem_agc_event events[8];
	size_t event_count;
	uint32_t epoch;
	uint32_t faults;
	uint32_t overflow;
	uint32_t acquire_transitions;
	uint32_t transitions;
	uint32_t lagged_transitions;
	unsigned int lagged_status_reads;
	size_t read_batch;
	int open_count;
	int close_count;
	int acquire_count;
	int status_count;
	int release_count;
#ifdef IIOD_HAS_KERNEL_PERSISTENT_HOP
	uint64_t hop_counter;
	uint64_t hop_event_id;
	int hop_start_count;
	int hop_recall_count;
	int hop_restore_count;
	int hop_caps_unsupported;
#endif
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
	status->state = ADI_TANDEM_AGC_STATE_ARMED_AUTO;
	status->ownership_epoch = mock->epoch;
	status->fault_flags = mock->faults;
	status->overflow_count = mock->overflow;
	status->transition_count = mock->acquire_count ? mock->transitions :
		mock->acquire_transitions;
	status->minimum_gain_db = -20;
	status->maximum_gain_db = 71;
	status->initial_gain_db = 20;
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
			ADI_TANDEM_AGC_FEATURE_PAIRED_GAIN;
		caps->event_size = sizeof(struct adi_tandem_agc_event);
		caps->fifo_depth = 64;
		return 0;
	}
	if (request == ADI_TANDEM_AGC_IOC_ACQUIRE) {
		struct adi_tandem_agc_acquire *acquire = argument;
		assert(acquire->request.magic == ADI_TANDEM_AGC_REQUEST_MAGIC);
		assert(acquire->request.minimum_gain_db == -20);
		fill_status(mock, &acquire->status);
		mock->acquire_count++;
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
		return 0;
	}
	if (request == ADI_TANDEM_AGC_IOC_RELEASE) {
		assert(argument == NULL);
		mock->release_count++;
		return 0;
	}
#ifdef IIOD_HAS_KERNEL_PERSISTENT_HOP
	if (request == ADI_PERSISTENT_HOP_IOC_GET_CAPS) {
		struct adi_persistent_hop_caps_v1 *caps = argument;
		memset(caps, 0, sizeof(*caps));
		caps->version = ADI_PERSISTENT_HOP_ABI_VERSION;
		caps->size = sizeof(*caps);
		caps->features = ADI_PERSISTENT_HOP_REQUIRED_FEATURES;
		caps->maximum_profiles = 8;
		caps->fpga_identity = UINT32_C(0x54414732);
		caps->fpga_abi = 2;
		if (mock->hop_caps_unsupported)
			caps->fpga_abi = 1;
		return 0;
	}
	if (request == ADI_PERSISTENT_HOP_IOC_START) {
		struct adi_persistent_hop_start_v1 *start = argument;
		assert(start->version == ADI_PERSISTENT_HOP_ABI_VERSION);
		assert(start->required_features ==
			ADI_PERSISTENT_HOP_REQUIRED_FEATURES);
		start->actual_original_lo_hz = start->expected_original_lo_hz;
		start->active_profile = ADI_PERSISTENT_HOP_PROFILE_NONE;
		mock->hop_start_count++;
		return 0;
	}
	if (request == ADI_PERSISTENT_HOP_IOC_GET_COUNTER) {
		struct adi_persistent_hop_counter_v1 *counter = argument;
		memset(counter, 0, sizeof(*counter));
		counter->version = ADI_PERSISTENT_HOP_ABI_VERSION;
		counter->size = sizeof(*counter);
		counter->sample_counter = mock->hop_counter;
		return 0;
	}
	if (request == ADI_PERSISTENT_HOP_IOC_RECALL) {
		struct adi_persistent_hop_transition_v1 *transition = argument;
		transition->active_profile = transition->profile;
		transition->actual_lo_hz = transition->expected_lo_hz;
		transition->transition_before = mock->hop_counter;
		transition->transition_after = mock->hop_counter + 4;
		transition->device_event_id = ++mock->hop_event_id;
		mock->hop_counter += 4;
		mock->hop_recall_count++;
		return 0;
	}
	if (request == ADI_PERSISTENT_HOP_IOC_RESTORE) {
		struct adi_persistent_hop_restore_v1 *restore = argument;
		restore->actual_lo_hz = restore->expected_original_lo_hz;
		restore->transition_before = mock->hop_counter;
		restore->transition_after = mock->hop_counter + 3;
		restore->active_profile = ADI_PERSISTENT_HOP_PROFILE_NONE;
		mock->hop_counter += 3;
		mock->hop_restore_count++;
		return 0;
	}
#endif
	assert(!"unexpected ioctl");
	return -1;
}

static ssize_t mock_read(int fd, void *destination, size_t bytes, void *opaque)
{
	struct mock_device *mock = opaque;
	size_t count;
	assert(fd == 23);
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

static struct spf_tandem_syscalls mock_syscalls(struct mock_device *mock)
{
	const struct spf_tandem_syscalls calls = {
		.open_device = mock_open,
		.ioctl_device = mock_ioctl,
		.read_device = mock_read,
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

#ifdef IIOD_HAS_KERNEL_PERSISTENT_HOP
static void test_owner_authenticated_hop_wrappers(void)
{
	struct adi_persistent_hop_transition_v1 transition;
	struct adi_persistent_hop_restore_v1 restore;
	struct spf_tandem_session session;
	struct mock_device mock = {0};
	struct spf_tandem_syscalls calls = mock_syscalls(&mock);
	uint8_t wire[104];
	uint64_t counter;

	valid_request(wire);
	mock.epoch = 9;
	mock.hop_counter = 1000;
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_hop_start(&session, UINT64_C(11000000000)) == 0);
	assert(spf_tandem_session_hop_get_counter(&session, &counter) == 0);
	assert(counter == 1000);
	assert(spf_tandem_session_hop_recall(&session, 3,
		UINT64_C(11003000000), &transition) == 0);
	assert(transition.transition_before == 1000);
	assert(transition.transition_after == 1004);
	assert(transition.device_event_id == 1);
	assert(spf_tandem_session_hop_restore(&session,
		UINT64_C(11000000000), &restore) == 0);
	assert(restore.transition_before == 1004);
	assert(restore.transition_after == 1007);
	assert(mock.hop_start_count == 1 && mock.hop_recall_count == 1 &&
		mock.hop_restore_count == 1);
	spf_tandem_session_close(&session);

	memset(&mock, 0, sizeof(mock));
	mock.epoch = 10;
	mock.hop_caps_unsupported = 1;
	calls = mock_syscalls(&mock);
	assert(spf_tandem_session_init(&session, wire, sizeof(wire), &calls) == 0);
	assert(spf_tandem_session_acquire(&session) == 0);
	assert(spf_tandem_session_hop_start(&session,
		UINT64_C(11000000000)) == -EPROTONOSUPPORT);
	assert(mock.hop_start_count == 0);
	spf_tandem_session_close(&session);
}
#endif

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
#ifdef IIOD_HAS_KERNEL_PERSISTENT_HOP
	test_owner_authenticated_hop_wrappers();
#endif
	puts("SPF tandem session tests passed");
	return 0;
}
