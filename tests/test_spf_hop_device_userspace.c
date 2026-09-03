/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define iio_channel_attr_read mock_iio_channel_attr_read
#define iio_channel_attr_read_longlong mock_iio_channel_attr_read_longlong
#define iio_channel_attr_write_longlong mock_iio_channel_attr_write_longlong
#define iio_channel_find_attr mock_iio_channel_find_attr
#define iio_context_destroy mock_iio_context_destroy
#define iio_context_find_device mock_iio_context_find_device
#define iio_context_get_name mock_iio_context_get_name
#define iio_create_local_context mock_iio_create_local_context
#define iio_device_find_channel mock_iio_device_find_channel
#define iio_device_get_context mock_iio_device_get_context
#define iio_device_reg_read mock_iio_device_reg_read
#define spf_hop_scheduler_v1_create mock_spf_hop_scheduler_v1_create
#define spf_hop_scheduler_v1_destroy mock_spf_hop_scheduler_v1_destroy
#define spf_tandem_session_acquire mock_spf_tandem_session_acquire
#define spf_tandem_session_close mock_spf_tandem_session_close

#include "../iiod/spf-hop-device-userspace.c"

#undef iio_channel_attr_read
#undef iio_channel_attr_read_longlong
#undef iio_channel_attr_write_longlong
#undef iio_channel_find_attr
#undef iio_context_destroy
#undef iio_context_find_device
#undef iio_context_get_name
#undef iio_create_local_context
#undef iio_device_find_channel
#undef iio_device_get_context
#undef iio_device_reg_read
#undef spf_hop_scheduler_v1_create
#undef spf_hop_scheduler_v1_destroy
#undef spf_tandem_session_acquire
#undef spf_tandem_session_close

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static unsigned int mock_counter_reads;
static unsigned int mock_frequency_writes;
static unsigned int mock_tandem_closes;
static long long mock_frequency;

struct iio_context *mock_iio_create_local_context(void)
{
	return NULL;
}

void mock_iio_context_destroy(struct iio_context *context)
{
	(void)context;
}

const char *mock_iio_context_get_name(const struct iio_context *context)
{
	(void)context;
	return "local";
}

struct iio_device *mock_iio_context_find_device(
	const struct iio_context *context, const char *name)
{
	(void)context;
	(void)name;
	return NULL;
}

const struct iio_context *mock_iio_device_get_context(
	const struct iio_device *device)
{
	(void)device;
	return NULL;
}

struct iio_channel *mock_iio_device_find_channel(
	const struct iio_device *device, const char *name, bool output)
{
	(void)device;
	(void)name;
	(void)output;
	return NULL;
}

const char *mock_iio_channel_find_attr(const struct iio_channel *channel,
	const char *name)
{
	(void)channel;
	return name;
}

ssize_t mock_iio_channel_attr_read(const struct iio_channel *channel,
	const char *attribute, char *destination, size_t length)
{
	(void)channel;
	(void)attribute;
	(void)destination;
	(void)length;
	return -EIO;
}

int mock_iio_channel_attr_read_longlong(const struct iio_channel *channel,
	const char *attribute, long long *value)
{
	(void)channel;
	if (!strcmp(attribute, "frequency")) {
		*value = mock_frequency;
		return 0;
	}
	assert(!strcmp(attribute, "fastlock_recall"));
	return -EINVAL;
}

int mock_iio_channel_attr_write_longlong(const struct iio_channel *channel,
	const char *attribute, long long value)
{
	(void)channel;
	assert(!strcmp(attribute, "frequency"));
	mock_frequency = value;
	mock_frequency_writes++;
	return 0;
}

int mock_iio_device_reg_read(struct iio_device *device, uint32_t address,
	uint32_t *value)
{
	(void)device;
	assert(address == SPF_ADC_SAMPLE_COUNTER_LOW_REG);
	mock_counter_reads++;
	if (mock_counter_reads == 1)
		return -EIO;
	*value = 1234;
	return 0;
}

int mock_spf_tandem_session_acquire(struct spf_tandem_session *session)
{
	(void)session;
	return 0;
}

void mock_spf_tandem_session_close(struct spf_tandem_session *session)
{
	(void)session;
	mock_tandem_closes++;
}

int mock_spf_hop_scheduler_v1_create(
	const struct spf_hop_request_v1 *request,
	const struct spf_hop_scheduler_io_v1 *io, void *io_context,
	void **device_context, const struct spf_hop_device_ops_v1 **ops)
{
	(void)request;
	(void)io;
	(void)io_context;
	(void)device_context;
	(void)ops;
	return -EOPNOTSUPP;
}

void mock_spf_hop_scheduler_v1_destroy(void *device_context)
{
	(void)device_context;
}

static void test_counter_failure_does_not_skip_physical_restore(void)
{
	const uint64_t expected_lo = UINT64_C(10875000000);
	struct spf_hop_scheduler_restore_v1 restored = {0};
	struct spf_userspace_hop_io io = {0};
	pthread_mutex_t tandem_lock;

	mock_counter_reads = 0;
	mock_frequency_writes = 0;
	mock_tandem_closes = 0;
	mock_frequency = (long long)expected_lo - 1000;
	assert(pthread_mutex_init(&tandem_lock, NULL) == 0);
	io.rx = (struct iio_device *)(uintptr_t)1;
	io.lo = (struct iio_channel *)(uintptr_t)2;
	io.tandem = (struct spf_tandem_session *)(uintptr_t)3;
	io.tandem_lock = &tandem_lock;

	/* The missing before-counter evidence must fail the receipt, while the
	 * conventional LO and inactive Fast Lock state are still restored. */
	assert(userspace_restore(&io, expected_lo, &restored) == -EIO);
	assert(mock_counter_reads == 2);
	assert(mock_tandem_closes == 1);
	assert(mock_frequency_writes == 2);
	assert(mock_frequency == (long long)expected_lo);
	assert(restored.transition_before == 0);
	assert(restored.transition_after == 1234);
	assert(restored.actual_lo_frequency_hz == expected_lo);
	assert(restored.active_profile == UINT32_MAX);
	assert(pthread_mutex_destroy(&tandem_lock) == 0);
}

int main(void)
{
	test_counter_failure_does_not_skip_physical_restore();
	return 0;
}
