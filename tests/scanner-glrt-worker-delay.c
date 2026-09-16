/* Test-only overload at the real worker's numerical entry point. Link with
 * --wrap=leo_presence_dwell_run_ci16; never include in a radio bundle. The
 * real computation and evidence remain unchanged after the deliberate wait. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <time.h>
#include "dwell.h"

#ifndef SPF_TEST_DWELL_DELAY_NS
#define SPF_TEST_DWELL_DELAY_NS 170000000L
#endif

int __real_leo_presence_dwell_run_ci16(leo_presence_dwell_workspace *,
    const int16_t *, size_t, uint32_t, uint32_t, leo_presence_dwell_result *);

int __wrap_leo_presence_dwell_run_ci16(leo_presence_dwell_workspace *workspace,
    const int16_t *iq, size_t count, uint32_t confirmations,
    uint32_t seeded, leo_presence_dwell_result *result)
{
    struct timespec remaining = {0, SPF_TEST_DWELL_DELAY_NS};
    while (nanosleep(&remaining, &remaining)) {
        if (errno != EINTR) return -1;
    }
    return __real_leo_presence_dwell_run_ci16(workspace, iq, count,
        confirmations, seeded, result);
}
