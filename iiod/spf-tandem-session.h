/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_TANDEM_SESSION_H__
#define __SPF_TANDEM_SESSION_H__

#include <linux/adi_tandem_agc.h>
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
#include <linux/adi_persistent_hop.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SPF_TANDEM_DEVICE "/dev/tandem-agc-events"
#define SPF_TANDEM_EVENT_QUEUE_CAPACITY 64U
#define SPF_TANDEM_EVENT_RETENTION_FRAMES 2U

struct spf_tandem_syscalls {
	int (*open_device)(const char *path, int flags, void *opaque);
	int (*ioctl_device)(int fd, unsigned long request, void *argument,
		void *opaque);
	ssize_t (*read_device)(int fd, void *destination, size_t bytes,
		void *opaque);
	int (*close_device)(int fd, void *opaque);
	void *opaque;
};

struct spf_tandem_session {
	struct spf_tandem_syscalls syscalls;
	struct adi_tandem_agc_request_v1 request;
	struct adi_tandem_agc_status status;
	struct adi_tandem_agc_event queue[SPF_TANDEM_EVENT_QUEUE_CAPACITY];
	size_t queue_count;
	uint32_t expected_event_sequence;
	uint32_t initial_overflow_count;
	uint32_t frame_transition_count;
	uint64_t last_event_sample_sequence;
	uint8_t frame_rx1_gain_index;
	uint8_t frame_rx2_gain_index;
	int fd;
	bool acquired;
	bool sequence_valid;
	bool sample_sequence_valid;
};

int spf_tandem_request_decode(struct adi_tandem_agc_request_v1 *destination,
	const void *wire_request, size_t wire_bytes);
int spf_tandem_request_validate_event_window(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel);
int spf_tandem_request_observation_interval(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel, uint32_t *interval_samples);
int spf_tandem_session_init(struct spf_tandem_session *session,
	const void *wire_request, size_t wire_bytes,
	const struct spf_tandem_syscalls *syscalls);
int spf_tandem_session_acquire(struct spf_tandem_session *session);
int spf_tandem_session_heartbeat(struct spf_tandem_session *session);
int spf_tandem_session_collect(struct spf_tandem_session *session,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	struct adi_tandem_agc_event *events, size_t event_capacity,
	size_t *event_count);
#ifdef IIOD_HAS_BUFFER_PERSISTENT_HOP
int spf_tandem_session_hop_start(struct spf_tandem_session *session,
	uint64_t expected_original_lo_hz);
int spf_tandem_session_hop_get_counter(struct spf_tandem_session *session,
	uint64_t *sample_counter);
int spf_tandem_session_hop_recall(struct spf_tandem_session *session,
	uint32_t profile, uint64_t expected_lo_hz,
	struct adi_persistent_hop_transition_v1 *transition);
int spf_tandem_session_hop_restore(struct spf_tandem_session *session,
	uint64_t expected_original_lo_hz,
	struct adi_persistent_hop_restore_v1 *restore);
#endif
void spf_tandem_session_close(struct spf_tandem_session *session);

#endif
