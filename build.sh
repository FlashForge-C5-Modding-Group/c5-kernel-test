#!/bin/bash
# Builds a kernel for the FlashForge Creator 5 from Ingenic's X2600 kernel 6.6
# SDK (Linux 6.6.18-rt), with swap, zram and ieee754=relaxed. See README.md.
#
#   ./build.sh KERNEL_SRC TOOLCHAIN_DIR [make targets]
#
# KERNEL_SRC     kernel/kernel-6.6 from ingenic-linux-kernel6.6-x2600-v1.0-20250221
# TOOLCHAIN_DIR  the SDK's prebuilts/toolchains/mips-gcc720-glibc238 (contains bin/)
# OUT            build folder, by default ~/c5-kernel-out-66 (a Linux file system builds faster)
set -euo pipefail

KSRC=$(realpath "${1:?kernel source dir}")
TC=$(realpath "${2:?toolchain dir}")
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/c5-kernel-out-66}
DEFCONFIG=${DEFCONFIG:-x2600_halley7_linux_defconfig}
FRAGMENTS=$(ls "$HERE"/config/*.fragment)

die() { echo "build: $*" >&2; exit 1; }

grep -q '^VERSION = 6$' "$KSRC/Makefile" && grep -q '^PATCHLEVEL = 6$' "$KSRC/Makefile" ||
    die "$KSRC is not Linux 6.6"
# Ingenic's SDK toolchain is mips-linux-gnu-, their standalone one mips-xburst2-linux-gnu-
for prefix in mips-linux-gnu- mips-xburst2-linux-gnu-; do
    [ -x "$TC/bin/${prefix}gcc" ] && { CROSS=$prefix; break; }
done
[ -n "${CROSS:-}" ] || die "no mips-linux-gnu-gcc or mips-xburst2-linux-gnu-gcc in $TC/bin"

# Changes Ingenic's SDK lacks, applied once to the SDK tree. A patch with a
# NAME.skip-if file ("FILE TEXT") is left out when FILE already contains TEXT.
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    if [ -f "$p.skip-if" ]; then
        read -r skip_file skip_text < "$p.skip-if"
        grep -qF "$skip_text" "$KSRC/$skip_file" 2>/dev/null && continue
    fi
    if patch -d "$KSRC" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$KSRC" -p1 -s -N < "$p" || die "patch $(basename "$p") doesn't apply"
done
# The board's device tree (FlashForge's, plus 6.6 additions), selected by
# CONFIG_DT_X2600_MODULE_BASE_DTS_FILE in config/c5.fragment
cp "$HERE/dts/creator5_stock.dts" "$HERE/dts/creator5.dts" "$KSRC/module_drivers/dts/"
# Drivers of our own (hooked into Kconfig/Makefile by patches/)
cp "$HERE"/drivers/c5_mcu.c "$HERE"/drivers/c5_jz_pwm.c "$KSRC/module_drivers/drivers/misc/"
cp "$HERE"/drivers/panel-kx070.c "$KSRC/module_drivers/drivers/video/fbdev/ingenic/displays/"

export PATH=$TC/bin:$PATH ARCH=mips CROSS_COMPILE=$CROSS
mkdir -p "$OUT"
# Wi-Fi firmware built into the kernel (CONFIG_EXTRA_FIRMWARE in config/c5.fragment)
FW_URL=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtw88/rtw8821c_fw.bin
FW_SHA256=2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0
mkdir -p "$OUT/firmware/rtw88"
FW=$OUT/firmware/rtw88/rtw8821c_fw.bin
[ -f "$FW" ] || curl -fsSL -o "$FW" "$FW_URL" || die "can't download $FW_URL"
echo "$FW_SHA256  $FW" | sha256sum -c --quiet || die "$FW: unexpected checksum"
printf 'CONFIG_EXTRA_FIRMWARE_DIR="%s"\n' "$OUT/firmware" > "$OUT/firmware.fragment"

make -C "$KSRC" O="$OUT" "$DEFCONFIG" >/dev/null
# shellcheck disable=SC2086
"$KSRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" $FRAGMENTS "$OUT/firmware.fragment" >/dev/null
make -C "$KSRC" O="$OUT" olddefconfig >/dev/null

# olddefconfig silently drops options whose dependencies are unmet
lost=
for fragment in $FRAGMENTS; do
    while IFS= read -r line; do
        case $line in CONFIG_*) grep -qxF "$line" "$OUT/.config" || lost="$lost
  $line ($(basename "$fragment"))" ;; esac
    done < "$fragment"
done
[ -z "$lost" ] || die "config lost:$lost"

make -C "$KSRC" O="$OUT" -j"${JOBS:-$(nproc)}" "${@:-vmlinux}"

# c5_kexec.ko for this kernel (a module only loads into the kernel it was built
# for): $OUT/c5-kexec/c5_kexec.ko, installed as c5_kexec-<uname -r>.ko next to
# kexec/c5-kexec.sh
make -C "$KSRC" O="$OUT" -j"${JOBS:-$(nproc)}" modules
mkdir -p "$OUT/c5-kexec"
cp "$HERE"/kexec/c5_kexec_main.c "$HERE"/kexec/c5_kexec_tramp.S "$HERE"/kexec/Makefile "$OUT/c5-kexec/"
make -C "$KSRC" O="$OUT" M="$OUT/c5-kexec" modules
