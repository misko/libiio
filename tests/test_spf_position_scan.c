/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "spf-scan-session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Exercise the public session/adapter interfaces. The mock behaves like the
 * owner UAPI, including immutable original CRCs and mutable post-ALC CRCs. */
static uint64_t now, generations[8];
static uint32_t mappings[8];
static struct adi_rx_counter_bank_config bank;
static unsigned fault, acquired, released, calls;
static uint8_t iq[600000 * 8];
static const char *fixture_directory;

static void fixture(const char *name, const void *wire, size_t bytes)
{
	char path[1024];
	FILE *file;
	if (!fixture_directory) return;
	assert(snprintf(path,sizeof(path),"%s/%s.bin",fixture_directory,name)<(int)sizeof(path));
	file=fopen(path,"wb"); assert(file);
	assert(fwrite(wire,1,bytes,file)==bytes); assert(!fclose(file));
}

static uint32_t crc32(const uint8_t *p, size_t bytes)
{
	uint32_t crc = UINT32_MAX;
	for (size_t i = 0; i < bytes; i++) {
		crc ^= p[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1));
	}
	return ~crc;
}

static int mock_ioctl(int fd, unsigned long request, void *argument)
{
	assert(fd == 31);
	if (request == ADI_RX_COUNTER_IOC_DIAG_CONTEXT || request == ADI_RX_COUNTER_IOC_GET_DIAGNOSTICS) {
		errno = ENOTTY; return -1;
	}
	if (request == ADI_RX_COUNTER_IOC_GET_SCAN_CAPS) {
		struct adi_rx_counter_scan_caps *c = argument;
		*c = (struct adi_rx_counter_scan_caps){.magic=ADI_RX_COUNTER_MAGIC,
			.version=1,.size=sizeof(*c),.features=ADI_RX_COUNTER_SCAN_FEATURES,
			.maximum_profiles=8,.source_counter_bits=32,.frequency_resolution_hz=2};
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_GET_BANK_CAPS) {
		struct adi_rx_counter_bank_caps *c = argument;
		if (fault == 1) { errno = ENOTTY; return -1; }
		*c = (struct adi_rx_counter_bank_caps){.magic=ADI_RX_COUNTER_MAGIC,
			.version=1,.size=sizeof(*c),.features=ADI_RX_COUNTER_BANK_FEATURES,
			.maximum_targets=24,.maximum_profiles=8,.profile_bytes=16,.source_counter_bits=32};
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_ACQUIRE) { acquired++; return 0; }
	if (request == ADI_RX_COUNTER_IOC_CONFIGURE_BANK) {
		bank = *(struct adi_rx_counter_bank_config *)argument;
		assert(bank.target_count == 24 && bank.size == sizeof(bank));
		for (unsigned i=0;i<24;i++) assert(bank.profiles[i].crc32 == crc32(bank.profiles[i].values,16));
		for (unsigned i=0;i<8;i++) { generations[i]=1; mappings[i]=i; }
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_REPLACE_RECALL) {
		struct adi_rx_counter_bank_recall *r = argument;
		uint32_t t = r->logical_target, p = r->profile;
		calls++;
		assert(t<24 && p==t%8 && r->expected_generation==generations[p]);
		assert(r->requested_frequency_hz==bank.profiles[t].frequency_hz);
		if (fault==2 || fault==3 || fault==7) {
			errno = fault==2 ? EIO : fault==3 ? ESTALE : ETIMEDOUT; return -1;
		}
		if (mappings[p]!=t) { generations[p]++; mappings[p]=t; }
		r->slot_generation=generations[p];
		r->frequency_hz=bank.profiles[t].frequency_hz + (fault==4 ? 1000000 : 0);
		r->profile_identity_crc32=bank.profiles[t].crc32 ^ (fault==5 ? 1 : 0);
		r->profile_crc32=bank.profiles[t].crc32 ^ UINT32_C(0x1234);
		if (fault==6) r->slot_generation++;
		r->counter_before=(uint32_t)(now+100);
		r->load_after=(uint32_t)(now+400);
		r->recall_before=(uint32_t)(now+500);
		r->counter_after=(uint32_t)(now+1000);
		return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_SCAN_SNAPSHOT) {
		((struct adi_rx_counter_scan_snapshot *)argument)->counter=(uint32_t)now; return 0;
	}
	if (request == ADI_RX_COUNTER_IOC_RELEASE_SCAN) {
		struct adi_rx_counter_scan_release *r = argument;
		r->frequency_hz=915000000; r->counter_before=(uint32_t)(now+10);
		r->counter_after=(uint32_t)(now+20); released++; return 0;
	}
	assert(!"unexpected ioctl"); return -1;
}

static int release_block(void *ctx, uintptr_t token)
{
	(void)ctx; assert(token); return 0;
}

static struct spf_scan_setup setup(void)
{
	struct spf_scan_setup s = {.protocol_version=6,.session=1,.generation=2,.seed=3,
		.source_rate_hz=10000000,.analog_bandwidth_hz=8000000,.duration_ms=0,
		.dwell_ms=20,.transition_budget_ms=20,.maximum_revisit_ms=3000,
		.feedback_age_ms=1000,.application_delay_ms=1000,.decay_ms=5000,.maximum_boost=1,
		.maximum_queue_bytes=200000000,.maximum_queue_age_ms=10000,.maximum_queue_visits=64,
		.target_count=24,.rx_mask=3,.format=1,.flags=1};
	memset(s.analysis_digest,0x47,32);
	for (unsigned i=0;i<24;i++) {
		s.targets[i]=(struct spf_scan_target){.channel=i,.profile=i%8,
			.frequency_hz=1081328125U+i*15000000U,.baseline_weight=1};
		for (unsigned j=0;j<16;j++) s.targets[i].profile_values[j]=(uint8_t)(i*17+j);
		s.targets[i].profile_crc32=crc32(s.targets[i].profile_values,16);
	}
	return s;
}

static struct spf_scan_session_runtime runtime(void)
{
	return (struct spf_scan_session_runtime){.block_count=64,.headroom_blocks=2,
		.block_samples=600000,.bytes_per_sample=8,.drain_bytes_per_second=1000000000,
		.release_block=release_block};
}

static void protocol(void)
{
	struct spf_scan_setup s=setup(), decoded;
	struct spf_scan_caps caps, caps_out;
	uint8_t wire[SPF_SCAN_POSITION_SETUP_BYTES];
	assert(!spf_scan_setup_encode(wire,sizeof(wire),&s));
	fixture("setup6",wire,1120);
	assert(wire[4]==6 && wire[6]==0x60 && wire[7]==4);
	assert(!spf_scan_setup_decode(&decoded,wire,sizeof(wire)));
	assert(!memcmp(&s,&decoded,sizeof(s)));
	for (unsigned i=0;i<sizeof(wire);i++) {
		wire[i]^=1; assert(spf_scan_setup_decode(&decoded,wire,sizeof(wire))); wire[i]^=1;
	}
	s.targets[8].profile_values[3]^=1; assert(spf_scan_setup_validate(&s));
	s=setup(); s.targets[8].profile=1; assert(spf_scan_setup_validate(&s));
	s=setup(); s.target_count=8; assert(spf_scan_setup_validate(&s));
	s=setup(); s.protocol_version=5; assert(spf_scan_setup_validate(&s));
	spf_scan_caps_default(&caps); caps.protocol_version=6; caps.rate_mask=SPF_SCAN_RATE_10M;
	caps.maximum_targets=24; caps.minimum_dwell_ms=caps.maximum_dwell_ms=20; caps.maximum_duration_ms=0;
	assert(!spf_scan_caps_encode(wire,sizeof(wire),&caps));
	fixture("caps6",wire,96);
	assert(!spf_scan_caps_decode(&caps_out,wire,96));
	assert(caps_out.maximum_targets==24 && caps_out.maximum_fastlock_profiles==8);
}

static void check_output(struct spf_scan_session *session, uint64_t visit)
{
	struct spf_scan_session_output out;
	struct spf_scan_visit_record decoded;
	uint8_t wire[SPF_SCAN_POSITION_VISIT_BYTES];
	assert(!spf_scan_session_take_output(session,&out));
	assert(out.record.visit==visit && out.record.target==visit%24 && out.record.profile==visit%8);
	assert(out.record.slot_generation==1+(visit>=8 ? (visit-8)/8+1 : 0));
	assert(out.record.profile_identity_crc32==bank.profiles[visit%24].crc32);
	assert(out.record.profile_crc32==(out.record.profile_identity_crc32^UINT32_C(0x1234)));
	assert(out.record.frequency_hz==out.record.verified_frequency_hz);
	assert(out.record.valid_end-out.record.valid_start==200000 && out.record.iq_bytes==1600000);
	assert(out.record.valid_start-out.record.transition_after>=200000);
	assert(out.record.valid_start-out.record.selection_counter>=200000);
	assert(out.record.load_after-out.record.transition_before==300);
	assert(out.record.transition_after-out.record.recall_before==500);
	assert(!spf_scan_visit_encode(wire,sizeof(wire),&out.record));
	if (visit==23) fixture("visit6",wire,224);
	assert(!spf_scan_visit_decode(&decoded,wire,sizeof(wire)));
	assert(decoded.sweep==visit/24 && decoded.rx_mask==3 && decoded.analysis_digest[0]==0x47);
	if (visit==0) {
		/* A delayed reader sees the acquired generation although slot0 has
		 * already held logical8,16 and0 again. */
		assert(generations[0]>=4 && out.record.slot_generation==1);
		for (unsigned i=0;i<sizeof(wire);i++) {
			wire[i]^=1; assert(spf_scan_visit_decode(&decoded,wire,sizeof(wire))); wire[i]^=1;
		}
		out.record.target=1; assert(spf_scan_visit_encode(wire,sizeof(wire),&out.record));
	}
	assert(!spf_scan_session_complete_output(session,visit));
}

static void wrap_and_delayed_readers(void)
{
	struct spf_scan_setup s=setup();
	struct spf_scan_session_runtime r=runtime();
	struct spf_scan_session *session;
	struct spf_scan_choice choice;
	struct spf_scan_radio radio;
	struct spf_scan_terminal terminal, decoded;
	struct spf_scan_control query={.request=9,.session=1,.generation=2,.flags=1,.protocol_version=6};
	struct spf_scan_status status;
	uint8_t wire[128];
	uint64_t next=0;
	fault=0; now=UINT64_C(0xffff0000); calls=0;
	assert(!spf_scan_radio_init(&radio,31,mock_ioctl));
	assert(!spf_scan_session_create(&session,&s,&r,&radio,now));
	assert(!spf_scan_session_schedule(session,now,now,&choice));
	for (uint64_t visit=0;visit<10000;visit++) {
		assert(choice.visit==visit && choice.target==visit%24);
		assert(!spf_scan_session_feed(session,(uintptr_t)(visit+1),iq,now,600000));
		now+=600000;
		if (visit==9999) assert(!spf_scan_session_stop(session,now));
		else assert(!spf_scan_session_schedule(session,now,now,&choice));
		if (visit>=24) while (next<=visit) check_output(session,next++);
		assert(!spf_scan_session_status(session,&query,&status));
		assert(status.sweep==status.planned/24);
		if (visit==24) {
			assert(!spf_scan_status_encode(wire,sizeof(wire),&status));
			fixture("status6",wire,128);
			assert(!spf_scan_control_encode(wire,sizeof(wire),&query));
			fixture("control6",wire,48);
		}
	}
	assert(calls==10000);
	assert(!spf_scan_session_terminal(session,&terminal));
	assert(terminal.protocol_version==6 && terminal.planned==10000 && terminal.delivered==10000);
	assert(terminal.iq_bytes==UINT64_C(16000000000) && !terminal.invalid && !terminal.cancelled);
	assert(!spf_scan_terminal_encode(wire,sizeof(wire),&terminal) && wire[4]==6);
	fixture("terminal6",wire,128);
	assert(!spf_scan_terminal_decode(&decoded,wire,sizeof(wire)) && decoded.protocol_version==6);
	assert(!spf_scan_session_destroy(session));
}

static void failures_and_cleanup(void)
{
	for (unsigned f=1;f<=7;f++) {
		struct spf_scan_setup s=setup();
		struct spf_scan_session_runtime r=runtime();
		struct spf_scan_session *session=NULL;
		struct spf_scan_radio radio;
		struct spf_scan_choice choice;
		struct spf_scan_session_output out;
		struct spf_scan_terminal terminal;
		uint8_t wire[224];
		unsigned before=acquired, restored=released;
		now=UINT64_C(0x1ffff0000); fault=f;
		assert(!spf_scan_radio_init(&radio,31,mock_ioctl));
		if (f==1) {
			assert(spf_scan_session_create(&session,&s,&r,&radio,now)==-ENOTTY);
			assert(acquired==before && released==restored); continue;
		}
		assert(!spf_scan_session_create(&session,&s,&r,&radio,now));
		assert(spf_scan_session_schedule(session,now,now,&choice)<0);
		assert(released==restored+1 && radio.slot_generation[0]==1);
		assert(!spf_scan_session_take_output(session,&out));
		assert(out.record.result==SPF_VISIT_CANCELLED && !out.record.iq_bytes);
		assert(!spf_scan_visit_encode(wire,sizeof(wire),&out.record));
		assert(!spf_scan_session_complete_output(session,0));
		assert(!spf_scan_session_terminal(session,&terminal));
		assert(terminal.planned==1 && terminal.cancelled==1 && terminal.error<0 && terminal.flags==1);
		assert(!spf_scan_session_destroy(session));
	}
	fault=0;
}

static void cancellation_and_pressure(void)
{
	for (unsigned mode=0;mode<3;mode++) {
		struct spf_scan_setup s=setup();
		struct spf_scan_session_runtime r=runtime();
		struct spf_scan_session *session;
		struct spf_scan_radio radio;
		struct spf_scan_choice choice;
		struct spf_scan_session_output out;
		struct spf_scan_terminal terminal;
		uint8_t wire[224];
		now=UINT64_C(0x1ffff0000); fault=0;
		if (mode==2) s.maximum_queue_visits=1;
		if (mode==1) r.block_samples=100000;
		assert(!spf_scan_radio_init(&radio,31,mock_ioctl));
		assert(!spf_scan_session_create(&session,&s,&r,&radio,now));
		assert(!spf_scan_session_schedule(session,now,now,&choice));
		if (mode==0) {
			/* Cancellation during the guard retains an explicit empty visit. */
			now+=100000;
			assert(!spf_scan_session_cancel(session,now));
		} else if (mode==1) {
			/* Cancellation after half a valid dwell preserves acquired IQ. */
			for (unsigned i=0;i<3;i++)
				assert(!spf_scan_session_feed(session,i+1,iq,now+i*100000,100000));
			now+=300000;
			assert(!spf_scan_session_cancel(session,now));
		} else {
			assert(!spf_scan_session_feed(session,1,iq,now,600000));
			now+=600000;
			assert(spf_scan_session_schedule(session,now,now,&choice)==-ENOSPC);
		}
		assert(!spf_scan_session_take_output(session,&out));
		assert(out.record.visit==0);
		assert(out.record.iq_bytes==(mode==0 ? 0U : mode==1 ? 792000U : 1600000U));
		assert(!spf_scan_visit_encode(wire,sizeof(wire),&out.record));
		assert(!spf_scan_session_complete_output(session,0));
		if (mode==2) {
			assert(!spf_scan_session_take_output(session,&out));
			assert(out.record.visit==1 && out.record.result==SPF_VISIT_SKIP_CAPACITY && !out.record.iq_bytes);
			assert(!spf_scan_visit_encode(wire,sizeof(wire),&out.record));
			assert(!spf_scan_session_complete_output(session,1));
		}
		assert(!spf_scan_session_terminal(session,&terminal));
		assert(terminal.flags==1 && terminal.planned==(mode==2 ? 2U : 1U));
		assert(terminal.state==(mode==2 ? SPF_SCAN_TERMINAL_FAILED : SPF_SCAN_TERMINAL_CANCELLED));
		assert(terminal.delivered==(mode==2 ? 1U : 0U) && terminal.skipped==(mode==2 ? 1U : 0U));
		assert(!spf_scan_session_destroy(session));
	}
}

int main(int argc, char **argv)
{
	if (argc==2) fixture_directory=argv[1];
	protocol(); wrap_and_delayed_readers(); failures_and_cleanup(); cancellation_and_pressure();
	puts("PASS: v6 profiles, 10000 ordered visits, wraps, delayed receipts, injected failures and restoration");
	return 0;
}
