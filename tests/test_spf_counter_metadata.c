/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "spf-counter-metadata.h"
#include "adi-rx-counter.h"
#include "spf-tandem-session.h"
#include <spf_gain_sampler.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <iio.h>

static const uint8_t request[32] = {
    0x53, 0x50, 0x46, 0x43, 1, 0, 32, 0, 7, 0, 0, 0, 3, 0, 0, 0,
    0x00, 0x87, 0x93, 0x03, 4, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0}; /* 60 MHz, four complex samples */
static uint8_t raw[24] = {0xfe, 0xff, 0xff, 0xff, 1, 0, 0, 0, 1, 0, 2, 0,
			  3,	0,    4,    0,	  5, 0, 6, 0, 7, 0, 8, 0};
static int opens, closes, acquisitions, reject_acquire;
int __wrap_open(const char *path, int flags, ...)
{
	(void)flags;
	assert(!strcmp(path, "/dev/tandem-agc-events"));
	++opens;
	return 97;
}
int __wrap_close(int fd)
{
	assert(fd == 97);
	++closes;
	return 0;
}
int __wrap_ioctl(int fd, unsigned long cmd, ...)
{
	va_list args;
	const struct adi_rx_counter_request *lease;
	assert(fd == 97 && cmd == ADI_RX_COUNTER_IOC_ACQUIRE);
	va_start(args, cmd);
	lease = va_arg(args, const struct adi_rx_counter_request *);
	va_end(args);
	assert(lease->magic == ADI_RX_COUNTER_MAGIC && lease->scan_mask == 3);
	assert(lease->sample_rate_hz == 60000000 && lease->samples_per_channel == 4);
	++acquisitions;
	if (reject_acquire) {
		errno = EOPNOTSUPP;
		return -1;
	}
	return 0;
}
const char *__wrap_iio_device_get_name(const struct iio_device *dev)
{
	(void)dev;
	return "cf-ad9361-lpc";
}
void *__wrap_iio_buffer_start(const struct iio_buffer *buffer)
{
	(void)buffer;
	return raw;
}
int __wrap_spf_tandem_session_acquire(struct spf_tandem_session *session)
{
	(void)session;
	assert(!"counter capture acquired paired tandem");
	return -EIO;
}
bool __wrap_spf_gain_sampler_start(spf_gain_sampler_t *sampler, uint32_t interval)
{
	(void)sampler;
	(void)interval;
	assert(!"counter capture started paired sampler");
	return false;
}

int main(int argc, char **argv)
{
	uint8_t frame[80], mutated[80];
	struct spf_counter_request decoded;
	struct iiod_buffer_metadata_frame_info info;
	struct iiod_buffer_burst_plan plan;
	struct iio_device *dev = (struct iio_device *)(uintptr_t)1;
	struct iio_buffer *buffer = (struct iio_buffer *)(uintptr_t)2;
	void *ctx = NULL;
	size_t extra = 0, offset, iqbytes;
	uint32_t mask = 3;
	unsigned i;
	assert(spf_counter_request_decode(request, 32, 4, 3, &decoded) == 0);
	assert(spf_counter_request_decode(request, 31, 4, 3, &decoded) == -EINVAL);
	assert(spf_counter_request_decode(request, 32, 4, 12, &decoded) == -EINVAL);
	assert(spf_counter_request_decode(request, 32, 3, 3, &decoded) == -EINVAL);
	assert(spf_counter_frame_build(frame, 80, 1, 0, UINT64_C(0x1fffffffe), 0, 4, 60000000) ==
	       80);
	assert(spf_counter_frame_describe(frame, 80, &info) == 0);
	assert(info.frame_end == UINT64_C(0x200000002));
	for (i = 0; i < 80; ++i) {
		memcpy(mutated, frame, 80);
		mutated[i] ^= 1;
		assert(spf_counter_frame_describe(mutated, 80, &info) < 0);
	}
	assert(spf_counter_frame_build(frame, 80, 1, 0, UINT64_MAX - 2, 0, 4, 60000000) < 0);
	assert(spf_counter_frame_rebase(frame, 80, UINT64_C(0x1fffffff8)) == 0);
	assert(spf_counter_frame_describe(frame, 80, &info) == 0 &&
	       info.missing_samples_before == 6);
	assert(spf_counter_frame_rebase(frame, 80, UINT64_C(0x200000002)) == -ERANGE);
	if (argc > 1) {
		FILE *out = fopen(argv[1], "wb");
		assert(out);
		assert(fwrite(frame, 1, 80, out) == 80);
		assert(fclose(out) == 0);
	}
	/* Execute the actual provider dispatch/lifecycle. No RX1 device exists. */
	assert(iiod_buffer_metadata_open(dev, 4, &mask, 1, 4, request, 32, &ctx, &extra, &plan) ==
	       0);
	assert(ctx && extra == 2 && opens == 1 && acquisitions == 1);
	assert(iiod_buffer_metadata_buffer_opened(ctx, 50) == 0);
	assert(iiod_buffer_metadata_before_refill(ctx) == 0);
	assert(iiod_buffer_metadata_after_refill(ctx) == 0);
	assert(iiod_buffer_metadata_get(ctx, dev, buffer, 23, frame, 80, &offset, &iqbytes) ==
	       -EIO);
	assert(iiod_buffer_metadata_get(ctx, dev, buffer, 24, frame, 79, &offset, &iqbytes) ==
	       -ENOSPC);
	assert(iiod_buffer_metadata_get(ctx, dev, buffer, 24, frame, 80, &offset, &iqbytes) == 80);
	assert(offset == 8 && iqbytes == 16 && raw[offset] == 1 && raw[offset + 14] == 8);
	assert(iiod_buffer_metadata_describe_frame(ctx, frame, 80, &info) == 0);
	assert(info.first_sample_sequence == UINT64_C(0x1fffffffc));
	assert(iiod_buffer_metadata_get(ctx, dev, buffer, 24, frame, 80, &offset, &iqbytes) ==
	       -ERANGE);
	/* Nine samples later: five missing, deliberately not a frame multiple. */
	raw[0] = 7;
	raw[1] = raw[2] = raw[3] = 0;
	raw[4] = 2;
	assert(iiod_buffer_metadata_get(ctx, dev, buffer, 24, frame, 80, &offset, &iqbytes) == 80);
	assert(iiod_buffer_metadata_describe_frame(ctx, frame, 80, &info) == 0 &&
	       info.missing_samples_before == 5);
	assert(iiod_buffer_metadata_rebase_frame(ctx, frame, 80, UINT64_C(0x1fffffffc)) == 0);
	assert(iiod_buffer_metadata_describe_frame(ctx, frame, 80, &info) == 0 &&
	       info.missing_samples_before == 9);
	iiod_buffer_metadata_close(ctx);
	assert(closes == 1);
	reject_acquire = 1;
	ctx = NULL;
	assert(iiod_buffer_metadata_open(dev, 4, &mask, 1, 4, request, 32, &ctx, &extra, &plan) ==
	       -EOPNOTSUPP);
	assert(!ctx && closes == 2);
	mask = 12;
	assert(iiod_buffer_metadata_open(dev, 4, &mask, 1, 4, request, 32, &ctx, &extra, &plan) ==
	       -EINVAL);
	assert(opens == 2 && acquisitions == 2);
	puts("PASS: counter wire, real provider dispatch, prefix, exact gaps and cleanup");
	return 0;
}
