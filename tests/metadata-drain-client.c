// SPDX-License-Identifier: LGPL-2.1-or-later
/* Keep iiOD's and the client's private headers in separate translation units. */
#include "metadata-drain-client.h"
#include "../iiod-client.h"
#include <errno.h>
#include <stdlib.h>

struct drain_test_client {
	void *context;
	drain_test_read read;
	drain_test_write write;
	struct iiod_client *client;
};

static ssize_t client_read(struct iio_context_pdata *opaque,
		struct iiod_client_pdata *desc, char *output, size_t bytes)
{
	struct drain_test_client *client = (void *)opaque;
	(void)desc;
	return client->read(client->context, output, bytes);
}

static ssize_t client_write(struct iio_context_pdata *opaque,
		struct iiod_client_pdata *desc, const char *input, size_t bytes)
{
	struct drain_test_client *client = (void *)opaque;
	(void)desc;
	return client->write(client->context, input, bytes);
}

static ssize_t client_line(struct iio_context_pdata *opaque,
		struct iiod_client_pdata *desc, char *output, size_t capacity)
{
	size_t bytes = 0;
	while (bytes < capacity) {
		ssize_t ret = client_read(opaque, desc, output + bytes, 1);
		if (ret < 0)
			return ret;
		if (output[bytes++] == '\n')
			return (ssize_t)bytes;
	}
	return -EOVERFLOW;
}

static const struct iiod_client_ops ops = {
	.write = client_write, .read = client_read, .read_line = client_line,
};

struct drain_test_client *drain_test_client_new(void *context,
		drain_test_read read, drain_test_write write)
{
	struct drain_test_client *client = calloc(1, sizeof(*client));
	if (!client)
		return NULL;
	client->context = context;
	client->read = read;
	client->write = write;
	client->client = iiod_client_new((void *)client, &ops);
	if (!client->client) {
		free(client);
		return NULL;
	}
	return client;
}

void drain_test_client_destroy(struct drain_test_client *client)
{
	iiod_client_destroy(client->client);
	free(client);
}

ssize_t drain_test_client_run(struct drain_test_client *client,
		const struct iio_device *device, void *result, size_t capacity,
		bool *stream_valid)
{
	return iiod_client_drain_buffer_metadata_unlocked(client->client,
		(void *)client, device, result, capacity, stream_valid);
}

int feedback_test_client_run(struct drain_test_client *client, const struct iio_device *dev,
	const void *feedback, size_t bytes, bool *valid)
{
	return iiod_client_submit_metadata_feedback_unlocked(client->client,(void *)client,
		dev,feedback,bytes,valid);
}
