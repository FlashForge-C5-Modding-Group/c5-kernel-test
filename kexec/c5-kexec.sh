#!/bin/sh
# Boot another kernel image once, without flashing anything, from any kernel
# c5_kexec.ko loads into (FlashForge's 2.0.x, c5-kernel 5.10 and 6.6).
# A power cycle (or reboot) boots stock again.
#
#   c5-kexec.sh IMAGE          dry run: check everything, boot nothing
#   c5-kexec.sh --go IMAGE     boot IMAGE now (the printer drops off the network)
#   --no-console               don't show boot messages on the touchscreen
#   C5_KEXEC_EXTRA="..."       more kernel options, appended to the command line
#
# c5_kexec-$(uname -r).ko is looked for next to this script (a module only loads
# into the kernel version it was built for), then c5_kexec.ko.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
MOD=$DIR/c5_kexec-$(uname -r).ko
[ -f "$MOD" ] || MOD=$DIR/c5_kexec.ko
GO= CONSOLE=1
while :; do
    case "${1:-}" in
        --go) GO=1; shift ;;
        --no-console) CONSOLE=; shift ;;
        *) break ;;
    esac
done
IMAGE=${1:?usage: c5-kexec.sh [--go] [--no-console] IMAGE}

die() { echo "c5-kexec: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
[ -f "$MOD" ] || die "no $MOD"
[ -f "$IMAGE" ] || die "no $IMAGE"
grep -q '^c5_kexec ' /proc/modules && rmmod c5_kexec

# The running kernel's own addresses; c5_kexec checks them against its printk
sym() { awk -v s="$1" '$3 == s { print "0x" $1; exit }' /proc/kallsyms; }
PRINTK=$(sym printk) KALLSYMS=$(sym kallsyms_lookup_name)
# printk is _printk from Linux 5.15 on
[ -n "$PRINTK" ] || PRINTK=$(sym _printk)
case "$PRINTK$KALLSYMS" in
    ""|*0x00000000*) die "no addresses in /proc/kallsyms (kptr_restrict?)" ;;
esac
echo "c5-kexec: running $(uname -r) $(uname -v)"

# BusyBox insmod drops quotes, so spaces travel as | (the module turns them back)
grep -q '|' /proc/cmdline && die "/proc/cmdline contains |"
# What an earlier c5-kexec boot (or a 6.6 kernel's built-in command line) added
# comes off, so it doesn't pile up
CMDLINE=$(sed -e 's/ console=tty0//g; s/ fbcon=[^ ]*//g; s/ ieee754=relaxed//g; s/ clk_ignore_unused//g; s/ panic=[0-9]*//g' /proc/cmdline)
if [ -n "$CONSOLE" ]; then
    # Boot messages on the touchscreen (the new kernel's framebuffer console), turned
    # like GrumpyScreen's display_rotate, which counts the other way round from
    # fbcon=rotate; the last console= becomes /dev/console
    ROTATE=$(sed -n 's/^display_rotate: *\([0-3]\).*/\1/p' /usr/data/opencreator/klipper/config/grumpyscreen.cfg \
        /usr/data/grumpyscreen/grumpyscreen.cfg 2>/dev/null | head -n 1)
    CMDLINE="$CMDLINE console=tty0 fbcon=rotate:$(( (4 - ${ROTATE:-1}) % 4 ))"
fi
CMDLINE="$CMDLINE${C5_KEXEC_EXTRA:+ $C5_KEXEC_EXTRA}"
CMDLINE=$(echo "$CMDLINE" | tr ' ' '|')
if [ -z "$GO" ]; then
    dmesg -c > /dev/null
    insmod "$MOD" image="$IMAGE" cmdline="$CMDLINE" printk_addr="$PRINTK" kallsyms_addr="$KALLSYMS" ||
        { dmesg | grep c5_kexec; die "dry run failed"; }
    dmesg | grep c5_kexec
    rmmod c5_kexec
    exit 0
fi

# Not during a print: the MCUs would lose Klipper mid-move
state=$(wget -q -O - "http://127.0.0.1:7125/printer/objects/query?print_stats=state" 2>/dev/null |
    sed -n 's/.*"state": *"\([a-z]*\)".*/\1/p')
[ "$state" = printing ] || [ "$state" = paused ] && die "a print is $state"

echo "c5-kexec: booting $IMAGE"
sync
echo 0 > /sys/devices/system/cpu/cpu1/online
sync
# Show the first of the framebuffer's buffers: a kernel without a driver for the
# panel (6.6) draws its console there through simple-framebuffer
echo 0,0 > /sys/class/graphics/fb0/pan 2>/dev/null || true
insmod "$MOD" image="$IMAGE" cmdline="$CMDLINE" printk_addr="$PRINTK" kallsyms_addr="$KALLSYMS" go=1
# Only reached if the module refused: bring the second core back
echo 1 > /sys/devices/system/cpu/cpu1/online
die "c5_kexec refused; see dmesg"
