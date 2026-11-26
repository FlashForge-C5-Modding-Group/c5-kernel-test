// SPDX-License-Identifier: GPL-2.0
/*
 * c5_kexec: boot another kernel from a Creator 5 kernel without kexec (FlashForge's
 * have none). Nothing on the eMMC changes; power-cycling boots stock again.
 *
 *   insmod c5_kexec.ko image=/path/xImage cmdline=a=1|b=2        dry run
 *   insmod c5_kexec.ko image=/path/xImage cmdline=a=1|b=2 go=1   boot it
 *
 * The image is a U-Boot uImage (FlashForge's xImage format). A dry run reads and
 * checks it, stages it in memory and logs what a boot would do, then frees
 * everything. go=1 needs the second core offline (it's then held in reset), shuts
 * the drivers down as a reboot does, and jumps through c5_kexec_tramp.
 *
 * It calls two kernel functions that aren't exported (kernel_restart_prepare,
 * migrate_to_reboot_cpu), found through kallsyms_lookup_name. Its address and
 * printk's come from the running kernel's /proc/kallsyms (c5-kexec.sh passes
 * them); printk's must match the module's own, or it refuses. Without them it
 * uses FlashForge kernel 2.0.7's addresses and refuses on any other kernel.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kernel_read_file.h>
#include <linux/crc32.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/cpumask.h>
#include <linux/irqflags.h>
#include <linux/reboot.h>
#include <asm/addrspace.h>
#include <asm/mipsregs.h>
#include <asm/page.h>
#include <linux/version.h>

/* Stock 2.0.7 (kernel package 2.0.7) addresses, from its /proc/kallsyms */
#define STOCK_PRINTK			0x8083a828UL
#define STOCK_KALLSYMS_LOOKUP_NAME	0x800dcc1cUL
/* printk became a macro around _printk in 5.15 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#define PRINTK_FN			_printk
#define PRINTK_SYM			"_printk"
#else
#define PRINTK_FN			printk
#define PRINTK_SYM			"printk"
#endif

/*
 * Everything below this physical address may be overwritten while the new kernel
 * starts: the uImage at its load address (16-21 MB for these images) and the
 * kernel it unpacks to 0x80010000. Staged pages, the trampoline and argv stay above.
 */
#define SAFE_PHYS	(32UL << 20)
#define MAX_IMAGE	(8UL << 20)	/* 2048 pages: what the page list holds */
#define MAX_REJECTS	65536

#define IH_MAGIC	0x27051956
#define IH_ARCH_MIPS	5
#define IH_TYPE_KERNEL	2
#define IH_COMP_NONE	0

struct uimage_header {
	__be32 magic, hcrc, time, size, load, ep, dcrc;
	u8 os, arch, type, comp;
	u8 name[32];
};

/* Read by c5_kexec_tramp at these offsets */
struct c5_kexec_args {
	u32 list, count, dest, entry, argc, argv;
	u32 dsize, dline, isize, iline, ssize, sline;
};

extern char c5_kexec_tramp[], c5_kexec_tramp_end[];

static char *image;
module_param(image, charp, 0);
MODULE_PARM_DESC(image, "uImage to boot (e.g. an xImage)");
static char *cmdline;
module_param(cmdline, charp, 0);
MODULE_PARM_DESC(cmdline, "kernel command line with | for each space (BusyBox insmod drops quotes)");
static bool go;
module_param(go, bool, 0);
MODULE_PARM_DESC(go, "boot the image instead of a dry run");
static unsigned long printk_addr, kallsyms_addr;
module_param(printk_addr, ulong, 0);
MODULE_PARM_DESC(printk_addr, "printk's address in /proc/kallsyms, to check kallsyms_addr is this kernel's");
module_param(kallsyms_addr, ulong, 0);
MODULE_PARM_DESC(kallsyms_addr, "kallsyms_lookup_name's address in /proc/kallsyms (default: kernel 2.0.7's)");

/*
 * Blocks taken from the allocator, kept or rejected (below SAFE_PHYS) until done.
 * Each entry is the block's address with its order (0 or 1) in bit 0.
 */
static unsigned long *kept, *rejects;
static unsigned int nkept, nrejects, maxkept;

static unsigned long safe_block(unsigned int order)
{
	while (nrejects < MAX_REJECTS) {
		unsigned long p = __get_free_pages(GFP_KERNEL, order);

		if (!p)
			return 0;
		if (virt_to_phys((void *)p) >= SAFE_PHYS && virt_to_phys((void *)p) < 0x20000000UL) {
			if (nkept == maxkept) {
				free_pages(p, order);
				return 0;
			}
			kept[nkept++] = p | order;
			return p;
		}
		rejects[nrejects++] = p | order;
	}
	return 0;
}

static void free_all(void)
{
	unsigned int i;

	for (i = 0; i < nrejects; i++)
		free_pages(rejects[i] & PAGE_MASK, rejects[i] & 1);
	nrejects = 0;
	if (go)
		return;	/* the image pages are needed, not freed, once booting starts */
	for (i = 0; i < nkept; i++)
		free_pages(kept[i] & PAGE_MASK, kept[i] & 1);
	nkept = 0;
}

/* L1 and L2 sizes from Config1/Config2, as the kernel probes them */
static int cache_geometry(struct c5_kexec_args *a)
{
	unsigned int c1 = read_c0_config1(), c2, sets, line, ways;

	sets = ((c1 >> 22) & 7) == 7 ? 32 : 64 << ((c1 >> 22) & 7);
	line = (c1 >> 19) & 7 ? 2 << ((c1 >> 19) & 7) : 0;
	ways = ((c1 >> 16) & 7) + 1;
	a->isize = sets * line * ways;
	a->iline = line;
	sets = ((c1 >> 13) & 7) == 7 ? 32 : 64 << ((c1 >> 13) & 7);
	line = (c1 >> 10) & 7 ? 2 << ((c1 >> 10) & 7) : 0;
	ways = ((c1 >> 7) & 7) + 1;
	a->dsize = sets * line * ways;
	a->dline = line;
	a->ssize = a->sline = 0;
	if (c1 & MIPS_CONF_M) {
		c2 = read_c0_config2();
		line = (c2 >> 4) & 15;
		if (line && line <= 7 && ((c2 >> 8) & 15) <= 7) {
			a->sline = 2 << line;
			a->ssize = (64 << ((c2 >> 8) & 15)) * a->sline * ((c2 & 15) + 1);
		}
	}
	return a->iline && a->dline && a->sline ? 0 : -ENODEV;
}

/* Write a range back from L1 and L2 to memory */
static void wback(unsigned long start, unsigned long len, unsigned int dline, unsigned int sline)
{
	unsigned long a;

	for (a = start & ~(unsigned long)(dline - 1); a < start + len; a += dline)
		__asm__ __volatile__(".set push\n.set mips32r2\ncache 0x15, 0(%0)\n.set pop" : : "r"(a));
	__asm__ __volatile__("sync" ::: "memory");
	for (a = start & ~(unsigned long)(sline - 1); a < start + len; a += sline)
		__asm__ __volatile__(".set push\n.set mips32r2\ncache 0x17, 0(%0)\n.set pop" : : "r"(a));
	__asm__ __volatile__("sync" ::: "memory");
}

static int __init c5_kexec_init(void)
{
	unsigned long (*lookup)(const char *);
	void (*restart_prepare)(char *);
	void (*migrate)(void);
	struct uimage_header h;
	struct c5_kexec_args *args;
	void *buf = NULL;
	size_t fsize = 0;
	unsigned long tramp, list, load, size, off, p;
	unsigned int i, npages, tramp_len = c5_kexec_tramp_end - c5_kexec_tramp;
	char **argv, *str;
	int ret;

	/* kallsyms_lookup_name: given (checked through printk) or kernel 2.0.7's */
	if (kallsyms_addr) {
		if (printk_addr != (unsigned long)PRINTK_FN) {
			pr_err("c5_kexec: printk_addr %lx isn't this kernel's printk (%px), refusing\n",
			       printk_addr, PRINTK_FN);
			return -EINVAL;
		}
		lookup = (void *)kallsyms_addr;
	} else if ((unsigned long)PRINTK_FN == STOCK_PRINTK) {
		lookup = (void *)STOCK_KALLSYMS_LOOKUP_NAME;
	} else {
		pr_err("c5_kexec: not kernel 2.0.7: pass printk_addr= and kallsyms_addr= from /proc/kallsyms\n");
		return -ENODEV;
	}
	if (lookup(PRINTK_SYM) != (unsigned long)PRINTK_FN) {
		pr_err("c5_kexec: kallsyms_lookup_name doesn't find printk, refusing\n");
		return -ENODEV;
	}
	restart_prepare = (void *)lookup("kernel_restart_prepare");
	migrate = (void *)lookup("migrate_to_reboot_cpu");
	if (!restart_prepare || !migrate) {
		pr_err("c5_kexec: kernel_restart_prepare or migrate_to_reboot_cpu not found, refusing\n");
		return -ENODEV;
	}
	pr_info("c5_kexec: running kernel: kernel_restart_prepare %px, migrate_to_reboot_cpu %px\n",
		restart_prepare, migrate);
	if (!image || !cmdline) {
		pr_err("c5_kexec: image= and cmdline= are required\n");
		return -EINVAL;
	}
	if (strlen(cmdline) > 2048) {
		pr_err("c5_kexec: cmdline too long\n");
		return -EINVAL;
	}

	ret = kernel_read_file_from_path(image, 0, &buf, MAX_IMAGE, &fsize, READING_KEXEC_IMAGE);
	if (ret < 0) {
		pr_err("c5_kexec: can't read %s: %d\n", image, ret);
		return ret;
	}
	ret = -EINVAL;
	if (fsize < sizeof(h)) {
		pr_err("c5_kexec: %s is too small for a uImage\n", image);
		goto out;
	}
	memcpy(&h, buf, sizeof(h));
	size = be32_to_cpu(h.size);
	load = be32_to_cpu(h.load);
	if (be32_to_cpu(h.magic) != IH_MAGIC || h.arch != IH_ARCH_MIPS || h.type != IH_TYPE_KERNEL ||
	    h.comp != IH_COMP_NONE) {
		pr_err("c5_kexec: %s is not an uncompressed MIPS kernel uImage\n", image);
		goto out;
	}
	i = be32_to_cpu(h.hcrc);
	((struct uimage_header *)buf)->hcrc = 0;
	if ((crc32_le(~0, buf, sizeof(h)) ^ ~0) != i) {
		pr_err("c5_kexec: header CRC mismatch\n");
		goto out;
	}
	if (sizeof(h) + size > fsize ||
	    (crc32_le(~0, (u8 *)buf + sizeof(h), size) ^ ~0) != be32_to_cpu(h.dcrc)) {
		pr_err("c5_kexec: image data size or CRC mismatch\n");
		goto out;
	}
	/* Load address in KSEG0, the image and its BSS (1 MB allowance) below SAFE_PHYS */
	if (load < 0x80100000UL || CPHYSADDR(load) + size + (1UL << 20) > SAFE_PHYS ||
	    be32_to_cpu(h.ep) < load || be32_to_cpu(h.ep) >= load + size) {
		pr_err("c5_kexec: load %08lx / entry %08x / size %lu outside the safe range\n",
		       load, be32_to_cpu(h.ep), size);
		goto out;
	}

	npages = DIV_ROUND_UP(size, PAGE_SIZE);
	maxkept = npages + 4;
	ret = -ENOMEM;
	kept = kcalloc(maxkept, sizeof(*kept), GFP_KERNEL);
	rejects = vmalloc(MAX_REJECTS * sizeof(*rejects));
	if (!kept || !rejects)
		goto out;

	/* Trampoline page (code, then args, argv and the command line) and the page
	 * list: one KSEG0 address per image page, 2048 in an order-1 block */
	tramp = safe_block(0);
	list = safe_block(1);
	if (!tramp || !list) {
		pr_err("c5_kexec: no memory above %lu MB for the trampoline and page list\n",
		       SAFE_PHYS >> 20);
		goto out;
	}
	for (i = 0; i < npages; i++) {
		p = safe_block(0);
		if (!p) {
			pr_err("c5_kexec: out of memory above %lu MB at page %u of %u\n",
			       SAFE_PHYS >> 20, i, npages);
			goto out;
		}
		off = (unsigned long)i * PAGE_SIZE;
		memcpy((void *)p, (u8 *)buf + sizeof(h) + off, min(PAGE_SIZE, size - off));
		((u32 *)list)[i] = p;
	}

	memcpy((void *)tramp, c5_kexec_tramp, tramp_len);
	args = (void *)ALIGN(tramp + tramp_len, 16);
	argv = (char **)(args + 1);
	str = (char *)(argv + 4);
	strcpy(str, "c5_kexec");
	argv[0] = str;
	argv[1] = str + 16;
	strcpy(argv[1], cmdline);
	strreplace(argv[1], '|', ' ');
	argv[2] = NULL;
	args->list = list;
	args->count = npages;
	args->dest = load;
	args->entry = be32_to_cpu(h.ep);
	args->argc = 2;
	args->argv = (u32)argv;
	ret = cache_geometry(args);
	pr_info("c5_kexec: %s: \"%.32s\", %lu bytes, load %08lx, entry %08x, %u pages staged above %lu MB (%u lower pages skipped)\n",
		image, h.name, size, load, args->entry, npages, SAFE_PHYS >> 20, nrejects);
	pr_info("c5_kexec: caches: L1I %u/%u, L1D %u/%u, L2 %u/%u (size/line); trampoline %u bytes at phys %08lx\n",
		args->isize, args->iline, args->dsize, args->dline, args->ssize, args->sline,
		tramp_len, (unsigned long)virt_to_phys((void *)tramp));
	pr_info("c5_kexec: command line: %s\n", argv[1]);
	if (ret) {
		pr_err("c5_kexec: cache geometry not found, refusing\n");
		goto out;
	}
	vfree(buf);
	buf = NULL;

	if (!go) {
		pr_info("c5_kexec: dry run OK; load with go=1 (second core offline) to boot it\n");
		ret = 0;
		goto out;
	}
	if (num_online_cpus() != 1 || cpu_online(1)) {
		pr_err("c5_kexec: take the second core offline first (echo 0 > /sys/devices/system/cpu/cpu1/online)\n");
		go = false;
		ret = -EBUSY;
		goto out;
	}

	pr_emerg("c5_kexec: shutting down devices and booting %s\n", image);
	free_all();
	restart_prepare(NULL);	/* reboot notifiers, device shutdown, as kernel_kexec() */
	migrate();
	local_irq_disable();
	/* The trampoline runs uncached: put it, its arguments and the list in memory */
	wback(tramp, PAGE_SIZE, args->dline, args->sline);
	wback(list, 2 * PAGE_SIZE, args->dline, args->sline);
	((void (*)(struct c5_kexec_args *))CKSEG1ADDR(tramp))(args);
	return -EIO;	/* not reached */

out:
	free_all();
	kfree(kept);
	vfree(rejects);
	vfree(buf);
	return ret;
}

static void __exit c5_kexec_exit(void)
{
}

module_init(c5_kexec_init);
module_exit(c5_kexec_exit);
MODULE_DESCRIPTION("Boot another kernel from FlashForge Creator 5 kernel 2.0.7");
MODULE_LICENSE("GPL");
