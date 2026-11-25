#!/bin/bash
# Builds a kernel for the FlashForge Creator 5 from Ingenic's X2600 SDK
# (Linux 5.10.186, the source FlashForge's kernel is built from), with swap,
# zram and ieee754=relaxed. See README.md.
#
#   ./build.sh KERNEL_SRC TOOLCHAIN_DIR [make targets]
#
# KERNEL_SRC     kernel/kernel-5.10 from ingenic-linux-kernel4.4.94-kernel5.10-x2600-v2.0-20230906
# TOOLCHAIN_DIR  mips-xburst2-linux-toolchain-r6.0.0 (contains bin/)
# OUT            build folder, by default ~/c5-kernel-out (a Linux file system builds faster)
set -euo pipefail

KSRC=$(realpath "${1:?kernel source dir}")
TC=$(realpath "${2:?toolchain dir}")
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/c5-kernel-out}
DEFCONFIG=${DEFCONFIG:-x2600_module_base_linux_mmc0_defconfig}
FRAGMENTS=$(ls "$HERE"/config/*.fragment)

die() { echo "build: $*" >&2; exit 1; }

grep -q '^SUBLEVEL = 186$' "$KSRC/Makefile" || die "$KSRC is not Linux 5.10.186"
[ -x "$TC/bin/mips-xburst2-linux-gnu-gcc" ] || die "no mips-xburst2-linux-gnu-gcc in $TC/bin"

# FlashForge's kernel has source changes Ingenic's SDK lacks; patches/ puts back
# what their modules need. Applied once to the SDK tree (a git checkout).
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    if patch -d "$KSRC" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$KSRC" -p1 -s -N < "$p" || die "patch $(basename "$p") doesn't apply"
done
# The board's device tree (FlashForge's, decompiled from their kernel), selected by
# CONFIG_DT_X2600_MODULE_BASE_DTS_FILE in config/c5.fragment
cp "$HERE/dts/creator5_stock.dts" "$KSRC/module_drivers/dts/creator5_stock.dts"

export PATH=$TC/bin:$PATH ARCH=mips CROSS_COMPILE=mips-xburst2-linux-gnu-
mkdir -p "$OUT"
make -C "$KSRC" O="$OUT" "$DEFCONFIG" >/dev/null
# shellcheck disable=SC2086
"$KSRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" $FRAGMENTS >/dev/null
make -C "$KSRC" O="$OUT" olddefconfig >/dev/null

# olddefconfig silently drops options whose dependencies are unmet
for fragment in $FRAGMENTS; do
    while IFS= read -r line; do
        case $line in CONFIG_*) grep -qxF "$line" "$OUT/.config" || die "config lost: $line" ;; esac
    done < "$fragment"
done

make -C "$KSRC" O="$OUT" -j"${JOBS:-$(nproc)}" "${@:-vmlinux}"
