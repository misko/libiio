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
	cpu_set_t default_affinity;
	int pinned_passed;
	int default_passed;
};

static void check_default_affinity(struct thread_pool *pool, void *data)
{
	struct affinity_result *result = data;
	cpu_set_t cpuset;
	uint64_t signal = 1;

	result->default_passed =
		sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0 &&
		CPU_EQUAL(&result->default_affinity, &cpuset);
	if (write(result->eventfd, &signal, sizeof(signal)) != sizeof(signal))
		result->default_passed = 0;
}

static void check_pinned_affinity(struct thread_pool *pool, void *data)
{
	struct affinity_result *result = data;
	cpu_set_t cpuset;
	uint64_t signal = 1;
	int i;
	int ret;

	result->pinned_passed = sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0 &&
		CPU_ISSET((unsigned int)result->cpu, &cpuset);
	for (i = 0; result->pinned_passed && i < CPU_SETSIZE; i++) {
		if (i != result->cpu && CPU_ISSET((unsigned int)i, &cpuset))
			result->pinned_passed = 0;
	}
	ret = thread_pool_add_thread(pool, check_default_affinity, result,
		"default_affinity_test");
	if (ret) {
		result->default_passed = 0;
		signal++;
	}
	if (write(result->eventfd, &signal, sizeof(signal)) != sizeof(signal))
		result->pinned_passed = 0;
}

int main(void)
{
	struct affinity_result result = { .eventfd = -1, .cpu = -1 };
	struct thread_pool *pool;
	cpu_set_t allowed;
	uint64_t signals = 0;
	int ret;
	int i;

	if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
		perror("sched_getaffinity");
		return 1;
	}
	result.default_affinity = allowed;
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

	ret = thread_pool_add_thread_on_cpu(pool, check_pinned_affinity, &result,
		"affinity_test", result.cpu);
	if (ret) {
		errno = ret;
		perror("thread_pool_add_thread_on_cpu");
		close(result.eventfd);
		thread_pool_destroy(pool);
		return 1;
	}
	while (signals < 2) {
		uint64_t signal;

		if (read(result.eventfd, &signal, sizeof(signal)) != sizeof(signal)) {
			perror("read");
			result.pinned_passed = 0;
			result.default_passed = 0;
			break;
		}
		signals += signal;
	}
	thread_pool_stop_and_wait(pool);
	close(result.eventfd);
	thread_pool_destroy(pool);

	if (!result.pinned_passed || !result.default_passed) {
		fprintf(stderr,
			"worker-specific or nested default CPU affinity was not preserved\n");
		return 1;
	}
	return 0;
}
