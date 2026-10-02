/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* User-space interface of /dev/edu0, shared by the driver and the tests. */
#ifndef EDU_UAPI_H
#define EDU_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct edu_fact {
	__u32 n;	/* in: compute n! */
	__u32 result;	/* out: n! modulo 2^32 (the device register is 32 bits) */
};

/* DMA loopback: user buffer -> device (DMA) -> back into another buffer (DMA) -> user */
#define EDU_DMA_MAX 4096	/* size of the device's internal buffer */

struct edu_dma {
	__u64 in;	/* user pointer: data to send */
	__u64 out;	/* user pointer: receives the data after the round trip */
	__u32 len;	/* 1..EDU_DMA_MAX */
	__u32 pad;
};

#define EDU_IOC_MAGIC        'E'
#define EDU_IOC_FACTORIAL    _IOWR(EDU_IOC_MAGIC, 1, struct edu_fact)
#define EDU_IOC_DMA_LOOPBACK _IOW(EDU_IOC_MAGIC, 2, struct edu_dma)

#endif
