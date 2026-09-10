/* Included by the actual provider fixture, with hardware-only substitutions.
 * This synthesizes recall receipts; the native policy, queue, collector and
 * numerical worker remain real. The threaded scheduler has separate tests. */
static struct {
	struct spf_hop_request_v2 request;
	const struct spf_hop_scheduler_policy_v2 *ports;
	void *policy;
	struct spf_hop_device_event_v2 events[2500];
	uint64_t next, block_end;
	unsigned count, drained;
	bool opened, started, stopped;
} adaptive_fixture;
static int16_t *adaptive_pilot[2];

static int adaptive_fixture_start(void *context, const struct spf_hop_request_v2 *r)
{
	assert(context == &adaptive_fixture && adaptive_fixture.opened);
	assert(r->policy.generation == adaptive_fixture.request.policy.generation);
	adaptive_fixture.started = true;
	return 0;
}
static int adaptive_fixture_events(void *context, struct spf_hop_device_event_v2 *out,
	size_t capacity, size_t *count, uint64_t *dropped)
{
	assert(context == &adaptive_fixture);
	size_t n = adaptive_fixture.count - adaptive_fixture.drained;
	if (n > capacity) n = capacity;
	memcpy(out, adaptive_fixture.events + adaptive_fixture.drained, n * sizeof(*out));
	adaptive_fixture.drained += (unsigned)n;
	*count = n; *dropped = 0;
	return 0;
}
static int adaptive_fixture_restore(void *context, uint16_t reason,
	struct spf_hop_restore_receipt_v1 *out)
{
	assert(context == &adaptive_fixture && reason && !adaptive_fixture.stopped);
	adaptive_fixture.stopped = true;
	*out = (struct spf_hop_restore_receipt_v1){
		.transition_before = adaptive_fixture.block_end,
		.transition_after = adaptive_fixture.block_end + 2,
		.restored_lo_frequency_hz = 900000000, .restored_profile = SPF_HOP_PROFILE_NONE,
		.flags = SPF_HOP_EVENT_FLAGS_V1,
	};
	++restores;
	return 0;
}
static const struct spf_hop_device_ops_v2 adaptive_fixture_ops = {
	adaptive_fixture_start, adaptive_fixture_events, adaptive_fixture_restore,
};

int fixture_hop_v2_open(const struct iio_device *rx, const struct iio_device *phy,
	struct spf_tandem_session *tandem, pthread_mutex_t *lock, const struct spf_hop_request_v2 *r,
	const struct spf_hop_scheduler_policy_v2 *ports, void *policy, void **context,
	const struct spf_hop_device_ops_v2 **ops)
{
	assert(rx == rx_device && phy == phy_device && tandem && lock && ports && policy);
	assert(!adaptive_fixture.opened);
	memset(&adaptive_fixture, 0, sizeof(adaptive_fixture));
	adaptive_fixture.request = *r;
	adaptive_fixture.ports = ports;
	adaptive_fixture.policy = policy;
	adaptive_fixture.next = fixture_first + 3;
	adaptive_fixture.block_end = fixture_first;
	adaptive_fixture.opened = true;
	fixture_request = r->geometry; restores = 0;
	*context = &adaptive_fixture; *ops = &adaptive_fixture_ops;
	return 0;
}
void fixture_hop_v2_destroy(void *context)
{
	assert(context == &adaptive_fixture && adaptive_fixture.opened);
	adaptive_fixture.opened = false;
}

static void fixture_adaptive_prepare(uint8_t *raw, uint64_t first, size_t samples)
{
	if (!adaptive_fixture.opened || !adaptive_fixture.started || adaptive_fixture.stopped) return;
	const struct spf_hop_request_v1 *r = &adaptive_fixture.request.geometry;
	uint64_t end = first + samples;
	adaptive_fixture.block_end = end;
	while (adaptive_fixture.count < r->dwell_count && adaptive_fixture.next < end &&
		(!adaptive_fixture.count || adaptive_fixture.next - (fixture_first + 3) < r->capture_span_samples)) {
		unsigned i = adaptive_fixture.count;
		struct spf_hop_device_event_v2 *e = &adaptive_fixture.events[i];
		assert(!adaptive_fixture.ports->choose(adaptive_fixture.policy, i, adaptive_fixture.next, &e->choice));
		unsigned target = adaptive_fixture.request.policy.mode == SPF_HOP_SHADOW ? i % 8 : e->choice.proposed_target;
		e->device = (struct spf_hop_device_event_v1){
			.event_sequence = i, .dwell_index = i, .transition_before = adaptive_fixture.next,
			.transition_after = adaptive_fixture.next + 4,
			.actual_lo_frequency_hz = r->profiles[target].lo_frequency_hz,
			.actual_if_offset_hz = r->if_offset_hz, .device_event_id = i + 1,
			.from_profile = i ? adaptive_fixture.events[i - 1].device.to_profile : SPF_HOP_PROFILE_NONE,
			.to_profile = target, .kind = i ? SPF_HOP_EVENT_RETUNE : SPF_HOP_EVENT_STARTUP,
			.flags = SPF_HOP_EVENT_FLAGS_V1, .fastlock_slot = target,
		};
		uint64_t valid = e->device.transition_after + r->transition_guard_samples;
		adaptive_fixture.next = valid + r->dwell_samples;
		assert(!adaptive_fixture.ports->commit(adaptive_fixture.policy, e, valid, adaptive_fixture.next));
		++adaptive_fixture.count;
	}
	int16_t *iq = (void *)(raw + 8);
	for (size_t i = 0; i < samples; ++i) { iq[4 * i + 2] = 0; iq[4 * i + 3] = 0; }
	for (unsigned i = 0; i < adaptive_fixture.count; ++i) {
		const struct spf_hop_device_event_v2 *e = &adaptive_fixture.events[i];
		uint64_t valid = e->device.transition_after + r->transition_guard_samples;
		uint64_t begin = first > valid ? first : valid;
		uint64_t stop = end < valid + r->dwell_samples ? end : valid + r->dwell_samples;
		const int16_t *pilot = adaptive_pilot[e->device.to_profile / 4];
		/* Only 1L, 3L and 4L carry the supplied synthetic pilot. Other targets
		 * are zeros, not fabricated positives or reassigned shadow samples. */
		if (!pilot || !(13 & (1U << e->device.to_profile))) continue;
		for (uint64_t sample = begin; sample < stop; ++sample) {
			size_t dst = (size_t)(sample - first), src = (size_t)(sample - valid);
			iq[4 * dst + 2] = pilot[2 * src]; iq[4 * dst + 3] = pilot[2 * src + 1];
		}
	}
}

#ifndef SPF_GLRT_NETWORK_FIXTURE
static size_t make_request(uint8_t *, bool);
static uint32_t read32(const uint8_t *);
static uint64_t read64(const uint8_t *);

static size_t adaptive_request_packet(uint8_t *packet, unsigned mode, int invalid)
{
	uint8_t original[4096], legacy[104 + SPF_HOP_ADAPTIVE_REQUEST_BYTES];
	leo_scanner_glrt_request_v1 glrt;
	struct spf_hop_request_v2 r = {0};
	size_t bytes = make_request(original, true);
	assert(!leo_scanner_glrt_request_decode(&glrt, original, bytes));
	memcpy(legacy, glrt.legacy_request, 104);
	assert(!spf_hop_request_v1_decode(&r.geometry, glrt.legacy_request + 104, SPF_HOP_REQUEST_BYTES));
	r.geometry.dwell_count = 64;
	r.geometry.capture_span_samples = fixture_rate * 4ULL;
	r.policy = (struct spf_hop_policy_v2){9, mode, 3, 3, 3, 1, 2000, 3000, 160, 1000, 3};
	if (invalid == 1) ++r.policy.generation;
	if (invalid == 2) ++r.policy.cooldown_ms;
	assert(!spf_hop_request_v2_encode(legacy + 104, SPF_HOP_ADAPTIVE_REQUEST_BYTES, &r));
	if (invalid == 3) { memcpy(packet, legacy, sizeof(legacy)); return sizeof(legacy); }
	glrt.legacy_request = legacy; glrt.legacy_bytes = sizeof(legacy);
	ssize_t result = leo_scanner_glrt_request_encode(&glrt, packet, 4096);
	assert(result > 0);
	return (size_t)result;
}

static unsigned adaptive_consume(const uint8_t *packet, unsigned *positives, unsigned *recovered)
{
	unsigned count = packet[16] | (unsigned)packet[17] << 8;
	const uint8_t *record = packet + 128 + read32(packet + 12);
	for (unsigned i = 0; i < count; ++i, record += 144) {
		uint64_t visit = read64(record + 8);
		assert(visit < adaptive_fixture.count);
		const struct spf_hop_device_event_v1 *e = &adaptive_fixture.events[visit].device;
		assert(read64(record + 16) == e->transition_after + fixture_request.transition_guard_samples);
		assert(read64(record + 24) - read64(record + 16) == fixture_request.dwell_samples);
		assert(record[68] == e->to_profile % 4 + 1 && record[69] == e->to_profile / 4 && record[70] == 1);
		assert(record[71] != 2); /* No public NO_SIGNAL claim. */
		if (record[71] == 1) { assert(13 & (1U << e->to_profile)); ++*positives; }
		if (visit >= 18 && read32(record + 76) == 63) ++*recovered;
	}
	return count;
}

static void test_adaptive_provider(unsigned mode, bool cancelled, bool failure, bool pressure)
{
	bool expect_fallback = failure;
#ifndef IIOD_SCANNER_GLRT_COOPERATIVE_SKIPS
	expect_fallback = expect_fallback || pressure;
#endif
#ifndef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
	assert(!pressure);
#endif
	uint8_t request[4096], output[65536]; uint32_t mask = 15;
	struct iiod_buffer_burst_plan plan;
	void *context = NULL;
	size_t extra, offset, iq_bytes;
	for (int invalid = 1; invalid <= 3; ++invalid) {
		size_t bytes = adaptive_request_packet(request, mode, invalid);
		unsigned before = register_writes, starts = sampler_starts;
		assert(iiod_buffer_metadata_open(rx_device, fixture_rate / 50, &mask, 1, 8,
			request, bytes, &context, &extra, &plan) == (invalid == 1 ? -ESTALE : -ENOTSUP));
		assert(!context && before == register_writes && starts == sampler_starts);
	}
	for (unsigned edge = 0; edge < 2; ++edge) {
		char variable[96];
		snprintf(variable, sizeof(variable), "SPF_ADAPTIVE_PILOT_%u_%s", fixture_rate, edge ? "UPPER" : "LOWER");
		const char *path = getenv(variable);
		assert(path && "supply explicit synthetic CI16 pilot fixtures, never a radio");
		FILE *file = fopen(path, "rb"); assert(file);
		size_t count = fixture_rate * 120ULL / 1000 * 2;
		adaptive_pilot[edge] = malloc(count * sizeof(int16_t)); assert(adaptive_pilot[edge]);
		assert(fread(adaptive_pilot[edge], sizeof(int16_t), count, file) == count);
		assert(fgetc(file) == EOF && !fclose(file));
	}
	size_t bytes = adaptive_request_packet(request, mode, 0);
	assert(!iiod_buffer_metadata_open(rx_device, fixture_rate / 50, &mask, 1, 8,
		request, bytes, &context, &extra, &plan));
	struct spf_iiod_metadata_context *state = context;
	assert(state->hop_adaptive && state->policy && state->glrt && plan.drain_metadata);
	assert(!spf_hop_session_v2_start(&state->adaptive_session));
	uint8_t *raw = calloc(1, state->layout.raw_bytes); assert(raw);
	for (size_t sample = 0; sample < state->samples_per_channel; ++sample)
		((int16_t *)(raw + 8))[sample * 4] = 30000;
	unsigned records = 0, positives = 0, weighted = 0, based = 0, fallback = 0, recovered = 0, frames;
	for (frames = 0; frames < 240; ++frames) {
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
		/* Exercise the real owner pressure port after initial activity. Keep
		 * acquisition moving through >3 visits, then allow normal recovery. */
		if (pressure && frames >= 60 && frames < 96)
			spf_scanner_glrt_capture_budget(state->glrt, 16000000, 0);
#endif
		uint64_t first = fixture_first + frames * (uint64_t)state->samples_per_channel;
		memcpy(raw, &first, 8);
		ssize_t result = iiod_buffer_metadata_get(context, rx_device, (void *)raw,
			state->layout.raw_bytes, output, sizeof(output), &offset, &iq_bytes);
		assert(result > 0 && offset == 8 && iq_bytes == state->layout.iq_bytes);
		for (size_t sample = 0; sample < state->samples_per_channel; ++sample)
			assert(((int16_t *)(raw + 8))[sample * 4] == 30000);
		records += adaptive_consume(output, &positives, &recovered);
		const uint8_t *legacy; size_t legacy_bytes;
		assert(!leo_scanner_glrt_legacy_view(output, (size_t)result, &legacy, &legacy_bytes));
		const spf_radio_meta_v3_prefix_t *prefix = (const void *)legacy;
		struct spf_hop_sidecar_v2 hop;
		assert(!spf_hop_sidecar_v2_decode(&hop, legacy + prefix->header_bytes, legacy_bytes - prefix->header_bytes));
		for (unsigned i = 0; i < hop.geometry.event_count; ++i) {
			weighted += hop.choices[i].reason == SPF_HOP_CHOICE_WEIGHTED;
			based += hop.choices[i].basis_visit != UINT64_MAX;
			fallback += hop.choices[i].reason == SPF_HOP_CHOICE_FAULT_FALLBACK;
			if (!expect_fallback) assert(hop.choices[i].reason != SPF_HOP_CHOICE_FAULT_FALLBACK);
#ifdef IIOD_SCANNER_GLRT_COOPERATIVE_SKIPS
			if (pressure && frames <= 130)
				assert(hop.choices[i].reason != SPF_HOP_CHOICE_FAULT_FALLBACK);
#endif
		}
		struct iiod_buffer_metadata_frame_info info;
		assert(!iiod_buffer_metadata_describe_frame(context, output, (size_t)result, &info));
		assert(info.first_sample_sequence == first && !info.missing_samples_before);
		if (failure && frames == (pressure ? 130U : 9U))
			spf_scanner_glrt_feed(state->glrt, NULL, NULL, 0);
		if (cancelled && frames == 9) { assert(!iiod_buffer_metadata_cancel(context)); break; }
		if (hop.geometry.state == SPF_HOP_STATE_COMPLETED) break;
		/* Real-time synthetic producer pacing, no RF. Detector completion is
		 * asynchronous; source time never waits on an individual GLRT result. */
		struct timespec pause = {0, 20000000}; nanosleep(&pause, NULL);
	}
	assert(frames < 240 && restores == 1);
	bool final = false;
	for (unsigned attempt = 0; attempt < 2500; ++attempt) {
		ssize_t result = plan.drain_metadata(context, output, sizeof(output));
		if (result == -EAGAIN) { struct timespec pause = {0, 2000000}; nanosleep(&pause, NULL); continue; }
		assert(result > 0);
		records += adaptive_consume(output, &positives, &recovered);
		if (read32(output + 20) & 2) { final = true; break; }
	}
	assert(final && records == adaptive_fixture.count);
	assert(plan.drain_metadata(context, output, sizeof(output)) == -ENODATA);
	assert(iiod_buffer_metadata_status(context, output, sizeof(output)) == SPF_HOP_STATUS_BYTES);
	struct spf_hop_status_v1 status;
	assert(!spf_hop_status_v2_decode(&status, output, SPF_HOP_STATUS_BYTES));
	assert(status.state == (cancelled ? SPF_HOP_STATE_CANCELLED : SPF_HOP_STATE_COMPLETED));
	if (!cancelled && !failure) {
		assert(positives && based);
		/* The legacy policy intentionally latches a pressure fault before
		 * weighted scheduling begins. Only healthy policy runs require it. */
		if (!expect_fallback) assert(weighted);
	}
	if (expect_fallback) {
		assert(fallback && status.state == SPF_HOP_STATE_COMPLETED);
		bool seen = false;
		for (unsigned i = 0; i < adaptive_fixture.count; ++i) {
			const struct spf_hop_device_event_v2 *e = &adaptive_fixture.events[i];
			if (e->choice.reason == SPF_HOP_CHOICE_FAULT_FALLBACK) seen = true;
			if (seen) assert(e->choice.reason == SPF_HOP_CHOICE_FAULT_FALLBACK && e->device.to_profile == i % 8);
		}
	}
#ifdef IIOD_SCANNER_GLRT_CAPTURE_PROTECTION
	if (pressure) {
		leo_scanner_glrt_protection_stats_v1 stats;
		assert(!spf_scanner_glrt_protection_stats(state->glrt, &stats));
		assert(stats.pressure_skips >= 3 && stats.resumptions >= 1);
		assert(stats.history_blocks_skipped >= 36 && stats.disabled == failure);
		assert(recovered); /* Actual numerical checks resumed after pressure. */
	}
#endif
	iiod_buffer_metadata_close(context);
	assert(restores == 1 && !adaptive_fixture.opened);
	free(raw);
	for (unsigned edge = 0; edge < 2; ++edge) { free(adaptive_pilot[edge]); adaptive_pilot[edge] = NULL; }
	printf("adaptive provider rate=%u mode=%u cancelled=%u failure=%u pressure=%u visits=%u positives=%u weighted=%u based=%u fallback=%u recovered=%u PASS\n",
		fixture_rate, mode, cancelled, failure, pressure, records, positives, weighted, based, fallback, recovered); fflush(stdout);
}
#endif
