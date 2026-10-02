/* DMA loopback test: data goes to the device and back, then must match. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../driver/edu_uapi.h"

static int check_len(int fd, uint32_t len)
{
	static unsigned char in[EDU_DMA_MAX], out[EDU_DMA_MAX];
	struct edu_dma req = { .in = (uintptr_t)in, .out = (uintptr_t)out, .len = len };

	for (uint32_t i = 0; i < len; i++)
		in[i] = (unsigned char)(i * 7 + len);
	memset(out, 0xAA, sizeof(out));

	if (ioctl(fd, EDU_IOC_DMA_LOOPBACK, &req) < 0) {
		printf("FAIL len=%u: ioctl: %s\n", len, strerror(errno));
		return 1;
	}
	if (memcmp(in, out, len)) {
		printf("FAIL len=%u: data mismatch\n", len);
		return 1;
	}
	if (len < EDU_DMA_MAX && out[len] != 0xAA) {
		printf("FAIL len=%u: wrote past the requested length\n", len);
		return 1;
	}
	return 0;
}

static int expect_einval(int fd, uint32_t len)
{
	unsigned char buf[8] = {0};
	struct edu_dma req = { .in = (uintptr_t)buf, .out = (uintptr_t)buf, .len = len };

	if (ioctl(fd, EDU_IOC_DMA_LOOPBACK, &req) == 0 || errno != EINVAL) {
		printf("FAIL len=%u: expected EINVAL\n", len);
		return 1;
	}
	return 0;
}

int main(void)
{
	static const uint32_t lens[] = { 1, 4, 63, 64, 1000, 4095, 4096 };
	int fd = open("/dev/edu0", O_RDWR);
	int failures = 0;

	if (fd < 0) {
		perror("open /dev/edu0");
		return 1;
	}
	for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
		failures += check_len(fd, lens[i]);
	failures += expect_einval(fd, 0);
	failures += expect_einval(fd, EDU_DMA_MAX + 1);

	close(fd);
	printf("test_dma: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
