// SPDX-License-Identifier: LGPL-2.1-or-later
#include "iiod-command-batch.h"

#include <iio.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct write_state {
	char bytes[IIOD_METADATA_COMMAND_BATCH_BYTES_MAX];
	size_t bytes_written;
	unsigned int calls;
	ssize_t result;
};

static ssize_t record_write(void *opaque, const char *src, size_t len)
{
	struct write_state *state = opaque;

	state->calls++;
	assert(len <= sizeof(state->bytes));
	memcpy(state->bytes, src, len);
	state->bytes_written = len;
	return state->result >= 0 ? state->result : (ssize_t)len;
}

static ssize_t fail_write(void *opaque, const char *src, size_t len)
{
	struct write_state *state = opaque;
	(void)src;
	(void)len;
	state->calls++;
	return state->result;
}

int main(void)
{
	static const char command[] = "READBUFM dev0 8192 4096\r\n";
	struct write_state state = {.result = -1};
	ssize_t ret = iiod_command_batch_write(record_write, &state, command, 3);
	assert(ret == (ssize_t)(3 * (sizeof(command) - 1U)));
	assert(state.calls == 1);
	assert(state.bytes_written == (size_t)ret);
	for (unsigned int frame = 0; frame < 3; frame++)
		assert(!memcmp(state.bytes + frame * (sizeof(command) - 1U),
			command, sizeof(command) - 1U));

	/* A short write is fatal and is never retried behind response 1. */
	memset(&state, 0, sizeof(state));
	state.result = 1;
	assert(iiod_command_batch_write(record_write, &state, command, 3) == -EIO);
	assert(state.calls == 1);

	memset(&state, 0, sizeof(state));
	state.result = -EPIPE;
	assert(iiod_command_batch_write(fail_write, &state, command, 3) == -EPIPE);
	assert(state.calls == 1);

	assert(iiod_command_batch_write(record_write, &state, command, 0) == -EINVAL);
	assert(iiod_command_batch_write(record_write, &state, command,
		IIO_BUFFER_METADATA_BATCH_MAX + 1U) == -E2BIG);

	char oversized[IIOD_METADATA_COMMAND_BATCH_BYTES_MAX + 2U];
	memset(oversized, 'x', sizeof(oversized) - 1U);
	oversized[sizeof(oversized) - 1U] = '\0';
	state.calls = 0;
	assert(iiod_command_batch_write(record_write, &state, oversized, 1) ==
		-E2BIG);
	assert(state.calls == 0);

	puts("iiOD metadata command batch: PASS");
	return 0;
}
