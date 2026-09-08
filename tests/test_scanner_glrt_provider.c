/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual SPF provider, hop engine, GLRT bridge and isolated numerical worker.
 * Only hardware-bound IIO, gain/RSSI and device operations are substituted.
 * No receive buffer, ioctl, sysfs write or network context is opened. */
#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define iio_buffer_start fixture_buffer_start
#define iio_device_get_context fixture_context
#define iio_context_find_device fixture_phy
#define iio_device_find_channel fixture_channel
#define iio_channel_attr_read_longlong fixture_attr
#define iio_device_reg_read fixture_reg_read
#define iio_device_reg_write fixture_reg_write
#define spf_gain_is_full_table_mode fixture_full_table
#define spf_gain_is_digital_gain_disabled fixture_digital_disabled
#define spf_gain_sampler_start fixture_sampler_start
#define spf_gain_sampler_stop fixture_sampler_stop
#define spf_gain_sampler_collect fixture_gain_collect
#define spf_gain_sampler_collect_rssi fixture_rssi_collect
#define spf_temperature_sampler_start fixture_temperature_start
#define spf_tandem_session_collect fixture_tandem_collect
#define spf_hop_device_v1_open fixture_hop_open
#define spf_hop_device_v1_destroy fixture_hop_destroy
#define spf_scanner_glrt_frame fixture_glrt_frame
#undef _POSIX_C_SOURCE
#include "../iiod/spf-buffer-metadata.c"
#undef spf_scanner_glrt_frame
ssize_t spf_scanner_glrt_frame(struct spf_scanner_glrt *, const void *, size_t, void *, size_t);

ssize_t fixture_glrt_frame(struct spf_scanner_glrt *state, const void *legacy,
	size_t bytes, void *output, size_t capacity)
{
	uint8_t scratch[65536];
	/* Simulate a drain arriving in the feed/frame gap, including the terminal
	 * block. It must not consume results, advance sequence or emit FINAL. */
	assert(spf_scanner_glrt_drain(state, scratch, sizeof(scratch)) == -EBUSY);
	return spf_scanner_glrt_frame(state, legacy, bytes, output, capacity);
}

static uint32_t fixture_rate, register_writes, sampler_starts;
static uint64_t fixture_first;
static unsigned int drain_calls, event_delay, restores;
static struct spf_hop_request_v1 fixture_request;
static uint8_t legacy_baseline[7][65536];
static size_t legacy_baseline_bytes[7];
static bool inject_failure;
static struct iio_device *const rx_device = (struct iio_device *)(uintptr_t)16;
static struct iio_device *const phy_device = (struct iio_device *)(uintptr_t)32;

void *fixture_buffer_start(const struct iio_buffer *buffer) { return (void *)buffer; }
const struct iio_context *fixture_context(const struct iio_device *dev)
{ assert(dev == rx_device); return (void *)(uintptr_t)48; }
struct iio_device *fixture_phy(const struct iio_context *ctx, const char *name)
{ assert(ctx && !strcmp(name, "ad9361-phy")); return phy_device; }
struct iio_channel *fixture_channel(const struct iio_device *dev, const char *name, bool output)
{ assert(dev == phy_device && !strcmp(name, "voltage0") && !output); return (void *)(uintptr_t)64; }
int fixture_attr(const struct iio_channel *channel, const char *name, long long *value)
{ assert(channel && (!strcmp(name, "sampling_frequency") || !strcmp(name, "rf_bandwidth"))); *value = fixture_rate; return 0; }
int fixture_reg_read(struct iio_device *dev, uint32_t address, uint32_t *value)
{ assert(dev == rx_device && address == SPF_ADC_TIMESTAMP_CONTROL_REG); *value = 0; return 0; }
int fixture_reg_write(struct iio_device *dev, uint32_t address, uint32_t value)
{ assert(dev == rx_device && address == SPF_ADC_TIMESTAMP_CONTROL_REG); (void)value; register_writes++; return 0; }
bool fixture_full_table(struct iio_device *dev) { assert(dev == phy_device); return true; }
bool fixture_digital_disabled(struct iio_device *dev) { assert(dev == phy_device); return true; }
bool fixture_sampler_start(spf_gain_sampler_t *sampler, uint32_t interval)
{ assert(sampler && interval); sampler_starts++; return true; }
void fixture_sampler_stop(spf_gain_sampler_t *sampler) { assert(sampler); }
bool fixture_temperature_start(struct spf_temperature_sampler *sampler)
{ assert(sampler); return false; }

uint16_t fixture_gain_collect(spf_gain_sampler_t *sampler, uint64_t first,
	uint32_t samples, spf_gain_observation_v3_t *out, uint16_t capacity, uint32_t *overflow)
{
	assert(sampler && samples && capacity);
	*out = (spf_gain_observation_v3_t){.sample_sequence_before = first - 2,
		.sample_sequence_after = first - 1, .read_duration_ns = 1000,
		.flags = SPF_GAIN_OBSERVATION_VALID | SPF_GAIN_OBSERVATION_SAMPLE_INTERVAL_VALID,
		.rx1_gain_index = 20, .rx2_gain_index = 20, .rx1_gain_db = 10, .rx2_gain_db = 10};
	*overflow = 0;
	return 1;
}
bool fixture_rssi_collect(spf_gain_sampler_t *sampler, uint64_t first, uint32_t samples,
	spf_rssi_pair_t *start, spf_rssi_pair_t *end, uint32_t *overflow)
{
	assert(sampler && first && samples);
	*start = *end = (spf_rssi_pair_t){.rx1_qdb = 100, .rx2_qdb = 110, .valid = true};
	*overflow = 0; return true;
}
int fixture_tandem_collect(struct spf_tandem_session *session, uint64_t first,
	uint32_t samples, struct adi_tandem_agc_event *events, size_t capacity, size_t *count)
{
	assert(first && samples && events && capacity);
	session->status = (struct adi_tandem_agc_status){
		.version = ADI_TANDEM_AGC_ABI_VERSION, .size = sizeof(session->status),
		.state = ADI_TANDEM_AGC_STATE_ARMED_HOLD, .ownership_epoch = 1,
		.gain_table_id = ADI_TANDEM_AGC_GAIN_TABLE_1300_4000_MHZ,
		.minimum_gain_db = 0, .maximum_gain_db = 62, .initial_gain_db = 10,
		.minimum_gain_index = 0, .maximum_gain_index = 76,
		.rx1_gain_index = 20, .rx2_gain_index = 20};
	*count = 0; return 0;
}
static int fixture_submit(void *opaque, const struct spf_hop_request_v1 *request)
{ assert(opaque && request->session_id == fixture_request.session_id); return 0; }
static int fixture_events(void *opaque, struct spf_hop_device_event_v1 *events,
	size_t capacity, size_t *count, uint64_t *dropped)
{
	assert(opaque && capacity);
	*count = 0; *dropped = 0;
	if (drain_calls++ != event_delay) return 0;
	*events = (struct spf_hop_device_event_v1){.transition_before = fixture_first + 3,
		.transition_after = fixture_first + 7, .actual_lo_frequency_hz =
		fixture_request.profiles[0].lo_frequency_hz,
		.actual_if_offset_hz = fixture_request.if_offset_hz,
		.device_event_id = 1, .from_profile = SPF_HOP_PROFILE_NONE, .to_profile = 0,
		.kind = SPF_HOP_EVENT_STARTUP, .flags = SPF_HOP_EVENT_FLAGS_V1, .fastlock_slot = 0};
	*count = 1; return 0;
}
static int fixture_restore(void *opaque, uint16_t reason, struct spf_hop_restore_receipt_v1 *receipt)
{
	assert(opaque && reason);
	uint64_t end = fixture_first + fixture_request.dwell_samples + 1000;
	*receipt = (struct spf_hop_restore_receipt_v1){.transition_before = end,
		.transition_after = end + 2, .restored_lo_frequency_hz = 900000000,
		.restored_profile = SPF_HOP_PROFILE_NONE, .flags = SPF_HOP_EVENT_FLAGS_V1};
	restores++; return 0;
}
static const struct spf_hop_device_ops_v1 fixture_ops = {
	.submit_plan = fixture_submit, .drain_events = fixture_events, .cancel_restore = fixture_restore};
int fixture_hop_open(const struct iio_device *rx, const struct iio_device *phy,
	struct spf_tandem_session *tandem, pthread_mutex_t *lock,
	const struct spf_hop_request_v1 *request, void **context,
	const struct spf_hop_device_ops_v1 **ops)
{
	assert(rx == rx_device && phy == phy_device && tandem && lock);
	fixture_request = *request; drain_calls = 0; restores = 0;
	*context = &fixture_request; *ops = &fixture_ops; return 0;
}
void fixture_hop_destroy(void *context) { assert(context == &fixture_request); }

static uint32_t read32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t read64(const uint8_t *p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }

static size_t make_request(uint8_t *packet, bool glrt)
{
	uint8_t legacy[104 + SPF_HOP_REQUEST_BYTES];
	struct adi_tandem_agc_request_v1 tandem = {.magic = ADI_TANDEM_AGC_REQUEST_MAGIC,
		.version = ADI_TANDEM_AGC_ABI_VERSION, .size = 104,
		.required_features = ADI_TANDEM_AGC_FEATURE_EVENTS | ADI_TANDEM_AGC_FEATURE_FAIL_CLOSED |
			ADI_TANDEM_AGC_FEATURE_PAIRED_GAIN, .mode = ADI_TANDEM_AGC_MODE_HOLD,
		.observation_capacity = 1, .event_capacity = 16, .minimum_gain_db = 0,
		.maximum_gain_db = 62, .initial_gain_db = 10, .power_measurement_samples = 1024,
		.cooldown_periods = 2, .overflow_policy = ADI_TANDEM_AGC_POLICY_FAIL_SESSION,
		.sync_fault_policy = ADI_TANDEM_AGC_POLICY_FAIL_SESSION};
	struct spf_hop_request_v1 hop = {.required_features = SPF_HOP_REQUIRED_FEATURES_V1,
		.flags = SPF_HOP_REQUEST_FLAGS_V1, .session_id = 77, .sample_rate_hz = fixture_rate,
		.rf_bandwidth_hz = fixture_rate, .dwell_samples = fixture_rate / 50 * 6,
		.transition_guard_samples = 64, .dwell_count = 1, .capture_span_samples = fixture_rate / 50 * 6};
	unsigned int j;
	for (j = 0; j < 8; j++) {
		hop.profiles[j] = (struct spf_hop_profile_v1){.profile_id = j, .fastlock_slot = j,
			.center_frequency_hz = (j < 4 ? UINT64_C(959687500) : UINT64_C(1190312500)) +
				(j % 4) * UINT64_C(250000000), .profile_crc32 = 1 + j};
		hop.profiles[j].lo_frequency_hz = hop.profiles[j].center_frequency_hz;
	}
	memcpy(legacy, &tandem, 104);
	assert(spf_hop_request_v1_encode(legacy + 104, SPF_HOP_REQUEST_BYTES, &hop) == 0);
	if (!glrt) { memcpy(packet, legacy, sizeof(legacy)); return sizeof(legacy); }
	leo_scanner_glrt_request_v1 request = {.generation = 9, .rx = 1,
		.legacy_request = legacy, .legacy_bytes = sizeof(legacy)};
	/* This fixture uses pinned test-only identities, never release identities. */
	for (j = 0; j < 32; j++) {
		unsigned int value;
		assert(sscanf(IIOD_SCANNER_GLRT_ALGORITHM_SHA256 + 2 * j, "%2x", &value) == 1);
		request.algorithm_sha256[j] = (uint8_t)value;
		assert(sscanf(IIOD_SCANNER_GLRT_CONFIGURATION_SHA256 + 2 * j, "%2x", &value) == 1);
		request.configuration_sha256[j] = (uint8_t)value;
	}
	ssize_t bytes = leo_scanner_glrt_request_encode(&request, packet, 4096);
	assert(bytes > 0); return (size_t)bytes;
}

static void test_rejected_open_is_side_effect_free(void)
{
	uint8_t request[4096]; uint32_t mask = 15;
	struct iiod_buffer_burst_plan plan;
	void *context = NULL; size_t extra;
	size_t bytes = make_request(request, true);
	uint32_t before = register_writes, starts = sampler_starts;
	request[32] ^= 1;
	assert(iiod_buffer_metadata_open(rx_device, fixture_rate / 50, &mask, 1, 8,
		request, bytes, &context, &extra, &plan) == -ESTALE);
	assert(!context && !plan.drain_metadata);
	request[32] ^= 1; request[28] = 1;
	assert(iiod_buffer_metadata_open(rx_device, fixture_rate / 50, &mask, 1, 8,
		request, bytes, &context, &extra, &plan) == -EINVAL);
	assert(register_writes == before && sampler_starts == starts);
}

static void test_cancel_cannot_overtake_reserved_carrier(void)
{
	uint8_t request[4096], output[65536];
	leo_scanner_glrt_request_v1 decoded;
	struct spf_hop_request_v1 hop;
	struct spf_scanner_glrt *state = NULL;
	size_t bytes = make_request(request, true);
	assert(leo_scanner_glrt_request_decode(&decoded, request, bytes) == 0);
	assert(spf_hop_request_v1_decode(&hop, decoded.legacy_request + 104, SPF_HOP_REQUEST_BYTES) == 0);
	assert(spf_scanner_glrt_open(&state, &decoded, &hop, fixture_rate / 50) == 0);
	spf_scanner_glrt_begin_frame(state);
	spf_scanner_glrt_finish(state, 1);
	assert(spf_scanner_glrt_drain(state, output, sizeof(output)) == -EBUSY);
	assert(spf_scanner_glrt_frame(state, "opaque", 6, output, sizeof(output)) > 0);
	unsigned int final = 0;
	for (unsigned int j = 0; j < 2500; j++) {
		ssize_t result = spf_scanner_glrt_drain(state, output, sizeof(output));
		if (result == -EAGAIN) { struct timespec pause = {0, 2000000}; nanosleep(&pause, NULL); continue; }
		assert(result > 0 && (read32(output + 20) & 3) == 3);
		final = 1; break;
	}
	assert(final);
	spf_scanner_glrt_close(state);
}

static unsigned int consume_results(const uint8_t *packet)
{
	unsigned int j, count = packet[16] | (unsigned int)packet[17] << 8;
	const uint8_t *record = packet + 128 + read32(packet + 12);
	assert(count <= 4 && read64(packet + 24) == 77 && read64(packet + 32) == 9);
	for (j = 0; j < count; j++, record += 144) {
		assert(read64(record) == 0 && read64(record + 8) == 0);
		assert(read64(record + 16) == fixture_first + 71);
		assert(read64(record + 24) == fixture_first + 71 + fixture_rate / 50 * 6);
		assert(read32(record + 64) == fixture_rate && record[68] == 1 && record[69] == 0);
		assert(record[70] == 1 && record[71] == 0); /* RX1, unavailable */
		assert(read32(record + 72) == (inject_failure ? 2U : 5U)); /* failed or unqualified */
		assert(read32(record + 76) == (inject_failure ? 0U : 63U));
	}
	return count;
}

static void test_provider(bool enabled, unsigned int delay)
{
	uint8_t request[4096], output[65536], scratch[65536];
	uint32_t mask = 15;
	size_t extra, offset, iq_bytes, request_bytes = make_request(request, enabled);
	struct iiod_buffer_burst_plan plan;
	void *context = NULL;
	event_delay = delay;
	assert(iiod_buffer_metadata_open(rx_device, fixture_rate / 50, &mask, 1, 8,
		request, request_bytes, &context, &extra, &plan) == 0);
	struct spf_iiod_metadata_context *state = context;
	assert(extra == 1 && !!state->glrt == enabled && !!plan.drain_metadata == enabled);
	state->stream_id = 100; /* deterministic fixture identity */
	assert(spf_hop_session_v1_start(&state->hop_session) == 0);
	if (enabled) assert(plan.drain_metadata(context, output, sizeof(output)) == -EBUSY);
	uint8_t *raw = malloc(state->layout.raw_bytes), *original = malloc(state->layout.raw_bytes);
	assert(raw && original);
	memset(raw, 0, state->layout.raw_bytes);
	for (size_t j = 0; j < state->samples_per_channel; j++)
		((int16_t *)(raw + 8))[j * 4] = 30000; /* RX0 never reaches GLRT. */
	unsigned int records = 0, frame;
	for (frame = 0; frame < 7; frame++) {
		uint64_t first = fixture_first + frame * (uint64_t)state->samples_per_channel;
		memcpy(raw, &first, 8); memcpy(original, raw, state->layout.raw_bytes);
		if (enabled && !frame) {
			assert(iiod_buffer_metadata_get(context, rx_device, (void *)raw,
				state->layout.raw_bytes, output, plan.metadata_capacity - 1, &offset, &iq_bytes) == -ENOSPC);
			assert(state->frames_emitted == 0 && drain_calls == 0);
		}
		ssize_t bytes = iiod_buffer_metadata_get(context, rx_device, (void *)raw,
			state->layout.raw_bytes, output, sizeof(output), &offset, &iq_bytes);
		assert(bytes > 0 && offset == 8 && iq_bytes == state->layout.iq_bytes);
		assert(!memcmp(raw, original, state->layout.raw_bytes));
		const uint8_t *legacy = output; size_t legacy_bytes = (size_t)bytes;
		if (enabled) {
			assert(leo_scanner_glrt_legacy_view(output, (size_t)bytes, &legacy, &legacy_bytes) == 0);
			assert(read64(output + 40) == frame);
			records += consume_results(output);
		}
		const spf_radio_meta_v3_prefix_t *prefix = (const void *)legacy;
		if (!enabled) {
			memcpy(legacy_baseline[frame], legacy, legacy_bytes);
			legacy_baseline_bytes[frame] = legacy_bytes;
		} else if (!delay) {
			assert(legacy_bytes == legacy_baseline_bytes[frame]);
			assert(!memcmp(legacy, legacy_baseline[frame], legacy_bytes));
		} else {
			/* Delayed HOPS delivery changes the sidecar, not base metadata. */
			assert(!memcmp(legacy, legacy_baseline[frame], prefix->header_bytes));
		}
		assert(prefix->version == SPF_GAIN_META_VERSION_V6 && prefix->first_sample_sequence == first);
		assert(prefix->buffer_sequence == frame && prefix->enabled_scan_mask == 15);
		struct spf_hop_sidecar_v1 hop;
		assert(spf_hop_sidecar_v1_decode(&hop, legacy + prefix->header_bytes,
			legacy_bytes - prefix->header_bytes) == 0);
		assert(hop.event_count == (frame == delay));
		struct iiod_buffer_metadata_frame_info info;
		assert(iiod_buffer_metadata_describe_frame(context, output, (size_t)bytes, &info) == 0);
		assert(info.first_sample_sequence == first && info.missing_samples_before == 0);
		memcpy(scratch, output, (size_t)bytes);
		assert(iiod_buffer_metadata_rebase_frame(context, scratch, (size_t)bytes, first - 5) == 0);
		assert(iiod_buffer_metadata_describe_frame(context, scratch, (size_t)bytes, &info) == 0);
		assert(info.missing_samples_before == 5);
		/* Rebase may modify the inner CRC/gap, never HOPS or GLRT records. */
		size_t base = enabled ? 128 : 0;
		assert(!memcmp(output + base + prefix->header_bytes, scratch + base + prefix->header_bytes,
			(size_t)bytes - base - prefix->header_bytes));
		if (enabled) assert(!memcmp(output, scratch, 128));
		if (enabled && inject_failure && !frame)
			spf_scanner_glrt_feed(state->glrt, NULL, NULL, 0);
	}
	assert(restores == 1 && state->hop_session.status.state == SPF_HOP_STATE_COMPLETED);
	if (enabled) {
		unsigned int attempt, final = 0;
		for (attempt = 0; attempt < 2500; attempt++) {
			ssize_t bytes = plan.drain_metadata(context, output, sizeof(output));
			if (bytes == -EAGAIN) { struct timespec pause = {0, 2000000}; nanosleep(&pause, NULL); continue; }
			assert(bytes > 0 && read32(output + 12) == 0 && (read32(output + 20) & 1));
			records += consume_results(output);
			if (read32(output + 20) & 2) { final = 1; break; }
		}
		assert(final && records == 1 && read64(output + 48) == 1);
		assert(plan.drain_metadata(context, output, sizeof(output)) == -ENODATA);
	}
	iiod_buffer_metadata_close(context);
	assert(restores == 1);
	free(raw); free(original);
}

int main(void)
{
	alarm(30);
	fixture_first = (UINT64_C(1) << 53) + 10000;
	for (fixture_rate = 2500000; fixture_rate <= 5000000; fixture_rate += 2500000) {
		test_rejected_open_is_side_effect_free();
		test_cancel_cannot_overtake_reserved_carrier();
		test_provider(false, 0);
		test_provider(true, 0);
		test_provider(true, 2);
		inject_failure = true;
		test_provider(true, 0);
		inject_failure = false;
	}
	puts("SPF GLRT provider: both rates, real worker, delayed events, terminal drain and exact-gap tests passed");
	return 0;
}
