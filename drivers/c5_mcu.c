// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/mcu for the X2600's RISC-V core, with the ioctls of FlashForge's closed
 * soc_mcu.ko (reverse engineered from it), for programs that load and start
 * their own firmware on the core, such as sep-ai.
 *
 *   MCU_LOAD  {offset, source, size}: copies size bytes of user memory to the
 *             firmware's load address + offset. A chunk whose second word is
 *             the header magic stops the core first and takes the load
 *             address, entry point and shared-memory window from the header.
 *   MCU_START sets the entry point and memory protection, releases reset.
 *   MCU_STOP  holds the core in reset.
 *   MCU_RESET pulses reset: the loaded firmware starts over.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <asm/addrspace.h>

#define MCU_STOP		_IO('M', 0x70)
#define MCU_RESET		_IO('M', 0x71)
#define MCU_START		_IO('M', 0x72)
#define MCU_LOAD		_IOW('M', 0x73, int)

/* Core control unit: the RISC-V core's reset */
#define CCU_PHYS		0x12200000
#define CCU_CFCR		0xfe0
#define CFCR_RISCV_RESET	BIT(31)
/* Clock gate of the efuse, read once before every start */
#define CPM_PHYS		0x10000000
#define CPM_CLKGR0		0x20
#define CLKGR0_EFUSE		BIT(0)
#define EFUSE_PHYS		0x13480000
#define EFUSE_CTRL		0x0
#define EFUSE_CFG		0x4
#define EFUSE_STATE		0x8
#define EFUSE_DATA		0xc

/* The core's own registers (the DT node's first reg) */
#define RV_CCSR			0x00
#define RV_CRER			0x04	/* reset entry */
#define RV_FROM_HOST		0x08
#define RV_TO_HOST		0x0c
#define RV_INTC_MASK_L		0x20
#define RV_INTC_MASK_H		0x24
#define RV_PMA_ADR(n)		(0x40 + 4 * (n))
#define RV_PMA_CFG(n)		(0x60 + 4 * (n))
#define PMA_X			BIT(0)
#define PMA_R			BIT(1)
#define PMA_W			BIT(2)
#define PMA_TOR			(1 << 3)
#define PMA_NAPOT		(3 << 3)
#define PMA_CACHED		BIT(5)

#define MCU_HEADER_MAGIC	0x4c455054
/* A stored header with this flag is never replaced by a later load */
#define MCU_HEADER_LOCKED	0xfffffffc

/* At the start of the firmware image */
struct mcu_header {
	u32 word0;
	u32 magic;
	u32 load_addr;		/* physical */
	u32 entry;
	u32 flag;
	u32 reserved[3];
	u32 shared_addr;	/* uncached window shared with Linux */
	u32 shared_size;
};

struct mcu_load {
	u32 offset;
	u32 source;		/* user address */
	u32 size;
};

struct c5_mcu {
	void __iomem *rv, *ccu, *cpm, *efuse;
	struct mutex lock;
	struct mcu_header hdr;
	struct miscdevice misc;
};

static void c5_mcu_hold_reset(struct c5_mcu *mcu, bool hold)
{
	u32 v = readl(mcu->ccu + CCU_CFCR);

	writel(hold ? v | CFCR_RISCV_RESET : v & ~CFCR_RISCV_RESET, mcu->ccu + CCU_CFCR);
}

/* soc_mcu refuses to start the core on chips whose efuse word fails this */
static int c5_mcu_check_efuse(struct c5_mcu *mcu)
{
	u32 gate = readl(mcu->cpm + CPM_CLKGR0), state, data;
	int ret;

	if (gate & CLKGR0_EFUSE)
		writel(gate & ~CLKGR0_EFUSE, mcu->cpm + CPM_CLKGR0);
	writel(0, mcu->efuse + EFUSE_CFG);
	writel(0, mcu->efuse + EFUSE_STATE);
	writel(0x03600000, mcu->efuse + EFUSE_CTRL);
	writel(0x03600001, mcu->efuse + EFUSE_CTRL);
	ret = readl_poll_timeout_atomic(mcu->efuse + EFUSE_STATE, state, state & 1, 1, 10000);
	data = readl(mcu->efuse + EFUSE_DATA);
	if (gate & CLKGR0_EFUSE)
		writel(gate, mcu->cpm + CPM_CLKGR0);
	if (ret)
		return ret;
	if (data >> 28 != ((data >> 8) & 0xf) || data >> 28 >= 7)
		return -ENODEV;
	return 0;
}

static int c5_mcu_start(struct c5_mcu *mcu)
{
	const struct mcu_header *h = &mcu->hdr;
	u32 v;
	int ret;

	if (!h->load_addr)
		return -ENOEXEC;	/* nothing loaded */
	ret = c5_mcu_check_efuse(mcu);
	if (ret) {
		dev_err(mcu->misc.this_device, "efuse check failed: %d\n", ret);
		return ret;
	}
	writel(0, mcu->rv + RV_INTC_MASK_L);
	writel(0, mcu->rv + RV_INTC_MASK_H);
	writel(0, mcu->rv + RV_FROM_HOST);
	writel(0, mcu->rv + RV_TO_HOST);
	writel(h->entry, mcu->rv + RV_CRER);
	v = readl(mcu->rv + RV_CCSR);
	writel((v & ~BIT(5)) | BIT(1) | BIT(0), mcu->rv + RV_CCSR);

	/* Memory protection, in soc_mcu's order: regions 1-4 are top-of-range */
	writel(PMA_NAPOT | PMA_CACHED | PMA_W | PMA_R | PMA_X, mcu->rv + RV_PMA_CFG(0));
	writel(0x5ff, mcu->rv + RV_PMA_ADR(0));
	writel(h->shared_size ? PMA_TOR | PMA_CACHED | PMA_W | PMA_R | PMA_X : 0, mcu->rv + RV_PMA_CFG(1));
	writel(h->shared_addr >> 2, mcu->rv + RV_PMA_ADR(1));
	writel(h->shared_size ? PMA_TOR | PMA_W | PMA_R | PMA_X : 0, mcu->rv + RV_PMA_CFG(2));
	writel((h->shared_addr + h->shared_size) >> 2, mcu->rv + RV_PMA_ADR(2));
	writel(PMA_TOR | PMA_CACHED | PMA_W | PMA_R | PMA_X, mcu->rv + RV_PMA_CFG(3));
	writel(0x10000000 >> 2, mcu->rv + RV_PMA_ADR(3));
	writel(PMA_TOR | PMA_W | PMA_R | PMA_X, mcu->rv + RV_PMA_CFG(4));
	writel(0x20000000 >> 2, mcu->rv + RV_PMA_ADR(4));
	writel(0, mcu->rv + RV_PMA_CFG(5));
	writel(0, mcu->rv + RV_PMA_CFG(6));
	writel(0, mcu->rv + RV_PMA_CFG(7));

	c5_mcu_hold_reset(mcu, false);
	return 0;
}

static int c5_mcu_load(struct c5_mcu *mcu, const struct mcu_load *arg)
{
	const void __user *src = (const void __user *)(uintptr_t)arg->source;
	u32 magic, dst;

	if (arg->size >= 8 && !get_user(magic, (const u32 __user *)src + 1) &&
	    magic == MCU_HEADER_MAGIC && mcu->hdr.flag != MCU_HEADER_LOCKED) {
		if (copy_from_user(&mcu->hdr, src, sizeof(mcu->hdr)))
			return -EFAULT;
		c5_mcu_hold_reset(mcu, true);
	}
	if (!mcu->hdr.load_addr)
		return -ENOEXEC;	/* no header loaded yet */
	dst = mcu->hdr.load_addr + arg->offset;
	/* uncached through KSEG1, so the core sees it without cache maintenance */
	if (dst < mcu->hdr.load_addr || dst + arg->size < dst || dst + arg->size > 0x20000000)
		return -EINVAL;
	if (copy_from_user((void *)CKSEG1ADDR(dst), src, arg->size))
		return -EFAULT;
	return 0;
}

static long c5_mcu_ioctl(struct file *file, unsigned int cmd, unsigned long argp)
{
	struct c5_mcu *mcu = container_of(file->private_data, struct c5_mcu, misc);
	struct mcu_load arg;
	long ret = 0;

	/* soc_mcu reads all three words even though the ioctl number says int */
	if (cmd == MCU_LOAD && copy_from_user(&arg, (void __user *)argp, sizeof(arg)))
		return -EFAULT;

	mutex_lock(&mcu->lock);
	switch (cmd) {
	case MCU_STOP:
		c5_mcu_hold_reset(mcu, true);
		break;
	case MCU_RESET:
		c5_mcu_hold_reset(mcu, true);
		udelay(10);
		c5_mcu_hold_reset(mcu, false);
		break;
	case MCU_START:
		ret = c5_mcu_start(mcu);
		break;
	case MCU_LOAD:
		ret = c5_mcu_load(mcu, &arg);
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&mcu->lock);
	return ret;
}

static const struct file_operations c5_mcu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = c5_mcu_ioctl,
};

static int c5_mcu_probe(struct platform_device *pdev)
{
	struct c5_mcu *mcu;

	mcu = devm_kzalloc(&pdev->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;
	mcu->rv = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mcu->rv))
		return PTR_ERR(mcu->rv);
	/* shared with other drivers: not requested, only mapped */
	mcu->ccu = devm_ioremap(&pdev->dev, CCU_PHYS, 0x1000);
	mcu->cpm = devm_ioremap(&pdev->dev, CPM_PHYS, 0x100);
	mcu->efuse = devm_ioremap(&pdev->dev, EFUSE_PHYS, 0x100);
	if (!mcu->ccu || !mcu->cpm || !mcu->efuse)
		return -ENOMEM;
	mutex_init(&mcu->lock);
	mcu->misc.minor = MISC_DYNAMIC_MINOR;
	mcu->misc.name = "mcu";
	mcu->misc.fops = &c5_mcu_fops;
	mcu->misc.parent = &pdev->dev;
	platform_set_drvdata(pdev, mcu);
	return misc_register(&mcu->misc);
}

static int c5_mcu_remove(struct platform_device *pdev)
{
	struct c5_mcu *mcu = platform_get_drvdata(pdev);

	misc_deregister(&mcu->misc);
	return 0;
}

static const struct of_device_id c5_mcu_of_match[] = {
	{ .compatible = "ingenic,x2600-riscv-rproc" },
	{ }
};
MODULE_DEVICE_TABLE(of, c5_mcu_of_match);

static struct platform_driver c5_mcu_driver = {
	.probe = c5_mcu_probe,
	.remove = c5_mcu_remove,
	.driver = {
		.name = "c5-mcu",
		.of_match_table = c5_mcu_of_match,
	},
};
module_platform_driver(c5_mcu_driver);

MODULE_DESCRIPTION("X2600 RISC-V core loader (/dev/mcu, soc_mcu compatible)");
MODULE_LICENSE("GPL");
