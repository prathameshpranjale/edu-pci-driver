/*
 * Stress test: several processes hammer /dev/edu0 at once.
 * Each process uses a different sequence of inputs, so if the driver ever
 * hands one caller another caller's result, the check fails.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../driver/edu_uapi.h"

#define NPROC 8
#define NITER 500

static uint32_t factorial(uint32_t n)
{
	uint32_t r = 1;

	while (n > 1)
		r *= n--;
	return r;
}

/* Returns the number of wrong or failed requests. */
static int worker(int id)
{
	int fd = open("/dev/edu0", O_RDWR);
	int bad = 0;

	if (fd < 0) {
		perror("open");
		return NITER;
	}
	for (int i = 0; i < NITER; i++) {
		struct edu_fact req = { .n = (uint32_t)(id * 3 + i) % 13 };

		if (ioctl(fd, EDU_IOC_FACTORIAL, &req) < 0 ||
		    req.result != factorial(req.n))
			bad++;
	}
	close(fd);
	return bad;
}

int main(void)
{
	int failures = 0;

	for (int id = 0; id < NPROC; id++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0)
			_exit(worker(id) ? 1 : 0);
	}

	for (int id = 0; id < NPROC; id++) {
		int status;

		wait(&status);
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			failures++;
	}

	printf("test_stress: %s (%d procs x %d requests, %d bad workers)\n",
	       failures ? "FAIL" : "PASS", NPROC, NITER, failures);
	return failures ? 1 : 0;
}
