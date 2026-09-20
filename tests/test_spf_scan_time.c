/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual provider query, mock only the owner ioctl. No radio access. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../iiod/spf-buffer-metadata.c"
#include <assert.h>

static uint32_t low = 16;
static int fail_ioctl;
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
	assert(!pthread_mutex_destroy(&ctx.scan_lock));
	puts("PASS: real counter query lifecycle, contention, wrap, stale identity and nonfatal errors");
	return 0;
}
