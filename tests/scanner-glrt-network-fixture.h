/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef SCANNER_GLRT_NETWORK_FIXTURE_H
#define SCANNER_GLRT_NETWORK_FIXTURE_H
#include <iio.h>
#include <stdbool.h>
#include <stdint.h>
#define GLRT_FIXTURE_FIRST ((UINT64_C(1) << 53) + 10000)
void glrt_fixture_configure(struct iio_device *, uint32_t, unsigned int, bool);
void glrt_fixture_metadata_opened(void *);
void *glrt_network_buffer_start(const struct iio_buffer *);
size_t glrt_network_buffer_samples(const struct iio_buffer *);
#endif
