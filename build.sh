#!/bin/sh
# ============================================================
# build.sh - builds the OS binaries and configuration files
#
# Native counterpart of build.ps1 - no PowerShell needed.
#
# Produces build/bin (DOS binaries), build/boot (boot sectors),
# build/cfg (generated CONFIG.SYS/AUTOEXEC.BAT sources) and
# build/obj (intermediates). Disk images are assembled
# separately by image.sh.
#
# Toolchain: nasm from PATH, Open Watcom from $WATCOM (default
# ~/watcom; Linux host binaries binl64).
# ============================================================
set -e

# --- toolchain paths ---
WATCOM=${WATCOM:-$HOME/watcom}
if [ -d "$WATCOM/binl64" ]; then
	wbin=$WATCOM/binl64
else
	echo "Open Watcom not found - set WATCOM (looked in $WATCOM)" >&2
	exit 1
fi
export WATCOM
export PATH="$wbin:$PATH"
export INCLUDE="$WATCOM/h"

root=$(cd "$(dirname "$0")" && pwd)
src=$root/src
build=$root/build
bin=$build/bin
boot=$build/boot
obj=$build/obj
cfg=$build/cfg
mkdir -p "$build" "$bin" "$boot" "$obj" "$cfg"

# --- OS name/version: parsed from version.h (single source) ---
OSNAME=$(sed -n 's/.*OS_NAME[[:space:]]*"\([^"]*\)".*/\1/p' "$src/version.h")

# NASM include for boot.asm: BPB fields need fixed widths (8 / 11)
oem_id=$(printf '%-8.8s' "$OSNAME")
vol_lab=$(printf '%-11.11s' "$(printf '%s' "$OSNAME" | tr '[:lower:]' '[:upper:]')")
{
	printf "%%define OS_OEM '%s'\n" "$oem_id"
	printf "%%define OS_VOL '%s'\n" "$vol_lab"
} >"$obj/version.inc"

# --- 1. boot sector ---
nasm -f bin "$src/boot/boot.asm" -o "$boot/boot.bin" -I "$obj/"
nasm -f bin "$src/boot/mbr.asm" -o "$boot/mbr.bin"
nasm -f bin "$src/boot/boothdd.asm" -o "$boot/boothdd.bin"

# --- 2. kernel: NASM startup stub + OpenWatcom C, linked to a flat binary ---
nasm -f obj "$src/kernel/startup.asm" -o "$obj/startup.obj"

# small model, 386 instr, size-opt, no stack checks, no default libs
CFLAGS='-bt=dos -ms -3 -os -s -zl -zq -wx'
SOURCES='main console int21 disk fat file fcb clock proc util xms ems mouse share setver'
for name in $SOURCES; do
	wcc $CFLAGS "$src/kernel/$name.c" "-fo=$obj/$name.obj"
done

# wlink directive file (generated so paths are absolute); ORDER pins
# KSTART (entry + stubs) to offset 0 and the zero-initialized KSTACK
# behind _BSS, which makes wlink emit the BSS range into the binary.
# W1014 (no stack segment) is expected: the kernel is a raw binary
# and sets up its own stack in startup.asm.
{
	printf 'format raw bin\n'
	printf 'name %s/KERNEL.SYS\n' "$bin"
	printf 'option quiet\n'
	printf 'option map=%s/kernel.map\n' "$obj"
	printf 'disable 1014\n'
	printf 'order clname CODE clname KDATA clname DATA clname BSS clname KSTACK\n'
	for name in startup $SOURCES; do
		printf 'file %s/%s.obj\n' "$obj" "$name"
	done
} >"$obj/kernel.lnk"
wlink "@$obj/kernel.lnk"

# --- 3. COMMAND.COM: same tiny-layout trick, linked as .COM ---
nasm -f obj "$src/shell/cstart.asm" -o "$obj/cstart.obj"
wcc $CFLAGS "$src/shell/shell.c" "-fo=$obj/shell.obj"
{
	printf 'format dos com\n'
	printf 'name %s/COMMAND.COM\n' "$bin"
	printf 'option quiet\n'
	printf 'option map=%s/command.map\n' "$obj"
	printf 'file %s/cstart.obj\n' "$obj"
	printf 'file %s/shell.obj\n' "$obj"
} >"$obj/command.lnk"
wlink "@$obj/command.lnk"

# --- 3b. utilities: MORE, ATTRIB, CHKDSK (same .COM layout) ---
for util in more attrib chkdsk; do
	wcc $CFLAGS "$src/shell/$util.c" "-fo=$obj/$util.obj"
	U=$(printf '%s' "$util" | tr '[:lower:]' '[:upper:]')
	{
		printf 'format dos com\n'
		printf 'name %s/%s.COM\n' "$bin" "$U"
		printf 'option quiet\n'
		printf 'file %s/cstart.obj\n' "$obj"
		printf 'file %s/%s.obj\n' "$obj" "$util"
	} >"$obj/$util.lnk"
	wlink "@$obj/$util.lnk"
done

# --- 4. test programs ---
nasm -f bin "$src/tests/hello.asm" -o "$bin/HELLO.COM"
nasm -f obj "$src/tests/hellox.asm" -o "$obj/hellox.obj"
wlink format dos option quiet name "$bin/HELLOX.EXE" file "$obj/hellox.obj"
nasm -f bin "$src/tests/tsr.asm" -o "$bin/TSR.COM"
nasm -f bin "$src/tests/tests.asm" -o "$bin/TESTS.COM"
nasm -f bin "$src/tests/speaker.asm" -o "$bin/SPEAKER.COM"

# --- 5. DOS configuration files (CRLF, consumed by image.sh) ---
printf 'REM %s boot configuration\r\nFILES=20\r\nBUFFERS=20\r\nSHELL=A:\\COMMAND.COM\r\n' \
	"$OSNAME" >"$cfg/fdd-config.sys"
printf '@ECHO OFF\r\nECHO Welcome to %s!\r\nVER\r\n' "$OSNAME" >"$cfg/fdd-autoexec.bat"
printf 'REM %s boot configuration\r\nFILES=20\r\nBUFFERS=20\r\nSHELL=C:\\COMMAND.COM\r\n' \
	"$OSNAME" >"$cfg/hdd-config.sys"
{
	printf '@ECHO OFF\r\n'
	printf 'SET PATH=C:\\;C:\\DOS\r\n'
	printf 'SET BLASTER=A220 I5 D1 H5 T6\r\n'
	printf 'ECHO Welcome to %s! (booted from C:)\r\n' "$OSNAME"
	printf 'VER\r\n'
} >"$cfg/hdd-autoexec.bat"

echo "Build OK -> build/bin"
