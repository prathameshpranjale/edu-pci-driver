// SPDX-License-Identifier: GPL-2.0
/*
 * edu_driver: minimal PCI driver for QEMU's "edu" device.
 *
 * Binds to the device, maps BAR0, handles its interrupt, and exposes
 * /dev/edu0 with an ioctl that computes a factorial on the device.
 */
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/uaccess.h>

#include "edu_uapi.h"

#define EDU_VENDOR_ID   0x1234
#define EDU_DEVICE_ID   0x11e8

#define EDU_REG_ID        0x00	/* read-only: 0xRRrr00ed (major.minor) */
#define EDU_REG_LIVENESS  0x04	/* write x, read back ~x */
#define EDU_REG_FACT      0x08	/* write n: start n!; read: result */
#define EDU_REG_STATUS    0x20	/* bit0 busy, bit7 raise irq when done */
#define EDU_REG_IRQ_STAT  0x24	/* nonzero while an irq is pending */
#define EDU_REG_IRQ_ACK   0x64	/* write the bits to clear */

#define EDU_REG_DMA_SRC   0x80	/* 64-bit */
#define EDU_REG_DMA_DST   0x88	/* 64-bit */
#define EDU_REG_DMA_CNT   0x90	/* 64-bit */
#define EDU_REG_DMA_CMD   0x98	/* 64-bit */

#define EDU_STATUS_IRQ_ON_DONE  0x80
#define EDU_IRQ_FACT_DONE       0x01
#define EDU_IRQ_DMA_DONE        0x100

#define EDU_DMA_RUN       0x1
#define EDU_DMA_TO_RAM    0x2	/* direction: device buffer -> RAM (0 means RAM -> device) */
#define EDU_DMA_IRQ       0x4	/* raise the DMA-done interrupt */

#define EDU_DEV_BUF       0x40000	/* device-internal buffer, EDU_DMA_MAX bytes */
#define EDU_DMA_MASK_BITS 28		/* the device can only address 28 bits of RAM */

struct edu_dev {
	void __iomem *regs;
	struct miscdevice misc;
	struct mutex lock;	/* one job at a time: the device has one engine */
	struct completion done;	/* signalled by the IRQ handler */
	u32 result;
	void *dma_tx, *dma_rx;		/* coherent buffers shared with the device */
	dma_addr_t tx_bus, rx_bus;	/* what the device uses to reach them */
};

/* Runs in interrupt context: read, ack, store, wake. Nothing heavier. */
static irqreturn_t edu_irq(int irq, void *data)
{
	struct edu_dev *edu = data;
	u32 stat = ioread32(edu->regs + EDU_REG_IRQ_STAT);

	if (!stat)
		return IRQ_NONE;

	iowrite32(stat, edu->regs + EDU_REG_IRQ_ACK);
	if (stat & EDU_IRQ_FACT_DONE)
		edu->result = ioread32(edu->regs + EDU_REG_FACT);
	if (stat & (EDU_IRQ_FACT_DONE | EDU_IRQ_DMA_DONE))
		complete(&edu->done);
	return IRQ_HANDLED;
}

/* Submit n! to the device and sleep until the interrupt delivers the result. */
static int edu_factorial(struct edu_dev *edu, u32 n, u32 *out)
{
	int ret = 0;

	mutex_lock(&edu->lock);
	reinit_completion(&edu->done);
	iowrite32(EDU_STATUS_IRQ_ON_DONE, edu->regs + EDU_REG_STATUS);
	iowrite32(n, edu->regs + EDU_REG_FACT);

	if (!wait_for_completion_timeout(&edu->done, msecs_to_jiffies(1000)))
		ret = -ETIMEDOUT;
	else
		*out = edu->result;
	mutex_unlock(&edu->lock);

	return ret;
}

/* One DMA transfer; caller holds edu->lock. src/dst are bus addresses or device offsets. */
static int edu_dma_run(struct edu_dev *edu, u64 src, u64 dst, u32 len, u32 dir)
{
	reinit_completion(&edu->done);
	writeq(src, edu->regs + EDU_REG_DMA_SRC);
	writeq(dst, edu->regs + EDU_REG_DMA_DST);
	writeq(len, edu->regs + EDU_REG_DMA_CNT);
	writeq(EDU_DMA_RUN | EDU_DMA_IRQ | dir, edu->regs + EDU_REG_DMA_CMD);

	/* the device finishes DMA on a ~100 ms timer, so allow plenty of time */
	if (!wait_for_completion_timeout(&edu->done, msecs_to_jiffies(2000)))
		return -ETIMEDOUT;
	return 0;
}

static int edu_dma_loopback(struct edu_dev *edu, struct edu_dma *req)
{
	int ret;

	if (!req->len || req->len > EDU_DMA_MAX)
		return -EINVAL;

	mutex_lock(&edu->lock);
	if (copy_from_user(edu->dma_tx, u64_to_user_ptr(req->in), req->len)) {
		ret = -EFAULT;
		goto out;
	}
	memset(edu->dma_rx, 0, req->len);	/* so a transfer that does nothing is visible */

	ret = edu_dma_run(edu, edu->tx_bus, EDU_DEV_BUF, req->len, 0);
	if (!ret)
		ret = edu_dma_run(edu, EDU_DEV_BUF, edu->rx_bus, req->len, EDU_DMA_TO_RAM);
	if (!ret && copy_to_user(u64_to_user_ptr(req->out), edu->dma_rx, req->len))
		ret = -EFAULT;
out:
	mutex_unlock(&edu->lock);
	return ret;
}

static long edu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	/* misc_register() stores the miscdevice in file->private_data */
	struct edu_dev *edu = container_of(file->private_data, struct edu_dev, misc);
	void __user *uarg = (void __user *)arg;
	struct edu_fact fact;
	struct edu_dma dma;
	int ret;

	switch (cmd) {
	case EDU_IOC_FACTORIAL:
		if (copy_from_user(&fact, uarg, sizeof(fact)))
			return -EFAULT;
		ret = edu_factorial(edu, fact.n, &fact.result);
		if (ret)
			return ret;
		return copy_to_user(uarg, &fact, sizeof(fact)) ? -EFAULT : 0;
	case EDU_IOC_DMA_LOOPBACK:
		if (copy_from_user(&dma, uarg, sizeof(dma)))
			return -EFAULT;
		return edu_dma_loopback(edu, &dma);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations edu_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = edu_ioctl,
};

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_dev *edu;
	u32 reg_id, live;
	int ret;

	edu = devm_kzalloc(&pdev->dev, sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;

	/* pcim_* helpers undo themselves on remove */
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, BIT(0), KBUILD_MODNAME);
	if (ret)
		return ret;

	edu->regs = pcim_iomap_table(pdev)[0];
	pci_set_drvdata(pdev, edu);

	reg_id = ioread32(edu->regs + EDU_REG_ID);
	dev_info(&pdev->dev, "edu ID register = 0x%08x (version %u.%u)\n",
		 reg_id, reg_id >> 24, (reg_id >> 16) & 0xff);

	iowrite32(0x12345678, edu->regs + EDU_REG_LIVENESS);
	live = ioread32(edu->regs + EDU_REG_LIVENESS);
	if (live != ~0x12345678U) {
		dev_err(&pdev->dev, "liveness check failed: got 0x%08x\n", live);
		return -EIO;
	}

	mutex_init(&edu->lock);
	init_completion(&edu->done);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(EDU_DMA_MASK_BITS));
	if (ret)
		return ret;
	pci_set_master(pdev);	/* the device must be allowed to access RAM */
	edu->dma_tx = dmam_alloc_coherent(&pdev->dev, EDU_DMA_MAX, &edu->tx_bus, GFP_KERNEL);
	edu->dma_rx = dmam_alloc_coherent(&pdev->dev, EDU_DMA_MAX, &edu->rx_bus, GFP_KERNEL);
	if (!edu->dma_tx || !edu->dma_rx)
		return -ENOMEM;

	/*
	 * Legacy INTx on purpose: QEMU's edu model only raises INTx. Even when
	 * the guest enables MSI it never sends the message (verified with and
	 * without KVM), so the IRQ would be lost. INTx lines are shared, hence
	 * IRQF_SHARED and the IRQ_NONE path in the handler.
	 */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_LEGACY);
	if (ret < 0)
		return ret;
	ret = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0), edu_irq,
			       IRQF_SHARED, KBUILD_MODNAME, edu);
	if (ret)
		return ret;

	edu->misc.minor = MISC_DYNAMIC_MINOR;
	edu->misc.name  = "edu0";
	edu->misc.fops  = &edu_fops;
	ret = misc_register(&edu->misc);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "ready: /dev/edu0, irq %d\n", pci_irq_vector(pdev, 0));
	return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *edu = pci_get_drvdata(pdev);

	/* first: no new ioctl can start; the irq and mappings go away after we return */
	misc_deregister(&edu->misc);
	dev_info(&pdev->dev, "edu removed\n");
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
	.name     = KBUILD_MODNAME,
	.id_table = edu_ids,
	.probe    = edu_probe,
	.remove   = edu_remove,
};
module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal driver for the QEMU edu PCI device");
