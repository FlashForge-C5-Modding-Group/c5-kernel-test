#!/bin/sh
# Boot another kernel image once, from FlashForge's kernel 2.0.7, without
# flashing anything. A power cycle (or reboot) boots stock again.
#
#   c5-kexec.sh IMAGE          dry run: check everything, boot nothing
#   c5-kexec.sh --go IMAGE     boot IMAGE now (the printer drops off the network)
#
# c5_kexec.ko is looked for next to this script.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
MOD=$DIR/c5_kexec.ko
GO=
[ "${1:-}" = --go ] && { GO=1; shift; }
IMAGE=${1:?usage: c5-kexec.sh [--go] IMAGE}

die() { echo "c5-kexec: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
[ -f "$MOD" ] || die "no $MOD"
[ -f "$IMAGE" ] || die "no $IMAGE"
case "$(uname -r) $(ls /usr/prog/PROGRAM/kernel/ 2>/dev/null | head -n 1)" in
    "5.10.186+ 2.0.7") ;;
    *) die "only for FlashForge kernel 2.0.7 (this is $(uname -r))" ;;
esac
[ -e /proc/config.gz ] && die "already running a custom kernel"
grep -q '^c5_kexec ' /proc/modules && rmmod c5_kexec

# BusyBox insmod drops quotes, so spaces travel as | (the module turns them back)
grep -q '|' /proc/cmdline && die "/proc/cmdline contains |"
CMDLINE=$(tr ' ' '|' < /proc/cmdline)
if [ -z "$GO" ]; then
    dmesg -c > /dev/null
    insmod "$MOD" image="$IMAGE" cmdline="$CMDLINE" || { dmesg | grep c5_kexec; die "dry run failed"; }
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
insmod "$MOD" image="$IMAGE" cmdline="$CMDLINE" go=1
# Only reached if the module refused: bring the second core back
echo 1 > /sys/devices/system/cpu/cpu1/online
die "c5_kexec refused; see dmesg"
