#!/bin/sh
# ============================================================
# image.sh - builds bootable disk images from build artifacts
#
# Native counterpart of image.ps1 - same argument syntax.
# Run ./build.sh first; this script only assembles the images
# from build/bin, build/boot and build/cfg.
#
# FAT images are built with mtools (mformat, mcopy) and dd:
# the floppy (FAT12) and the hard disk (FAT16) share the
# DEST=SRC content injection below and differ only in the
# geometry and the boot blocks.
# ============================================================
set -e

root=$(cd "$(dirname "$0")" && pwd)
bin=$root/build/bin
boot=$root/build/boot
cfg=$root/build/cfg

export MTOOLS_SKIP_CHECK=1

usage() {
	cat <<'EOF'
Usage: ./image.sh [--fdd floppy.img] [--hdd disk.img] [--size N[K|M|G]] [--add directory]...

  --fdd floppy.img    create a bootable 1.44 MB FAT12 floppy image
  --hdd disk.img      create a bootable FAT16 hard disk image
  --fat32             make the --hdd image FAT32 instead of FAT16
  --size N[K|M|G]     hard disk image size (with --hdd only);
					  a plain number is bytes, the K/M/G suffix
					  selects 1024-based units (default 32M)
  --add directory     copy the directory's contents onto the hard
					  disk image root (with --hdd only; may be
					  repeated)
  --help              show this help

At least one of --fdd / --hdd is required; with both, both
images are created.
EOF
	exit 1
}

fdd=
hdd=
size=
fat32=
add_dirs=
while [ $# -gt 0 ]; do
	case $1 in
	--fdd)
		[ $# -ge 2 ] || usage
		fdd=$2
		shift
		;;
	--hdd)
		[ $# -ge 2 ] || usage
		hdd=$2
		shift
		;;
	--fat32)
		fat32=1
		;;
	--size)
		[ $# -ge 2 ] || usage
		size=$2
		shift
		;;
	--add)
		[ $# -ge 2 ] || usage
		# newline-separated list (POSIX sh has no arrays)
		add_dirs="$add_dirs$2
"
		shift
		;;
	*)
		usage
		;;
	esac
	shift
done

[ -n "$fdd" ] || [ -n "$hdd" ] || usage
if [ -n "$size" ] && [ -z "$hdd" ]; then
	echo "--size is only valid together with --hdd" >&2
	exit 1
fi
if [ -n "$add_dirs" ] && [ -z "$hdd" ]; then
	echo "--add is only valid together with --hdd" >&2
	exit 1
fi
oldifs=$IFS
IFS='
'
for d in $add_dirs; do
	if [ ! -d "$d" ]; then
		echo "--add: not a directory: $d" >&2
		exit 1
	fi
done
IFS=$oldifs

# --size: plain number = bytes, K/M/G suffix = 1024-based units
size_bytes=33554432 # 32M
if [ -n "$size" ]; then
	case $size in
	*[Kk]) num=${size%?} mult=1024 ;;
	*[Mm]) num=${size%?} mult=1048576 ;;
	*[Gg]) num=${size%?} mult=1073741824 ;;
	*) num=$size mult=1 ;;
	esac
	case $num in
	'' | *[!0-9]*)
		echo "Bad size '$size' - expected a number with an optional K/M/G suffix." >&2
		exit 1
		;;
	esac
	size_bytes=$((num * mult))
fi

if [ ! -f "$bin/KERNEL.SYS" ]; then
	echo "Missing $bin/KERNEL.SYS - run ./build.sh first." >&2
	exit 1
fi

# ============================================================
# shared helpers
# ============================================================

# require_sector <file> - fail unless the file is exactly 512 bytes
require_sector() {
	if [ "$(wc -c <"$1")" -ne 512 ]; then
		echo "boot sector must be exactly 512 bytes: $1" >&2
		exit 1
	fi
}

# inject_specs <mtools-drive> DEST=SRC... - copy host content
# onto the image: DEST is the path on the image (parent
# directories are created as needed), SRC a host file or
# directory (directories are copied recursively)
inject_specs() {
	drive=$1
	shift
	for spec in "$@"; do
		dest=${spec%%=*}
		src=${spec#*=}
		if [ -z "$dest" ] || [ "$dest" = "$spec" ]; then
			echo "bad file spec, expected DEST=SRC: $spec" >&2
			exit 1
		fi
		case $dest in
		*/*)
			# create parent directories on the image as needed;
			# -D sS skips existing ones and stdin is detached so
			# mtools can never show an interactive clash prompt
			parent=${dest%/*}
			path=""
			oldifs=$IFS
			IFS=/
			for d in $parent; do
				path=$path$d
				mmd -D sS -i "$drive" "::$path" </dev/null 2>/dev/null || true
				path=$path/
			done
			IFS=$oldifs
			;;
		esac
		if [ -d "$src" ]; then
			mcopy -s -i "$drive" "$src" "::$dest" </dev/null
		else
			mcopy -i "$drive" "$src" "::$dest" </dev/null
		fi
	done
}

# ============================================================
# image types
# ============================================================

# make_floppy <output> DEST=SRC... - 1.44 MB FAT12 floppy with
# geometry matching the BPB carried by the boot sector
# (src/boot/boot.asm): 512 B/sector, 1 reserved sector, 2 FATs
# x 9 sectors, 224 root entries, 2880 sectors
make_floppy() {
	out=$1
	shift
	require_sector "$boot/boot.bin"

	# empty image + boot sector (carries the full BPB)
	dd if=/dev/zero of="$out" bs=512 count=2880 2>/dev/null
	dd if="$boot/boot.bin" of="$out" conv=notrunc 2>/dev/null

	# seed both FAT copies (sectors 1 and 10):
	# FAT[0] = media descriptor (0xF0), FAT[1] = end-of-chain
	for fatsec in 1 10; do
		printf '\360\377\377' |
			dd of="$out" bs=1 seek=$((fatsec * 512)) conv=notrunc 2>/dev/null
	done

	inject_specs "$out" "$@"
	echo "$(basename "$out"): $# file(s)"
}

# make_hdd <output> <size-bytes> DEST=SRC... - FAT16 hard disk:
# MBR with one active type 06h partition at LBA 63; the
# sectors-per-cluster value follows the DOS FAT16 table and the
# FAT size is derived from the partition size. mformat patches
# the real BPB values into the boot code (src/boot/boothdd.asm).
make_hdd() {
	out=$1
	hdd_size=$2
	shift 2
	require_sector "$boot/mbr.bin"
	if [ -n "$fat32" ]; then
		require_sector "$boot/boothdd32.bin"
		bootcode=$boot/boothdd32.bin
		ptype=12  # 0x0C: FAT32 LBA partition
		drvoff=64 # BPB drive-number offset (FAT32 EBPB)
	else
		require_sector "$boot/boothdd.bin"
		bootcode=$boot/boothdd.bin
		ptype=6   # 0x06: FAT16B partition
		drvoff=36 # BPB drive-number offset (FAT16 EBPB)
	fi

	if [ "$hdd_size" -ge 2147483648 ]; then
		echo "disk size must be under 2 GB: $hdd_size" >&2
		exit 1
	fi
	total_secs=$(((hdd_size + 511) / 512))
	part_start=63
	part_secs=$((total_secs - part_start))
	part_off=$((part_start * 512))

	# sectors per cluster: FAT32 needs >= 65525 clusters, so use
	# small clusters; FAT16 follows the classic DOS size table
	if [ -n "$fat32" ]; then
		if [ "$hdd_size" -le 268435456 ]; then
			spc=1
		elif [ "$hdd_size" -le 536870912 ]; then
			spc=2
		elif [ "$hdd_size" -le 1073741824 ]; then
			spc=4
		else
			spc=8
		fi
	elif [ "$hdd_size" -le 134217728 ]; then
		spc=4
	elif [ "$hdd_size" -le 268435456 ]; then
		spc=8
	elif [ "$hdd_size" -le 536870912 ]; then
		spc=16
	elif [ "$hdd_size" -le 1073741824 ]; then
		spc=32
	else
		spc=64
	fi
	spf=$(((part_secs - 1 - 32 + 256 * spc + 1) / (256 * spc + 2)))
	clusters=$(((part_secs - 1 - 32 - 2 * spf) / spc))
	if [ -z "$fat32" ] && [ "$clusters" -lt 4085 ]; then
		echo "disk too small for FAT16 - use at least 9M: $hdd_size" >&2
		exit 1
	fi

	# OS name from version.h (single source); pad to BPB widths
	osname=$(sed -n 's/.*OS_NAME[[:space:]]*"\([^"]*\)".*/\1/p' "$root/src/version.h")
	oem_id=$(printf '%-8.8s' "$osname")
	vol_lab=$(printf '%-11.11s' "$(printf '%s-HD' "$osname" | tr '[:lower:]' '[:upper:]')")

	# empty image + MBR bootstrap code (bytes 0..445)
	dd if=/dev/zero of="$out" bs=512 count=$total_secs 2>/dev/null
	dd if="$boot/mbr.bin" of="$out" bs=1 count=446 conv=notrunc 2>/dev/null

	# partition table entry 0 + boot signature: active, CHS start
	# dummy (0/1/1), type 06h (FAT16B), CHS end capped (FE/FF/FF),
	# LBA start 63, LBA size = part_secs (little-endian)
	{
		printf '\200\001\001\000'
		printf "\\$(printf '%03o' "$ptype")"
		printf '\376\377\377\077\000\000\000'
		for shift_by in 0 8 16 24; do
			# shellcheck disable=SC2059
			printf "\\$(printf '%03o' $(((part_secs >> shift_by) & 255)))"
		done
	} | dd of="$out" bs=1 seek=446 conv=notrunc 2>/dev/null
	printf '\125\252' | dd of="$out" bs=1 seek=510 conv=notrunc 2>/dev/null

	# format the partition; mformat overwrites the BPB inside the
	# supplied boot sector code with the values derived from these
	# parameters (-F forces FAT32 for the FAT32 boot sector)
	if [ -n "$fat32" ]; then
		mformat -i "$out@@$part_off" \
			-T $part_secs -h 16 -s 63 -H $part_start \
			-c $spc -d 2 -m 0xf8 -F \
			-B "$bootcode" -v "$vol_lab"
	else
		mformat -i "$out@@$part_off" \
			-T $part_secs -h 16 -s 63 -H $part_start \
			-c $spc -d 2 -r 32 -m 0xf8 \
			-B "$bootcode" -v "$vol_lab"
	fi

	# BPB details mformat does not control: OEM id (offset 3,
	# 8 bytes) and BIOS drive number 80h (offset 36)
	printf '%s' "$oem_id" |
		dd of="$out" bs=1 seek=$((part_off + 3)) conv=notrunc 2>/dev/null
	printf '\200' |
		dd of="$out" bs=1 seek=$((part_off + drvoff)) conv=notrunc 2>/dev/null

	inject_specs "$out@@$part_off" "$@"
	echo "$(basename "$out"): $# item(s)"
}

# ============================================================
# image contents
# ============================================================

# --- floppy image (drive A:) ---
if [ -n "$fdd" ]; then
	mkdir -p "$(dirname "$fdd")"
	make_floppy "$fdd" \
		"KERNEL.SYS=$bin/KERNEL.SYS" \
		"COMMAND.COM=$bin/COMMAND.COM" \
		"MORE.COM=$bin/MORE.COM" \
		"ATTRIB.COM=$bin/ATTRIB.COM" \
		"CHKDSK.COM=$bin/CHKDSK.COM" \
		"CONFIG.SYS=$cfg/fdd-config.sys" \
		"AUTOEXEC.BAT=$cfg/fdd-autoexec.bat"
	echo "Image OK -> $fdd"
fi

# --- hard disk image (drive C:) ---
# System files in the root, utilities in DOS/ (on PATH via
# AUTOEXEC.BAT), test programs in TEST/, extra payload
# directories from --add (copied recursively).
if [ -n "$hdd" ]; then
	set -- \
		"KERNEL.SYS=$bin/KERNEL.SYS" \
		"COMMAND.COM=$bin/COMMAND.COM" \
		"CONFIG.SYS=$cfg/hdd-config.sys" \
		"AUTOEXEC.BAT=$cfg/hdd-autoexec.bat" \
		"DOS/MORE.COM=$bin/MORE.COM" \
		"DOS/ATTRIB.COM=$bin/ATTRIB.COM" \
		"DOS/CHKDSK.COM=$bin/CHKDSK.COM" \
		"TEST/TESTS.COM=$bin/TESTS.COM" \
		"TEST/SPEAKER.COM=$bin/SPEAKER.COM" \
		"TEST/HELLO.COM=$bin/HELLO.COM" \
		"TEST/TSR.COM=$bin/TSR.COM" \
		"TEST/HELLOX.EXE=$bin/HELLOX.EXE"
	# The contents of each --add directory land in the root of C:
	# (game payloads and other extra content).
	if [ -n "$add_dirs" ]; then
		oldifs=$IFS
		IFS='
'
		for d in $add_dirs; do
			d=${d%/}
			for entry in "$d"/*; do
				[ -e "$entry" ] || continue
				set -- "$@" "$(basename "$entry")=$entry"
			done
		done
		IFS=$oldifs
	fi
	mkdir -p "$(dirname "$hdd")"
	make_hdd "$hdd" "$size_bytes" "$@"
	echo "Image OK -> $hdd"
fi
