/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef METADATA_DRAIN_TEST_CLIENT_H
#define METADATA_DRAIN_TEST_CLIENT_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
struct iio_device;
struct drain_test_client;
typedef ssize_t (*drain_test_read)(void *, char *, size_t);
typedef ssize_t (*drain_test_write)(void *, const char *, size_t);
struct drain_test_client *drain_test_client_new(void *context,
		drain_test_read read, drain_test_write write);
void drain_test_client_destroy(struct drain_test_client *client);
ssize_t drain_test_client_run(struct drain_test_client *client,
		const struct iio_device *device, void *result, size_t capacity,
		bool *stream_valid);
#endif
