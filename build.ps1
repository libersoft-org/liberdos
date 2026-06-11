# ============================================================
# build.ps1 - builds the OS binaries and configuration files
#
# Produces build\bin (DOS binaries), build\boot (boot sectors),
# build\cfg (generated CONFIG.SYS/AUTOEXEC.BAT sources) and
# build\obj (intermediates). Disk images are assembled
# separately by image.ps1.
# ============================================================
$ErrorActionPreference = 'Stop'

# --- toolchain paths ---
$NASM   = 'C:\Program Files\NASM\nasm.exe'
$WATCOM = 'C:\Programy\watcom'
$WCC    = "$WATCOM\binnt64\wcc.exe"
$WLINK  = "$WATCOM\binnt64\wlink.exe"

$env:WATCOM  = $WATCOM
$env:PATH    = "$WATCOM\binnt64;$env:PATH"
$env:INCLUDE = "$WATCOM\h"

$root  = $PSScriptRoot
$src   = Join-Path $root 'src'
$build = Join-Path $root 'build'
$bin   = Join-Path $build 'bin'
$boot  = Join-Path $build 'boot'
$obj   = Join-Path $build 'obj'
$cfg   = Join-Path $build 'cfg'
New-Item -ItemType Directory -Force -Path $build, $bin, $boot, $obj, $cfg | Out-Null

# --- OS name/version: parsed from version.h (single source) ---
$verH   = Get-Content "$src\version.h" -Raw
$OSNAME = [regex]::Match($verH, 'OS_NAME\s+"([^"]+)"').Groups[1].Value
$OSVER  = [regex]::Match($verH, 'OS_VERSION\s+"([^"]+)"').Groups[1].Value

# NASM include for boot.asm: BPB fields need fixed widths (8 / 11)
$OemId  = $OSNAME.PadRight(8).Substring(0, 8)
$VolLab = $OSNAME.ToUpper().PadRight(11).Substring(0, 11)
@"
%define OS_OEM '$OemId'
%define OS_VOL '$VolLab'
"@ | Set-Content -Path "$obj\version.inc" -Encoding ascii

# --- 1. boot sector ---
& $NASM -f bin "$src\boot\boot.asm" -o "$boot\boot.bin" -I "$obj\"
if ($LASTEXITCODE) { exit 1 }
& $NASM -f bin "$src\boot\mbr.asm" -o "$boot\mbr.bin"
if ($LASTEXITCODE) { exit 1 }
& $NASM -f bin "$src\boot\boothdd.asm" -o "$boot\boothdd.bin" -I "$obj/"
if ($LASTEXITCODE) { exit 1 }

# --- 2. kernel: NASM startup stub + OpenWatcom C, linked to a flat binary ---
& $NASM -f obj "$src\kernel\startup.asm" -o "$obj\startup.obj"
if ($LASTEXITCODE) { exit 1 }

# small model, 386 instr, size-opt, no stack checks, no default libs
$CFLAGS = '-bt=dos', '-ms', '-3', '-os', '-s', '-zl', '-zq', '-wx'
$SOURCES = @('main', 'console', 'int21', 'disk', 'fat', 'file', 'fcb', 'clock', 'proc', 'util', 'xms', 'ems', 'mouse', 'share', 'setver')
foreach ($name in $SOURCES) {
	& $WCC @CFLAGS "$src\kernel\$name.c" "-fo=$obj\$name.obj"
	if ($LASTEXITCODE) { exit 1 }
}

# wlink directive file (generated so paths are absolute); ORDER pins
# KSTART (entry + stubs) to offset 0 and the zero-initialized KSTACK
# behind _BSS, which makes wlink emit the BSS range into the binary.
# W1014 (no stack segment) is expected: the kernel is a raw binary
# and sets up its own stack in startup.asm.
$fileLines = (@('startup') + $SOURCES | ForEach-Object { "file $obj\$_.obj" }) -join "`n"
@"
format raw bin
name $bin\KERNEL.SYS
option quiet
option map=$obj\kernel.map
disable 1014
order clname CODE clname KDATA clname DATA clname BSS clname KSTACK
$fileLines
"@ | Set-Content -Path "$obj\kernel.lnk" -Encoding ascii
& $WLINK "@$obj\kernel.lnk"
if ($LASTEXITCODE) { exit 1 }

# --- 3. COMMAND.COM: same tiny-layout trick, linked as .COM ---
& $NASM -f obj "$src\shell\cstart.asm" -o "$obj\cstart.obj"
if ($LASTEXITCODE) { exit 1 }
& $WCC @CFLAGS "$src\shell\shell.c" "-fo=$obj\shell.obj"
if ($LASTEXITCODE) { exit 1 }
@"
format dos com
name $bin\COMMAND.COM
option quiet
option map=$obj\command.map
file $obj\cstart.obj
file $obj\shell.obj
"@ | Set-Content -Path "$obj\command.lnk" -Encoding ascii
& $WLINK "@$obj\command.lnk"
if ($LASTEXITCODE) { exit 1 }

# --- 3b. utilities: MORE, ATTRIB, CHKDSK (same .COM layout) ---
foreach ($util in @('more', 'attrib', 'chkdsk')) {
	& $WCC @CFLAGS "$src\shell\$util.c" "-fo=$obj\$util.obj"
	if ($LASTEXITCODE) { exit 1 }
	$U = $util.ToUpper()
	@"
format dos com
name $bin\$U.COM
option quiet
file $obj\cstart.obj
file $obj\$util.obj
"@ | Set-Content -Path "$obj\$util.lnk" -Encoding ascii
	& $WLINK "@$obj\$util.lnk"
	if ($LASTEXITCODE) { exit 1 }
}

# --- 4. test programs ---
& $NASM -f bin "$src\tests\hello.asm" -o "$bin\HELLO.COM"
if ($LASTEXITCODE) { exit 1 }
& $NASM -f obj "$src\tests\hellox.asm" -o "$obj\hellox.obj"
if ($LASTEXITCODE) { exit 1 }
& $WLINK format dos option quiet name "$bin\HELLOX.EXE" file "$obj\hellox.obj"
if ($LASTEXITCODE) { exit 1 }
& $NASM -f bin "$src\tests\tsr.asm" -o "$bin\TSR.COM"
if ($LASTEXITCODE) { exit 1 }
& $NASM -f bin "$src\tests\tests.asm" -o "$bin\TESTS.COM"
if ($LASTEXITCODE) { exit 1 }
& $NASM -f bin "$src\tests\speaker.asm" -o "$bin\SPEAKER.COM"
if ($LASTEXITCODE) { exit 1 }

# --- 5. DOS configuration files (consumed by image.ps1) ---
$configsys = @(
	"REM $OSNAME boot configuration"
	'FILES=20'
	'BUFFERS=20'
	'SHELL=A:\COMMAND.COM'
) -join "`r`n"
Set-Content -Path "$cfg\fdd-config.sys" -Value "$configsys`r`n" -Encoding ascii -NoNewline
$autoexec = @(
	'@ECHO OFF'
	"ECHO Welcome to $OSNAME!"
	'VER'
) -join "`r`n"
Set-Content -Path "$cfg\fdd-autoexec.bat" -Value "$autoexec`r`n" -Encoding ascii -NoNewline
$configsysC = @(
	"REM $OSNAME boot configuration"
	'FILES=20'
	'BUFFERS=20'
	'SHELL=C:\COMMAND.COM'
) -join "`r`n"
Set-Content -Path "$cfg\hdd-config.sys" -Value "$configsysC`r`n" -Encoding ascii -NoNewline
$autoexecC = @(
	'@ECHO OFF'
	'SET PATH=C:\;C:\DOS'
	'SET BLASTER=A220 I5 D1 H5 T6'
	"ECHO Welcome to $OSNAME! (booted from C:)"
	'VER'
) -join "`r`n"
Set-Content -Path "$cfg\hdd-autoexec.bat" -Value "$autoexecC`r`n" -Encoding ascii -NoNewline

Write-Host "Build OK -> build\bin" -ForegroundColor Green
