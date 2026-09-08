/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual iiOD protocol + SPF provider. Only IIO hardware operations are mocked.
 * Loopback only, finite process watchdog, no local/USB/network radio context. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define iio_device_create_buffer network_create_buffer
#define iio_buffer_destroy network_destroy_buffer
#define iio_buffer_refill network_refill
#define iio_buffer_start glrt_network_buffer_start
#define iio_buffer_cancel network_cancel_buffer
#define iio_buffer_get_allocated_kernel_buffers_count network_allocated_buffers
#define iio_device_get_kernel_buffers_count network_kernel_buffers
#define iio_device_set_kernel_buffers_count network_set_kernel_buffers
#define iio_context_set_timeout network_set_timeout
#define iiod_buffer_metadata_open network_metadata_open
#include "../iiod/ops.c"
#undef iiod_buffer_metadata_open
int iiod_buffer_metadata_open(const struct iio_device *, size_t, const uint32_t *, size_t,
	size_t, const void *, size_t, void **, size_t *, struct iiod_buffer_burst_plan *);
#include "scanner-glrt-network-fixture.h"
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdlib.h>

bool server_demux;
struct thread_pool *main_thread_pool;
static volatile sig_atomic_t stopping;
static int listener = -1;
static unsigned int kernel_count = 8;
static uint32_t rate;
static atomic_uint opens, destroys, refills, drains;
static bool missing_final;
static ssize_t (*actual_drain)(void *, void *, size_t);

struct virtual_buffer {
	uint8_t *data;
	size_t samples, bytes;
	uint64_t next_counter;
	atomic_bool cancelled;
};

void *glrt_network_buffer_start(const struct iio_buffer *buffer)
{ return ((const struct virtual_buffer *)buffer)->data; }
size_t glrt_network_buffer_samples(const struct iio_buffer *buffer)
{ return ((const struct virtual_buffer *)buffer)->samples; }

struct iio_buffer *network_create_buffer(const struct iio_device *dev, size_t samples, bool cyclic)
{
	assert(dev && !cyclic && samples > 1 && samples <= 1048577);
	struct virtual_buffer *buffer = calloc(1, sizeof(*buffer));
	assert(buffer);
	buffer->samples = samples - 1; /* Existing SPF timestamp sample. */
	buffer->bytes = samples * 8;
	buffer->data = calloc(1, buffer->bytes);
	assert(buffer->data);
	buffer->next_counter = GLRT_FIXTURE_FIRST;
	atomic_init(&buffer->cancelled, false);
	atomic_fetch_add(&opens, 1);
	return (void *)buffer;
}

void network_destroy_buffer(struct iio_buffer *opaque)
{
	struct virtual_buffer *buffer = (void *)opaque;
	assert(buffer);
	free(buffer->data); free(buffer);
	atomic_fetch_add(&destroys, 1);
}

void network_cancel_buffer(struct iio_buffer *buffer)
{ atomic_store(&((struct virtual_buffer *)buffer)->cancelled, true); }

ssize_t network_refill(struct iio_buffer *opaque)
{
	struct virtual_buffer *buffer = (void *)opaque;
	if (atomic_load(&buffer->cancelled)) return -ECANCELED;
	/* A replay sample clock, not a claim to simulate DMA/IRQ timing. */
	uint64_t first = buffer->next_counter;
	memcpy(buffer->data, &first, 8);
	for (size_t i = 0; i < buffer->samples; i++) {
		int16_t *iq = (void *)(buffer->data + 8 + i * 8);
		iq[0] = 30000; iq[1] = -20000;
		iq[2] = 0; iq[3] = 0;
	}
	buffer->next_counter += buffer->samples;
	assert(atomic_fetch_add(&refills, 1) < 20000); /* Bounded corrupt-client load. */
	return (ssize_t)buffer->bytes;
}

int network_allocated_buffers(const struct iio_buffer *buffer, unsigned int *count)
{ assert(buffer && count); *count = kernel_count; return 0; }
unsigned int network_kernel_buffers(const struct iio_device *dev)
{ assert(dev); return kernel_count; }
int network_set_kernel_buffers(const struct iio_device *dev, unsigned int count)
{ assert(dev && count >= 2 && count <= 64); kernel_count = count; return 0; }
int network_set_timeout(struct iio_context *ctx, unsigned int ms)
{ assert(ctx && ms); return 0; }

static ssize_t network_drain(void *context, void *output, size_t capacity)
{
	unsigned int before = atomic_load(&refills);
	ssize_t result = actual_drain(context, output, capacity);
	assert(atomic_load(&refills) == before); /* Tail delivery must not refill. */
	atomic_fetch_add(&drains, 1);
	/* Deliberate terminal loss after the actual provider has produced FINAL.
	 * The IQ stream has already ended; the client must not report completion. */
	if (missing_final && result >= 128 && (((uint8_t *)output)[20] & 2))
		return -ENODATA;
	return result;
}

int network_metadata_open(const struct iio_device *dev, size_t samples, const uint32_t *mask,
	size_t words, size_t sample_bytes, const void *request, size_t request_bytes,
	void **context, size_t *extra, struct iiod_buffer_burst_plan *plan)
{
	int ret = iiod_buffer_metadata_open(dev, samples, mask, words, sample_bytes,
		request, request_bytes, context, extra, plan);
	if (!ret) glrt_fixture_metadata_opened(*context);
	if (!ret && plan->drain_metadata) {
		actual_drain = plan->drain_metadata;
		plan->drain_metadata = network_drain;
	}
	return ret;
}

struct client { struct iio_context *ctx; int fd; };
static void serve_client(struct thread_pool *pool, void *opaque)
{
	struct client *client = opaque;
	interpreter(client->ctx, client->fd, client->fd, false, true, false, false, pool, NULL, 0);
	close(client->fd); free(client);
}

static void stop_server(int signal_number)
{ (void)signal_number; stopping = 1; if (listener >= 0) close(listener); }

int main(int argc, char **argv)
{
	assert(argc == 4);
	rate = (uint32_t)strtoul(argv[1], NULL, 10);
	assert(rate == 2500000 || rate == 5000000);
	unsigned int delay = (unsigned int)strtoul(argv[2], NULL, 10);
	assert(delay <= 2);
	bool failure = !strcmp(argv[3], "worker-failure");
	missing_final = !strcmp(argv[3], "missing-final");
	assert(failure || missing_final || !strcmp(argv[3], "normal"));
	alarm(90);
	signal(SIGPIPE, SIG_IGN);
	struct sigaction action = {.sa_handler = stop_server};
	sigemptyset(&action.sa_mask);
	assert(sigaction(SIGTERM, &action, NULL) == 0);
	char xml[8192];
	int count = snprintf(xml, sizeof(xml),
		"<!DOCTYPE context [<!ELEMENT context (context-attribute | device)*>"
		"<!ATTLIST context name CDATA #REQUIRED><!ELEMENT context-attribute EMPTY>"
		"<!ATTLIST context-attribute name CDATA #REQUIRED value CDATA #REQUIRED>"
		"<!ELEMENT device (channel*)><!ATTLIST device id CDATA #REQUIRED name CDATA #IMPLIED>"
		"<!ELEMENT channel (scan-element)><!ATTLIST channel id CDATA #REQUIRED type CDATA #REQUIRED>"
		"<!ELEMENT scan-element EMPTY><!ATTLIST scan-element index CDATA #REQUIRED format CDATA #REQUIRED>]>"
		"<context name=\"xml\">"
		"<context-attribute name=\"hw_serial\" value=\"loopback-fixture-only\"/>"
		"<context-attribute name=\"iio,buffer-metadata\" value=\"3\"/>"
		"<context-attribute name=\"iio,buffer-metadata-status\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-persistent-hop\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-persistent-hop-request\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-persistent-hop-event\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-persistent-hop-status\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-persistent-hop-cancel\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-metadata-drain\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-scanner-glrt\" value=\"1\"/>"
		"<context-attribute name=\"iio,buffer-scanner-glrt-mode\" value=\"unqualified-evidence\"/>"
		"<context-attribute name=\"iio,buffer-scanner-glrt-algorithm-sha256\" value=\"%s\"/>"
		"<context-attribute name=\"iio,buffer-scanner-glrt-configuration-sha256\" value=\"%s\"/>"
		"<device id=\"dev0\" name=\"cf-ad9361-lpc\">"
		"<channel id=\"voltage0\" type=\"input\"><scan-element index=\"0\" format=\"le:S16/16&gt;&gt;0\"/></channel>"
		"<channel id=\"voltage1\" type=\"input\"><scan-element index=\"1\" format=\"le:S16/16&gt;&gt;0\"/></channel>"
		"<channel id=\"voltage2\" type=\"input\"><scan-element index=\"2\" format=\"le:S16/16&gt;&gt;0\"/></channel>"
		"<channel id=\"voltage3\" type=\"input\"><scan-element index=\"3\" format=\"le:S16/16&gt;&gt;0\"/></channel>"
		"</device></context>", IIOD_SCANNER_GLRT_ALGORITHM_SHA256,
		IIOD_SCANNER_GLRT_CONFIGURATION_SHA256);
	assert(count > 0 && count < (int)sizeof(xml));
	struct iio_context *ctx = iio_create_xml_context_mem(xml, (size_t)count);
	assert(ctx);
	glrt_fixture_configure(iio_context_find_device(ctx, "dev0"), rate, delay, failure);
	main_thread_pool = thread_pool_new(); assert(main_thread_pool);
	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); assert(listener >= 0);
	struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(listen(listener, 8) == 0);
	socklen_t length = sizeof(address);
	assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
	printf("%u\n", (unsigned int)ntohs(address.sin_port)); fflush(stdout);
	while (!stopping) {
		int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0) { if (stopping) break; assert(errno == EINTR); continue; }
		struct client *client = malloc(sizeof(*client)); assert(client);
		*client = (struct client){ctx, fd};
		assert(thread_pool_add_thread(main_thread_pool, serve_client, client, "glrt_fixture") == 0);
	}
	thread_pool_stop_and_wait(main_thread_pool);
	thread_pool_destroy(main_thread_pool);
	assert(atomic_load(&opens) == atomic_load(&destroys));
	iio_context_destroy(ctx);
	printf("{\"opens\":%u,\"destroys\":%u,\"refills\":%u,\"drains\":%u}\n",
		atomic_load(&opens), atomic_load(&destroys), atomic_load(&refills), atomic_load(&drains));
	return 0;
}
