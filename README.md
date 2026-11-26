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

**Untested on a printer.** Flashing a kernel can leave the printer unable to
boot; the way back is Ingenic's USB flasher over the mainboard's ports.

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

## License

GPL-2.0, as the Linux kernel ([LICENSE](LICENSE)).
