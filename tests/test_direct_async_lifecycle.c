#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "direct-async-lifecycle.h"

#define CHECK(condition) \
	do { \
		if (!(condition)) \
			return 1; \
	} while (0)

int main(void)
{
	CHECK(iiod_direct_async_segment_is_complete(true, true, true, false,
		false, 0, 4, 4, 0));
	CHECK(!iiod_direct_async_segment_is_complete(true, true, false, false,
		false, 0, 4, 4, 0));
	CHECK(!iiod_direct_async_segment_is_complete(true, true, true, true,
		false, 0, 4, 4, 0));
	CHECK(!iiod_direct_async_segment_is_complete(true, true, true, false,
		false, 1, 3, 4, 0));
	CHECK(!iiod_direct_async_segment_is_complete(true, true, true, false,
		true, 0, 4, 4, 0));
	CHECK(!iiod_direct_async_segment_is_complete(true, true, true, false,
		false, 0, 4, 4, -5));
	CHECK(!iiod_direct_async_segment_is_complete(true, true, true, false,
		false, 0, 3, 4, 0));
	return 0;
}
