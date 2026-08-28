// SPDX-License-Identifier: LGPL-2.1-or-later
#include "iiod-command-batch.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

ssize_t iiod_command_batch_write(iiod_command_write_fn write_fn, void *opaque,
		const char *command, unsigned int frames)
{
	char *commands;
	size_t command_bytes, commands_bytes;
	ssize_t ret;

	if (!write_fn || !command || !frames)
		return -EINVAL;
	if (frames > IIO_BUFFER_METADATA_BATCH_MAX)
		return -E2BIG;
	command_bytes = strlen(command);
	if (!command_bytes || command_bytes >
			IIOD_METADATA_COMMAND_BATCH_BYTES_MAX / frames)
		return -E2BIG;
	commands_bytes = command_bytes * frames;
	commands = malloc(commands_bytes);
	if (!commands)
		return -ENOMEM;
	for (unsigned int frame = 0; frame < frames; frame++)
		memcpy(commands + (size_t)frame * command_bytes,
			command, command_bytes);

	/* A partial first write could let iiOD parse READBUFM 1 and block while
	 * sending response 1. Never issue a second write behind that response. */
	ret = write_fn(opaque, commands, commands_bytes);
	free(commands);
	if (ret < 0)
		return ret;
	return (size_t)ret == commands_bytes ? ret : -EIO;
}
