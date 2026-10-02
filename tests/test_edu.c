/* User-space test for /dev/edu0. Built static: the initramfs has no libc. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../driver/edu_uapi.h"

static uint32_t factorial(uint32_t n)
{
	uint32_t r = 1;

	while (n > 1)
		r *= n--;
	return r;
}

int main(void)
{
	int fd = open("/dev/edu0", O_RDWR);
	int failures = 0;

	if (fd < 0) {
		perror("open /dev/edu0");
		return 1;
	}

	for (uint32_t n = 0; n <= 12; n++) {	/* 12! is the largest that fits 32 bits */
		struct edu_fact req = { .n = n };

		if (ioctl(fd, EDU_IOC_FACTORIAL, &req) < 0) {
			perror("ioctl");
			failures++;
			continue;
		}
		if (req.result != factorial(n)) {
			printf("FAIL %u! = %u, expected %u\n", n, req.result, factorial(n));
			failures++;
		}
	}

	close(fd);
	printf("test_edu: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
