// SPDX-License-Identifier: LGPL-2.1-or-later
/* Actual parser/handler/client with an XML-only device and an already-open
 * synthetic session. No local context, receive buffer or radio is opened.
 * Including the owned implementation permits pinning its private session list
 * without introducing a production test-only public API. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../iiod/ops.c"
#include "metadata-drain-client.h"

bool server_demux;
struct thread_pool *main_thread_pool;

struct fixture {
	struct parser_pdata parser;
	struct DevEntry entry;
	struct ThdEntry thread;
	struct drain_test_client *client;
	const char *input;
	size_t input_bytes, input_offset;
	uint8_t output[1024];
	size_t output_bytes, output_offset;
	unsigned int calls, produced;
	bool pending, active;
	ssize_t invalid_size;
	unsigned feedback_calls;
	int feedback_error;
};

static int provider_feedback(void *opaque, const void *bytes, size_t count)
{
	struct fixture *f=opaque;
	const uint8_t *p=bytes;
	assert(count==160);
	for (unsigned i=0;i<160;++i) assert(p[i]==(uint8_t)i);
	++f->feedback_calls;
	return f->feedback_error;
}

static ssize_t provider_drain(void *opaque, void *output, size_t capacity)
{
	struct fixture *fixture = opaque;
	static const char *const records[] = {"late-dwell-result", "FINAL"};
	size_t bytes;

	fixture->calls++;
	if (fixture->invalid_size)
		return fixture->invalid_size;
	if (fixture->active)
		return -EBUSY;
	if (fixture->pending)
		return -EAGAIN;
	if (fixture->produced == 2)
		return -ENODATA;
	bytes = strlen(records[fixture->produced]);
	if (capacity < bytes)
		return -ENOSPC;
	memcpy(output, records[fixture->produced++], bytes);
	return (ssize_t)bytes;
}

static ssize_t server_read(struct parser_pdata *parser, void *output, size_t bytes)
{
	struct fixture *fixture = (void *)parser;
	if (bytes > fixture->input_bytes - fixture->input_offset)
		return -EPIPE;
	memcpy(output, fixture->input + fixture->input_offset, bytes);
	fixture->input_offset += bytes;
	return (ssize_t)bytes;
}

static ssize_t server_write(struct parser_pdata *parser, const void *input,
		size_t bytes)
{
	struct fixture *fixture = (void *)parser;
	assert(fixture->output_bytes + bytes <= sizeof(fixture->output));
	memcpy(fixture->output + fixture->output_bytes, input, bytes);
	fixture->output_bytes += bytes;
	return (ssize_t)bytes;
}

static ssize_t client_write(void *opaque, const char *input, size_t bytes)
{
	struct fixture *fixture = (void *)opaque;
	yyscan_t scanner;
	int ret;
	assert(fixture->output_bytes == fixture->output_offset);
	fixture->output_bytes = fixture->output_offset = 0;
	fixture->input = input;
	fixture->input_bytes = bytes;
	fixture->input_offset = 0;
	assert(yylex_init_extra(&fixture->parser, &scanner) == 0);
	ret = yyparse(scanner);
	assert(ret == 0 || ret == 1); /* Error replies are valid protocol records. */
	yylex_destroy(scanner);
	assert(fixture->input_offset == fixture->input_bytes);
	return (ssize_t)bytes;
}

static ssize_t client_read(void *opaque, char *output, size_t bytes)
{
	struct fixture *fixture = (void *)opaque;
	if (bytes > fixture->output_bytes - fixture->output_offset)
		return -EPIPE;
	memcpy(output, fixture->output + fixture->output_offset, bytes);
	fixture->output_offset += bytes;
	return (ssize_t)bytes;
}

static ssize_t drain(struct fixture *fixture, char *result, size_t capacity)
{
	bool stream_valid;
	ssize_t ret = drain_test_client_run(fixture->client, fixture->entry.dev,
		result, capacity, &stream_valid);
	assert(stream_valid);
	assert(fixture->output_offset == fixture->output_bytes);
	return ret;
}

int main(void)
{
	static const char xml[] = "<!DOCTYPE context ["
		"<!ELEMENT context (device*)><!ELEMENT device EMPTY>"
		"<!ATTLIST context name CDATA #REQUIRED description CDATA #IMPLIED>"
		"<!ATTLIST device id CDATA #REQUIRED>] >"
		"<context name=\"xml\" description=\"drain test\">"
		"<device id=\"dev0\"/><device id=\"dev1\"/></context>";
	static const char *const malformed[] = {
		"DRAINBUFM dev0 0\r\n", "DRAINBUFM dev0 -1\r\n",
		"DRAINBUFM dev0 65537\r\n", "DRAINBUFM dev0 4294967360\r\n",
		"DRAINBUFM dev0 999999999999999999999999999999999999999\r\n",
		"DRAINBUFM dev0 32x\r\n", "DRAINBUFM dev0 +32\r\n",
	};
	struct fixture fixture = {0};
	char result[64];
	unsigned int calls;

	alarm(15); /* No external watchdog utility is required on the ARM target. */
	fixture.parser.ctx = iio_create_xml_context_mem(xml, sizeof(xml) - 1U);
	assert(fixture.parser.ctx);
	fixture.entry.dev = iio_context_find_device(fixture.parser.ctx, "dev0");
	assert(fixture.entry.dev);
	fixture.entry.metadata_enabled = true;
	fixture.entry.metadata_provider_context = &fixture;
	fixture.entry.burst_plan.drain_metadata = provider_drain;
	assert(pthread_mutex_init(&fixture.entry.thdlist_lock, NULL) == 0);
	fixture.parser.readfd = server_read;
	fixture.parser.writefd = server_write;
	SLIST_INIT(&fixture.parser.thdlist_head);
	fixture.thread.dev = fixture.entry.dev;
	fixture.thread.entry = &fixture.entry;
	SLIST_INSERT_HEAD(&fixture.parser.thdlist_head, &fixture.thread, parser_list_entry);
	fixture.client = drain_test_client_new(&fixture, client_read, client_write);
	assert(fixture.client);

	fixture.active = true;
	assert(drain(&fixture, result, sizeof(result)) == -EBUSY);
	fixture.active = false;
	fixture.pending = true;
	assert(drain(&fixture, result, sizeof(result)) == -EAGAIN);
	fixture.pending = false;
	assert(drain(&fixture, result, 1) == -ENOSPC);
	assert(fixture.produced == 0);
	assert(drain(&fixture, result, sizeof(result)) == 17);
	assert(!memcmp(result, "late-dwell-result", 17));
	assert(drain(&fixture, result, sizeof(result)) == 5);
	assert(!memcmp(result, "FINAL", 5));
	assert(drain(&fixture, result, sizeof(result)) == -ENODATA);

	calls = fixture.calls;
	fixture.entry.burst_plan.drain_metadata = NULL; /* Legacy opt-out. */
	assert(drain(&fixture, result, sizeof(result)) == -ENODATA);
	fixture.entry.burst_plan.drain_metadata = provider_drain;
	fixture.entry.metadata_provider_context = NULL; /* Producer teardown. */
	assert(drain(&fixture, result, sizeof(result)) == -ENODATA);
	fixture.entry.metadata_provider_context = &fixture;
	fixture.entry.metadata_enabled = false;
	assert(drain(&fixture, result, sizeof(result)) == -ENODATA);
	fixture.entry.metadata_enabled = true;
	SLIST_REMOVE_HEAD(&fixture.parser.thdlist_head, parser_list_entry);
	assert(drain(&fixture, result, sizeof(result)) == -EBADF); /* Other owner. */
	SLIST_INSERT_HEAD(&fixture.parser.thdlist_head, &fixture.thread, parser_list_entry);
	assert(fixture.calls == calls);

	for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
		client_write(&fixture, malformed[i], strlen(malformed[i]));
		assert(fixture.output_bytes == 4);
		assert(!memcmp(fixture.output, "-22\n", 4));
		fixture.output_offset = fixture.output_bytes;
	}
	assert(fixture.calls == calls);
	fixture.invalid_size = 65;
	assert(drain(&fixture, result, sizeof(result)) == -EOVERFLOW);
	fixture.invalid_size = 0;
	assert(fixture.entry.buf == NULL); /* Never creates an acquisition buffer. */
	{
		uint8_t packet[160];
		bool valid;
		for (unsigned i=0;i<sizeof(packet);++i) packet[i]=(uint8_t)i;
		assert(feedback_test_client_run(fixture.client,fixture.entry.dev,packet,sizeof(packet),&valid)==-ENODATA);
		assert(valid && !fixture.feedback_calls);
		fixture.entry.burst_plan.submit_feedback=provider_feedback;
		assert(!feedback_test_client_run(fixture.client,fixture.entry.dev,packet,sizeof(packet),&valid));
		assert(valid && fixture.feedback_calls==1);
		fixture.feedback_error=-ESTALE;
		assert(feedback_test_client_run(fixture.client,fixture.entry.dev,packet,sizeof(packet),&valid)==-ESTALE);
		assert(valid && fixture.feedback_calls==2);
		SLIST_REMOVE_HEAD(&fixture.parser.thdlist_head,parser_list_entry);
		assert(feedback_test_client_run(fixture.client,fixture.entry.dev,packet,sizeof(packet),&valid)==-EBADF);
		assert(valid && fixture.feedback_calls==2);
		SLIST_INSERT_HEAD(&fixture.parser.thdlist_head,&fixture.thread,parser_list_entry);
		const char *bad[]={"FEEDBACKBUFM dev0 0\r\n","FEEDBACKBUFM dev0 +01\r\n",
			"FEEDBACKBUFM dev0 0g\r\n","FEEDBACKBUFM dev0 01-\r\n"};
		for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
			client_write(&fixture,bad[i],strlen(bad[i]));
			assert(fixture.output_bytes==4 && !memcmp(fixture.output,"-22\n",4));
			fixture.output_offset=fixture.output_bytes;
		}
		assert(fixture.feedback_calls==2 && fixture.entry.buf==NULL);
	}
	client_write(&fixture, "SCANTIMECAPS\n", strlen("SCANTIMECAPS\n"));
	assert(fixture.output_bytes == 2 && !memcmp(fixture.output, "1\n", 2));
	fixture.output_offset = fixture.output_bytes;
	client_write(&fixture, "SCANTIME dev0 0\n", strlen("SCANTIME dev0 0\n"));
	assert(fixture.output_bytes == 4 && !memcmp(fixture.output, "-22\n", 4));
	fixture.output_offset = fixture.output_bytes;
	drain_test_client_destroy(fixture.client);
	pthread_mutex_destroy(&fixture.entry.thdlist_lock);
	iio_context_destroy(fixture.parser.ctx);
	puts("metadata drain parser/client/provider: PASS");
	return 0;
}
