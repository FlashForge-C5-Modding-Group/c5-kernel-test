# c5-kernel (test)

A Linux kernel for the FlashForge Creator 5 / Creator 5 Pro, built from
Ingenic's X2600 SDK, the same Linux 5.10.186 source FlashForge's kernel is
built from. It adds what FlashForge's kernel leaves out:

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

- **Configuration:** Ingenic's `x2600_module_base_linux_mmc0_defconfig` plus
  `config/*.fragment`. `config/stock.fragment` adds what FlashForge's kernel
  has built in beyond the defconfig (ext4, overlayfs, FUSE, exFAT, NTFS,
  codepages, EHCI/OHCI, CAN, virtio and more), found by comparing the
  functions in its symbol table with this build's `System.map`.
- **Device tree:** `dts/creator5_stock.dts`, decompiled from the device tree
  built into FlashForge's kernel (identical to `/sys/firmware/fdt` on a running
  printer). The one built into this kernel is byte-identical to theirs.
- **Version string:** `5.10.186+ SMP preempt mod_unload MIPS32_R5 32BIT`, the
  same as FlashForge's, so their driver modules (`/module_driver`,
  `/usr/prog/modules`) load. Every symbol those modules import is in this
  kernel.
- **`patches/0001-gpio-strength-slew-schmitt.patch`:** FlashForge added GPIO
  drive strength, slew rate and Schmitt trigger functions that their
  `soc_gpio.ko` needs; they're ported from Ingenic's kernel-6.6 SDK.

Known differences: FlashForge also changed a few drivers in ways the SDK
doesn't have, and those changes aren't here: the USB controller (`dwc2`), USB
PHY power, eMMC card detect, an RS485 timer in the serial driver, and drivers
for hardware the Creator 5 doesn't have (AD100 PHY, SDIO Wi-Fi modules).

## Building

On Linux (WSL2 works), with the SDK and toolchain unpacked (both are on
Ingenic's public FTP; see c5-usb-serial-modules for where):

```sh
./build.sh /path/to/ingenic-linux-kernel4.4.94-kernel5.10-x2600-v2.0-20230906/kernel/kernel-5.10 \
           /path/to/mips-xburst2-linux-toolchain-r6.0.0 xImage
```

It applies `patches/` to the SDK tree, copies the device tree in, configures
with the defconfig and every fragment (and stops if `olddefconfig` drops an
option), and builds into `$OUT` (`~/c5-kernel-out`). The image is
`arch/mips/boot/compressed/xImage`: a U-Boot uImage with load and entry
address `0x80F00000`, the same format as FlashForge's `xImage`.

## License

GPL-2.0, as the Linux kernel ([LICENSE](LICENSE)).
