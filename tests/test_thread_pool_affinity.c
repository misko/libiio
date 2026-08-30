// SPDX-License-Identifier: LGPL-2.1-or-later

#include "../iiod/thread-pool.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct affinity_result {
	int eventfd;
	int cpu;
	int passed;
};

static void check_affinity(struct thread_pool *pool, void *data)
{
	struct affinity_result *result = data;
	cpu_set_t cpuset;
	uint64_t signal = 1;
	int i;

	result->passed = sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0 &&
		CPU_ISSET((unsigned int)result->cpu, &cpuset);
	for (i = 0; result->passed && i < CPU_SETSIZE; i++) {
		if (i != result->cpu && CPU_ISSET((unsigned int)i, &cpuset))
			result->passed = 0;
	}
	if (write(result->eventfd, &signal, sizeof(signal)) != sizeof(signal))
		result->passed = 0;
}

int main(void)
{
	struct affinity_result result = { .eventfd = -1, .cpu = -1 };
	struct thread_pool *pool;
	cpu_set_t allowed;
	uint64_t signal;
	int ret;
	int i;

	if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
		perror("sched_getaffinity");
		return 1;
	}
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET((unsigned int)i, &allowed)) {
			result.cpu = i;
			break;
		}
	}
	if (result.cpu < 0) {
		fprintf(stderr, "no allowed CPU\n");
		return 1;
	}

	pool = thread_pool_new();
	if (!pool) {
		perror("thread_pool_new");
		return 1;
	}
	result.eventfd = eventfd(0, EFD_CLOEXEC);
	if (result.eventfd < 0) {
		perror("eventfd");
		thread_pool_destroy(pool);
		return 1;
	}

	ret = thread_pool_add_thread_on_cpu(pool, check_affinity, &result,
		"affinity_test", result.cpu);
	if (ret) {
		errno = ret;
		perror("thread_pool_add_thread_on_cpu");
		close(result.eventfd);
		thread_pool_destroy(pool);
		return 1;
	}
	if (read(result.eventfd, &signal, sizeof(signal)) != sizeof(signal)) {
		perror("read");
		result.passed = 0;
	}
	thread_pool_stop_and_wait(pool);
	close(result.eventfd);
	thread_pool_destroy(pool);

	if (!result.passed) {
		fprintf(stderr, "worker did not inherit the requested CPU mask\n");
		return 1;
	}
	return 0;
}
