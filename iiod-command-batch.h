/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __IIOD_COMMAND_BATCH_H__
#define __IIOD_COMMAND_BATCH_H__

#include <iio.h>

#define IIOD_METADATA_COMMAND_BATCH_BYTES_MAX 4096U

typedef ssize_t (*iiod_command_write_fn)(void *opaque,
		const char *src, size_t len);

ssize_t iiod_command_batch_write(iiod_command_write_fn write_fn, void *opaque,
		const char *command, unsigned int frames);

#endif
