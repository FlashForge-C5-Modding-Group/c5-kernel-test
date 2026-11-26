# c5-kernel (test)

A Linux kernel for the FlashForge Creator 5 / Creator 5 Pro, built from
Ingenic's X2600 SDK v5.0 (Linux 5.10.186, the same kernel version FlashForge's
is built from). It adds what FlashForge's kernel leaves out:

- **Swap:** `CONFIG_SWAP`, zram (compressed swap in RAM) and swap files.
- **Both NaN encodings:** `ieee754=relaxed` is built into the command line, so
  legacy-NaN and 2008-NaN programs both run. This replaces OpenCreator's
  `01-fixnan.sh`, which writes into kernel memory at an address that changes
  with each FlashForge kernel.
- **`/proc/config.gz`**, so its configuration can be read back.
- usb-serial and USB Ethernet built in (see
  [c5-usb-serial-modules](https://github.com/FlashForge-C5-Modding-Group/c5-usb-serial-modules)).
- **Boot messages on the touchscreen** (framebuffer console), when booted
  through `kexec/` (below).
- **Fewer USB interrupts:** the USB controller interrupts the CPU about 17,700
  times a second on a C5, on stock as well (start-of-frame, the camera, the
  ACE's USB serial adapter). A longer wait before retrying NAKed split bulk
  reads cuts about 1,500 of them; see patch 0004.
- Less debug overhead: `SCHED_DEBUG` and `DEBUG_NOTIFIERS` off. (Options that
  change structures FlashForge's modules use, `DEBUG_SPINLOCK` and
  `DETECT_HUNG_TASK`, stay.)

**Experimental.** It has been booted on a printer through `kexec/` (below),
not flashed. Flashing a kernel can leave the printer unable to boot;
the way back is Ingenic's USB flasher over the mainboard's ports.

## How it matches FlashForge's kernel

FlashForge's kernel has no embedded configuration, so this one was matched to
theirs (kernel package 2.0.7) by comparison:

- **Source:** SDK v5.0 (`ingenic-linux-kernel5.10-x2600-v5.0-20251115`) has
  Ingenic's own versions of what FlashForge's kernel has beyond the older
  v2.0 SDK: the newer USB controller driver (`dwc2`), USB PHY power-up, eMMC
  card detect, the RS485 transmit-enable timer in the serial driver (the VDS
  dryer's port), and GPIO drive strength, slew rate and Schmitt trigger.
- **Configuration:** Ingenic's `x2600_module_base_linux_mmc0_defconfig` plus
  `config/*.fragment`. `config/stock.fragment` adds what FlashForge's kernel
  has built in beyond the defconfig (ext4, overlayfs, FUSE, exFAT, NTFS,
  codepages, EHCI/OHCI, CAN, virtio, the watchdog and more), found by
  comparing the functions in its symbol table with this build's `System.map`.
- **Device tree:** `dts/creator5_stock.dts`, decompiled from the device tree
  built into FlashForge's kernel (identical to `/sys/firmware/fdt` on a running
  printer). The one built into this kernel is byte-identical to theirs.
- **Version string:** `5.10.186+ SMP preempt mod_unload MIPS32_R5 32BIT`, the
  same as FlashForge's, so their driver modules (`/module_driver`,
  `/usr/prog/modules`) load. Every symbol those modules import is in this
  kernel.
- **`patches/`:** applied where the SDK lacks the change (a `.skip-if` file
  names what to look for), so the same patches work on SDK v2.0 too:
  - `0001-gpio-strength-slew-schmitt.patch`: GPIO functions FlashForge's
    `soc_gpio.ko` needs, ported from Ingenic's kernel-6.6 SDK. v5.0 has them.
  - `0002-uart-rs485-tx-enable.patch`: the RS485 transmit enable, for SDKs
    without it. v5.0 has Ingenic's own.
  - `0003-console-init-gpio.patch`: FlashForge's kernel sets GPIO PC1 and PC2
    to pull-up inputs at the end of `console_init`; done here too, so pin
    states match. Their console mute on PC6 is left out: this kernel's
    console always prints.
  - `0004-dwc2-split-bulk-in-nak-delay.patch`: dwc2 retries a split bulk IN
    transfer the device NAKs after 1 ms; a full-speed USB serial adapter behind
    the C5's hub (the ACE) then costs ~2,700 interrupts a second. This waits
    4 ms instead, adjustable at run time:
    `/sys/module/dwc2/parameters/split_bulk_in_nak_delay_us`. Measured: 18,257
    interrupts/s at 1 ms, ~16,700 at 4 ms, the ACE working normally.
  - `0005-fbdev-generic-drawing-ops.patch`: FlashForge's display driver (jzfb
    in their `soc_fb.ko`) has no drawing operations, so the framebuffer console
    would call NULL. Framebuffers without them get the generic memory-mapped
    ones (in a copy of the driver's fb_ops) and a default palette, and the
    console takes them over a second after they register, holding only the
    console lock: taking jzfb over during registration, with its fb_info lock
    held too, hangs.

Remaining differences in the symbol table: the watchdog's restart handler,
which Ingenic disabled in v5.0 (rebooting goes through `jz_wdt_restart` in
both kernels, which runs first), and functions the compiler inlined
differently.

## Building

On Linux (WSL2 works), with the SDK and toolchain unpacked. Both are on
Ingenic's public FTP (`ftp://ingenic_public:BFdg2f9B12@ftp.ingenic.com.cn`):
the SDK is
`DevSupport/X2600E/01_SW/09_kernel5.10_x2600-sdk_v5.0-20251115/01_ingenic-linux-kernel5.10-x2600-v5.0-20251115/`
(only its `kernel/kernel-5.10` is needed); see c5-usb-serial-modules for the
toolchain. Unpack on a Linux file system: on NTFS, the netfilter files whose
names differ only by case overwrite each other.

```sh
./build.sh /path/to/ingenic-linux-kernel5.10-x2600-v5.0-20251115/kernel/kernel-5.10 \
           /path/to/mips-xburst2-linux-toolchain-r6.0.0 xImage
```

It applies `patches/` to the SDK tree, copies the device tree in, configures
with the defconfig and every fragment (and stops if `olddefconfig` drops an
option), and builds into `$OUT` (`~/c5-kernel-out`). The image is
`arch/mips/boot/compressed/xImage`: a U-Boot uImage with load and entry
address `0x80F00000`, the same format as FlashForge's `xImage`.

## Booting it without flashing (`kexec/`), experimental

> [!WARNING]
> **Don't try this on your printer.** It's a proof of concept that has run on
> one printer. If the jump goes wrong the printer can freeze and need a power
> cycle, a running print is lost, and a bug in the module can crash the stock
> kernel. It only works on FlashForge kernel package 2.0.7.

FlashForge's kernel has no kexec (`sys_kexec_load` is an empty stub), so
`kexec/c5_kexec.ko` adds it for kernel 2.0.7 as a module. It boots another
kernel image from the running stock one, in RAM only: nothing on the eMMC
changes, and any reboot or power cycle boots stock again.

- **Dry run by default:** `c5-kexec.sh IMAGE` loads the module without `go`. It
  checks it's on kernel 2.0.7 (it calls unexported kernel functions at that
  kernel's addresses and refuses anywhere else), checks the uImage header and
  CRCs, stages the image in pages above 32 MB, logs the cache sizes and command
  line, then frees everything.
- **`c5-kexec.sh --go IMAGE`:** refuses during a print, syncs, takes the second
  core offline (Ingenic's SMP code then holds it in reset), shuts the drivers
  down as a reboot does (`kernel_restart_prepare`) and runs
  `c5_kexec_tramp.S` uncached: it copies the image to its load address, writes
  back and invalidates the L1 and L2 caches and jumps in with the current
  command line, as U-Boot would. The new kernel starts the second core itself.
- **Console:** the command line gets `console=tty0 fbcon=rotate:N`, N turned
  from GrumpyScreen's `display_rotate` (which counts the other way round), so
  boot messages scroll on the touchscreen. OpenCreator's UI scripts hand the
  screen back to the UI when it starts. `--no-console` leaves it out.
  `C5_KEXEC_EXTRA="..."` appends more options.
- If the new kernel panics it reboots into stock after 10 s; if it hangs, it
  needs a power cycle.

Tested with this repository's v5.0 build: both cores, all FlashForge modules,
Wi-Fi, screen, camera and Klipper (all MCUs) came up, zram swap worked, boot
messages showed on the touchscreen and the ACE answered. Not tested: the VDS
dryer (RS485), USB devices, printing.

OpenCreator's `01-fixnan.sh` skips custom kernels (`/proc/config.gz`) since
opencreator-fs `a4ffaa6`: before, it wrote a byte into kernel memory at an
address meant for the stock kernel.

Build (against a kernel tree configured like stock 2.0.7: the SDK v2.0
defconfig with `CONFIG_HIGHMEM=y` and `CONFIG_LOCALVERSION="+"`, then
`make modules_prepare`):

```sh
make -C /path/to/kernel-5.10 O=/path/to/stock-config-tree M=$PWD/kexec \
     ARCH=mips CROSS_COMPILE=mips-xburst2-linux-gnu- modules
```

## License

GPL-2.0, as the Linux kernel ([LICENSE](LICENSE)).
