/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __SPF_TANDEM_SESSION_H__
#define __SPF_TANDEM_SESSION_H__

#include <linux/adi_tandem_agc.h>

#include <spf_gain_timeline.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SPF_TANDEM_DEVICE "/dev/tandem-agc-events"
#define SPF_TANDEM_EVENT_QUEUE_CAPACITY 64U
#define SPF_TANDEM_EVENT_RETENTION_FRAMES 2U

struct spf_tandem_frame_preview {
	struct adi_tandem_agc_status status;
	spf_gain_timeline_frame_t timeline;
	spf_gain_timeline_state_t next_state;
	uint64_t generation;
	uint64_t first_sample_sequence;
	uint32_t samples_per_channel;
	uint32_t event_sequence_start;
	size_t event_count;
	bool event_sequence_start_valid;
};

struct spf_tandem_syscalls {
	int (*open_device)(const char *path, int flags, void *opaque);
	int (*ioctl_device)(int fd, unsigned long request, void *argument,
		void *opaque);
	ssize_t (*read_device)(int fd, void *destination, size_t bytes,
		void *opaque);
	int (*wait_readable)(int fd, int timeout_ms, void *opaque);
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
	uint32_t fifo_depth;
	uint64_t last_event_sample_sequence;
	uint64_t pending_transition_watermark;
	uint64_t generation;
	uint8_t frame_rx1_gain_index;
	uint8_t frame_rx2_gain_index;
	struct adi_tandem_agc_status pending_status;
	spf_gain_timeline_state_t timeline_state;
	int fd;
	bool acquired;
	bool sequence_valid;
	bool sample_sequence_valid;
	bool pending_watermark_valid;
	bool authoritative_timeline;
};

int spf_tandem_request_decode(struct adi_tandem_agc_request_v1 *destination,
	const void *wire_request, size_t wire_bytes);
int spf_tandem_request_validate_event_window(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel);
int spf_tandem_request_validate_event_window_depth(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel, unsigned int kernel_buffers_count);
int spf_tandem_request_observation_interval(
	const struct adi_tandem_agc_request_v1 *request,
	uint32_t samples_per_channel, uint32_t *interval_samples);
int spf_tandem_session_init(struct spf_tandem_session *session,
	const void *wire_request, size_t wire_bytes,
	const struct spf_tandem_syscalls *syscalls);
int spf_tandem_session_enable_authoritative_timeline(
	struct spf_tandem_session *session);
int spf_tandem_session_acquire(struct spf_tandem_session *session);
int spf_tandem_session_heartbeat(struct spf_tandem_session *session);
int spf_tandem_session_snapshot_watermark(struct spf_tandem_session *session);
int spf_tandem_session_preview(struct spf_tandem_session *session,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	spf_gain_event_v7_t *events, size_t event_capacity,
	struct spf_tandem_frame_preview *preview);
int spf_tandem_session_commit(struct spf_tandem_session *session,
	const struct spf_tandem_frame_preview *preview);
int spf_tandem_session_collect(struct spf_tandem_session *session,
	uint64_t first_sample_sequence, uint32_t samples_per_channel,
	struct adi_tandem_agc_event *events, size_t event_capacity,
	size_t *event_count);
void spf_tandem_session_close(struct spf_tandem_session *session);

#endif
