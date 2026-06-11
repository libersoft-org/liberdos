#!/bin/sh
# ============================================================
# run.sh - launches LiberDOS in QEMU (Linux)
#
# Native counterpart of run.ps1 - no PowerShell needed.
# Run without arguments to print usage.
#
# Keyboard and mouse work in the window (PS/2 emulation).
# Click to grab the mouse, Ctrl+Alt+G releases it.
# ============================================================
set -e

root=$(cd "$(dirname "$0")" && pwd)
build=$root/build

usage() {
	cat << EOF
Usage: ./run.sh [--fdd floppy.img] [--hdd disk.img] [--log]

  --fdd floppy.img    attach a floppy image
  --hdd disk.img      attach a hard disk image
  --log               collect crash diagnostics (QEMU debug log,
					  guest serial output - in build/log)

At least one of --fdd / --hdd is required. The machine boots from
the floppy when one is attached, otherwise from the hard disk.
EOF
	exit 1
}

fdd=""
hdd=""
log=0
while [ $# -gt 0 ]; do
	case $1 in
	--fdd)
		[ -n "$2" ] || usage
		fdd=$2
		shift
		;;
	--hdd)
		[ -n "$2" ] || usage
		hdd=$2
		shift
		;;
	--log) log=1 ;;
	*) usage ;;
	esac
	shift
done

if [ -z "$fdd" ] && [ -z "$hdd" ]; then
	usage
fi

boot_dev=c
set --
if [ -n "$fdd" ]; then
	if [ ! -f "$fdd" ]; then
		echo "Missing $fdd - run ./build.sh first." >&2
		exit 1
	fi
	set -- "$@" -drive "format=raw,if=floppy,file=$fdd"
	boot_dev=a
fi
if [ -n "$hdd" ]; then
	if [ ! -f "$hdd" ]; then
		echo "Missing $hdd - run ./build.sh first." >&2
		exit 1
	fi
	set -- "$@" -drive "format=raw,file=$hdd"
fi

# Extra diagnostics for chasing QEMU crashes: guest_errors log,
# guest serial output (kernel mirrors the console to COM1) and
# a monitor socket. Host-side crash dumps come from the regular
# core dump mechanism (ulimit -c / coredumpctl), no extra tool
# needed.
if [ $log -eq 1 ]; then
	mkdir -p "$build/log"
	set -- "$@" -d guest_errors -D "$build/log/qemu.log" \
		-serial "file:$build/log/serial.log" \
		-monitor tcp:127.0.0.1:45454,server,nowait
fi

# Audio: PC speaker (PIT channel 2), Sound Blaster 16 at the
# conventional A220 I5 D1 H5, and an Adlib OPL2 at port 388h.
# Display must be SDL too: the default (GTK) display combined
# with SDL audio breaks VGA mode 13h output (black screen).
# VGA: cirrus with retrace=precise - some games busy-wait on
# the 3DAh retrace bits and hang with the default "dumb" retrace
# emulation; the std VGA model crashes QEMU 11 in precise mode.
# Accel: single-threaded TCG - the multi-threaded JIT corrupts a
# translation-block jump target under self-modifying game code
# and crashes QEMU itself (verified via crash dump, QEMU 11.0.0).
exec qemu-system-i386 \
	-accel tcg,thread=single \
	-vga cirrus,retrace=precise \
	-display sdl \
	-audiodev sdl,id=snd0,in.voices=0 \
	-machine pcspk-audiodev=snd0 \
	-device sb16,audiodev=snd0 \
	-device adlib,audiodev=snd0 \
	-boot $boot_dev \
	"$@"
