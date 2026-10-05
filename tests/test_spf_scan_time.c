/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual provider query, mock only the owner ioctl. No radio access. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../iiod/spf-buffer-metadata.c"
#include <assert.h>

static uint32_t low = 16;
static int fail_ioctl;
static int capture_error;
static bool capture_complete;

int spf_scan_session_terminal(struct spf_scan_session *session,
	struct spf_scan_terminal *terminal)
{
	(void)session;
	(void)terminal;
	return -EAGAIN;
}

bool spf_scan_session_capture_complete(const struct spf_scan_session *session)
{
	(void)session;
	return capture_complete;
}

int spf_scan_session_fail(struct spf_scan_session *session, uint64_t counter, int error)
{
	(void)session;
	(void)counter;
	capture_error = error;
	return error;
}

int spf_scan_session_fail_stage(struct spf_scan_session *session,
	uint64_t counter, int error, uint32_t stage)
{
	(void)stage;
	return spf_scan_session_fail(session, counter, error);
}

int spf_scan_session_diagnostics(struct spf_scan_session *session,
	uint64_t identity, uint64_t generation, char *output, size_t capacity)
{
	(void)session; (void)identity; (void)generation;
	(void)output; (void)capacity;
	return -EOPNOTSUPP;
}

int spf_scan_session_take_output(struct spf_scan_session *session,
	struct spf_scan_session_output *output)
{
	(void)session;
	(void)output;
	return -EAGAIN;
}
static int snapshot(int fd, unsigned long command, void *argument)
{
	struct adi_rx_counter_scan_snapshot *result = argument;
	assert(fd == 17 && command == ADI_RX_COUNTER_IOC_SCAN_SNAPSHOT);
	if (fail_ioctl) { errno = EIO; return -1; }
	result->counter = low;
	return 0;
}

int main(void)
{
	struct spf_iiod_metadata_context ctx = {0};
	struct spf_scan_time_query query = {1, 2, 3};
	struct spf_scan_time result;
	ctx.scan_enabled = true;
	ctx.scan_setup.session = 2;
	ctx.scan_setup.generation = 3;
	ctx.scan_setup.source_rate_hz = 20000000;
	ctx.scan_radio.fd = 17;
	ctx.scan_radio.call_ioctl = snapshot;
	ctx.scan_radio.configured = true;
	ctx.scan_counter_anchor = UINT64_C(0xfffffff0);
	assert(!pthread_mutex_init(&ctx.scan_lock, NULL));
	scan_clock_identity(&ctx);
	assert(ctx.scan_clock_epoch);
	assert(iiod_buffer_metadata_scan_time(&ctx, &query, &result) == -EAGAIN);
	ctx.scan_thread_live = true;
	assert(!iiod_buffer_metadata_scan_time(&ctx, &query, &result));
	assert(result.counter == UINT64_C(0x100000010));
	assert(result.maximum_snapshot_age_ns == UINT64_MAX);
	assert(result.monotonic_after_ns >= result.monotonic_before_ns);
	assert(ctx.scan_counter_anchor == UINT64_C(0xfffffff0));
	query.generation++;
	assert(iiod_buffer_metadata_scan_time(&ctx, &query, &result) == -ESTALE);
	query.generation--;
	assert(!pthread_mutex_lock(&ctx.scan_lock));
	assert(iiod_buffer_metadata_scan_time(&ctx, &query, &result) == -EAGAIN);
	assert(!pthread_mutex_unlock(&ctx.scan_lock));
	fail_ioctl = 1;
	assert(iiod_buffer_metadata_scan_time(&ctx, &query, &result) == -EIO);
	assert(!ctx.scan_radio.faulted);
	fail_ioctl = 0;
	low = 0xffffffe0;
	assert(iiod_buffer_metadata_scan_time(&ctx, &query, &result) == -ERANGE);
	assert(!ctx.scan_radio.faulted);
	ctx.scan_finished = true;
	assert(iiod_buffer_metadata_scan_time(&ctx, &query, &result) == -ESHUTDOWN);
	/* Exercise the real provider watchdog without sleeping or radio access. */
	struct spf_scan_session_output output;
	ctx.scan_finished = false;
	ctx.scan_thread_live = false;
	ctx.scan_setup.source_rate_hz = 1250000;
	ctx.samples_per_channel = 1000000;
	assert(!iiod_buffer_metadata_scan_start(&ctx));
	assert(iiod_buffer_metadata_scan_take(&ctx, &output) == -EAGAIN);
	assert(!capture_error);
	ctx.scan_last_dma_ns = scan_monotonic_ns() - UINT64_C(6000000000);
	assert(iiod_buffer_metadata_scan_take(&ctx, &output) == -EAGAIN);
	assert(capture_error == -ETIMEDOUT && ctx.scan_finished);
	ctx.scan_finished = ctx.scan_cancel_requested = false;
	capture_error = 0;
	ctx.samples_per_channel = 10000000; /* 8-second blocks remain valid. */
	assert(iiod_buffer_metadata_scan_take(&ctx, &output) == -EAGAIN);
	assert(!capture_error);
	ctx.scan_last_dma_ns = scan_monotonic_ns() - UINT64_C(19000000000);
	assert(iiod_buffer_metadata_scan_take(&ctx, &output) == -EAGAIN);
	assert(capture_error == -ETIMEDOUT);
	ctx.scan_finished = ctx.scan_cancel_requested = false;
	assert(iiod_buffer_metadata_scan_fail(&ctx, -EOVERFLOW) == -EOVERFLOW);
	assert(capture_error == -EOVERFLOW && ctx.scan_finished);
	struct spf_scan_terminal terminal;
	capture_complete = true;
	assert(iiod_buffer_metadata_scan_terminal(&ctx, &terminal) == -EOVERFLOW);
	capture_complete = false;
	assert(iiod_buffer_metadata_scan_terminal(&ctx, &terminal) == -EAGAIN);
	assert(!pthread_mutex_destroy(&ctx.scan_lock));
	puts("PASS: real counter query lifecycle, contention, wrap, stale identity and nonfatal errors");
	return 0;
}
