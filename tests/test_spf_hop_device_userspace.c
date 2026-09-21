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
#define spf_hop_scheduler_v2_create mock_spf_hop_scheduler_v2_create
#define spf_hop_scheduler_v2_destroy mock_spf_hop_scheduler_v2_destroy
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
#undef spf_hop_scheduler_v2_create
#undef spf_hop_scheduler_v2_destroy
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
static bool mock_profiles_enabled;
static unsigned int mock_saved_slot, mock_save_writes;

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
	return mock_profiles_enabled ? (const struct iio_context *)(uintptr_t)1 : NULL;
}

struct iio_channel *mock_iio_device_find_channel(
	const struct iio_device *device, const char *name, bool output)
{
	(void)device;
	if (mock_profiles_enabled) {
		assert(!strcmp(name, "altvoltage0") && output);
		return (struct iio_channel *)(uintptr_t)2;
	}
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
	if (mock_profiles_enabled && !strcmp(attribute, "fastlock_save"))
		return snprintf(destination, length, "%u 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0", mock_saved_slot);
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
	if (mock_profiles_enabled && !strcmp(attribute, "fastlock_save")) {
		assert(value >= 0 && value < 8);
		mock_saved_slot = (unsigned)value; ++mock_save_writes;
		return 0;
	}
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

#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
static unsigned mock_adaptive_creates, mock_adaptive_destroys;
static uint8_t mock_adaptive_mask;
static const struct spf_hop_device_ops_v2 mock_adaptive_ops = {0};
int mock_spf_hop_scheduler_v2_create(const struct spf_hop_request_v2 *r,
	const struct spf_hop_scheduler_io_v1 *ops, void *io,
	const struct spf_hop_scheduler_policy_v2 *policy, void *policy_context,
	void **output, const struct spf_hop_device_ops_v2 **device_ops)
{
	assert(r->policy.generation == 9 && r->policy.mode == SPF_HOP_ADAPTIVE);
	assert(r->eligible_target_mask == mock_adaptive_mask);
	assert(ops == &userspace_io && io && policy && policy_context);
	++mock_adaptive_creates;
	*output = io; *device_ops = &mock_adaptive_ops;
	return 0;
}
void mock_spf_hop_scheduler_v2_destroy(void *context)
{ ++mock_adaptive_destroys; free(context); }
static int unused_choose(void *p, uint64_t visit, uint64_t now, struct spf_hop_choice_v2 *c)
{ (void)p; (void)visit; (void)now; (void)c; assert(0); return -EIO; }
static int unused_commit(void *p, const struct spf_hop_device_event_v2 *e, uint64_t first, uint64_t end)
{ (void)p; (void)e; (void)first; (void)end; assert(0); return -EIO; }

static void test_adaptive_factory_validates_before_creating_scheduler(void)
{
	struct spf_hop_request_v2 r = {0};
	uint8_t values[16] = {0};
	const struct spf_hop_scheduler_policy_v2 policy = {unused_choose, unused_commit};
	const struct spf_hop_device_ops_v2 *ops = NULL;
	struct iio_device *rx = (void *)(uintptr_t)1, *phy = (void *)(uintptr_t)2;
	struct spf_tandem_session tandem = {0};
	pthread_mutex_t lock;
	void *context = NULL;
	r.geometry.required_features = SPF_HOP_REQUIRED_FEATURES_V1;
	r.geometry.flags = SPF_HOP_REQUEST_FLAGS_V1;
	r.geometry.session_id = 71;
	r.geometry.sample_rate_hz = r.geometry.rf_bandwidth_hz = 2500000;
	r.geometry.dwell_samples = 300000; r.geometry.transition_guard_samples = 2500;
	r.geometry.dwell_count = 64; r.geometry.capture_span_samples = 10000000;
	for (unsigned i = 0; i < 8; ++i) {
		r.geometry.profiles[i] = (struct spf_hop_profile_v1){
			.profile_id = i, .fastlock_slot = 7 - i, .center_frequency_hz = 1000000000 + i * 1000000,
			.lo_frequency_hz = 1000000000 + i * 1000000, .profile_crc32 = crc32_bytes(values, sizeof(values)),
		};
	}
	r.policy = (struct spf_hop_policy_v2){0, SPF_HOP_ADAPTIVE, 3, 3, 3, 1, 2000, 3000, 160, 1000, 3};
	mock_frequency = 1000000000; mock_frequency_writes = 0;
	mock_profiles_enabled = true;
	assert(!pthread_mutex_init(&lock, NULL));
	assert(spf_hop_device_userspace_v2_open(rx, phy, &tandem, &lock, &r, &policy,
		&r, &context, &ops) == -EINVAL);
	assert(!context && !mock_save_writes && !mock_adaptive_creates);
	r.policy.generation = 9;
	r.eligible_target_mask = 0x0f;
	mock_adaptive_mask = 0x0f;
	r.geometry.profiles[4].profile_crc32 ^= 1;
	assert(spf_hop_device_userspace_v2_open(rx, phy, &tandem, &lock, &r, &policy,
		&r, &context, &ops) == -ESTALE);
	assert(!context && mock_save_writes == 5 && !mock_adaptive_creates);
	r.geometry.profiles[4].profile_crc32 ^= 1;
	assert(!spf_hop_device_userspace_v2_open(rx, phy, &tandem, &lock, &r, &policy,
		&r, &context, &ops));
	assert(context && ops == &mock_adaptive_ops && mock_adaptive_creates == 1);
	assert(mock_save_writes == 13 && !mock_frequency_writes);
	spf_hop_device_userspace_v2_destroy(context);
	assert(mock_adaptive_destroys == 1);

	/* The upper-edge request must validate the same first eligible LO that
	 * scheduler submit subsequently requires.  A profile-0 pre-tune is stale. */
	r.eligible_target_mask = 0xf0;
	mock_adaptive_mask = 0xf0;
	mock_frequency = (long long)r.geometry.profiles[0].lo_frequency_hz;
	context = NULL; ops = NULL;
	assert(spf_hop_device_userspace_v2_open(rx, phy, &tandem, &lock, &r, &policy,
		&r, &context, &ops) == -ESTALE);
	assert(!context && mock_adaptive_creates == 1);
	mock_frequency = (long long)r.geometry.profiles[4].lo_frequency_hz;
	assert(!spf_hop_device_userspace_v2_open(rx, phy, &tandem, &lock, &r, &policy,
		&r, &context, &ops));
	assert(context && ops == &mock_adaptive_ops && mock_adaptive_creates == 2);
	assert(mock_save_writes == 21 && !mock_frequency_writes);
	spf_hop_device_userspace_v2_destroy(context);
	assert(mock_adaptive_destroys == 2);
	mock_profiles_enabled = false;
	assert(!pthread_mutex_destroy(&lock));
}
#endif

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
#ifdef IIOD_HAS_SCANNER_ADAPTIVE_HOP
	test_adaptive_factory_validates_before_creating_scheduler();
#endif
	return 0;
}
