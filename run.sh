#!/bin/sh
# ============================================================
# run.sh - launches the system in QEMU (Linux)
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
Usage: ./run.sh [--fdd floppy.img] [--hdd disk.img] [--log] [--vnc [N]] [--spice [port]]

  --fdd floppy.img    attach a floppy image
  --hdd disk.img      attach a hard disk image
  --log               collect crash diagnostics (QEMU debug log,
					  guest serial output - in build/log)
  --vnc [N]           headless mode for SSH sessions: no local
					  window, QEMU serves VNC display N (default 0,
					  i.e. port 5900+N on all interfaces) and audio
					  is disabled; connect with a VNC viewer
					  (no VNC authentication - LAN/dev use only)
  --spice [port]      headless mode with audio: QEMU serves the
					  display and sound over SPICE on the given port
					  (default 5930, all interfaces); connect with
					  virt-viewer / Remote Viewer
					  (no authentication - LAN/dev use only)

At least one of --fdd / --hdd is required. The machine boots from
the floppy when one is attached, otherwise from the hard disk.
EOF
	exit 1
}

fdd=""
hdd=""
log=0
vnc=""
spice=""
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
	--vnc)
		# optional display number (default 0)
		case $2 in
		'' | *[!0-9]*) vnc=0 ;;
		*)
			vnc=$2
			shift
			;;
		esac
		;;
	--spice)
		# optional port number (default 5930)
		case $2 in
		'' | *[!0-9]*) spice=5930 ;;
		*)
			spice=$2
			shift
			;;
		esac
		;;
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
#
# --vnc: headless variant for SSH sessions - VNC display instead
# of a local SDL window, audio routed to the null backend (no
# sound card / display on the host needed). Listens on all
# interfaces without authentication - LAN/dev use only.
#
# --spice: like --vnc but with sound - display and audio are
# served over the SPICE protocol (client: virt-viewer).
if [ -n "$spice" ]; then
	echo "SPICE server on port $spice (all interfaces, no auth)"
	set -- "$@" -display none \
		-spice "port=$spice,addr=0.0.0.0,disable-ticketing=on" \
		-audiodev spice,id=snd0
elif [ -n "$vnc" ]; then
	echo "VNC server on port $((5900 + vnc)) (all interfaces, no auth)"
	set -- "$@" -display "vnc=:$vnc" \
		-audiodev none,id=snd0
else
	set -- "$@" -display sdl \
		-audiodev sdl,id=snd0,in.voices=0
fi
exec qemu-system-i386 \
	-accel tcg,thread=single \
	-vga cirrus,retrace=precise \
	-machine pcspk-audiodev=snd0 \
	-device sb16,audiodev=snd0 \
	-device adlib,audiodev=snd0 \
	-boot $boot_dev \
	"$@"
