# ============================================================
# run.ps1 - launches LiberDOS in QEMU (interactive window)
#
# Windows counterpart of run.sh - same argument syntax.
# Run without arguments to print usage.
#
# Keyboard and mouse work in the window (PS/2 emulation).
# Click to grab the mouse, Ctrl+Alt+G releases it.
# ============================================================
$ErrorActionPreference = 'Stop'

$QEMU     = 'C:\Program Files\qemu\qemu-system-i386.exe'
$buildDir = Join-Path $PSScriptRoot 'build'

function Show-Usage {
	Write-Host @'
Usage: .\run.ps1 [--fdd floppy.img] [--hdd disk.img] [--log]

  --fdd floppy.img    attach a floppy image
  --hdd disk.img      attach a hard disk image
  --log               collect crash diagnostics (QEMU debug log,
                      stderr, guest serial output - in build\log)

At least one of --fdd / --hdd is required. The machine boots from
the floppy when one is attached, otherwise from the hard disk.
'@
	exit 1
}

# args parsed manually so the syntax matches run.sh
$Fdd = ''
$Hdd = ''
$Log = $false
$i = 0
while ($i -lt $args.Count) {
	switch ($args[$i]) {
		'--fdd' {
			if ($i + 1 -ge $args.Count) { Show-Usage }
			$i++
			$Fdd = $args[$i]
		}
		'--hdd' {
			if ($i + 1 -ge $args.Count) { Show-Usage }
			$i++
			$Hdd = $args[$i]
		}
		'--log' { $Log = $true }
		default { Show-Usage }
	}
	$i++
}

if (-not $Fdd -and -not $Hdd) { Show-Usage }

$bootDev = 'c'
$drives = @()
if ($Fdd) {
	if (-not (Test-Path $Fdd)) {
		Write-Host "Missing $Fdd - run .\build.ps1 first." -ForegroundColor Red
		exit 1
	}
	$drives += '-drive', "format=raw,if=floppy,file=$Fdd"
	$bootDev = 'a'
}
if ($Hdd) {
	if (-not (Test-Path $Hdd)) {
		Write-Host "Missing $Hdd - run .\build.ps1 first." -ForegroundColor Red
		exit 1
	}
	$drives += '-drive', "format=raw,file=$Hdd"
}

# Extra diagnostics for chasing QEMU host crashes: guest_errors
# log, guest serial output (kernel mirrors the console to COM1)
# and QEMU's own stderr captured to files under build\log\.
$logDir = "$buildDir\log"
$extra = @()
if ($Log) {
	New-Item -ItemType Directory -Path "$logDir\dumps" -Force | Out-Null
	$extra += '-d', 'guest_errors', '-D', "$logDir\qemu.log"
	$extra += '-serial', "file:$logDir\serial.log"
	$extra += '-monitor', 'tcp:127.0.0.1:45454,server,nowait'
}

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
$qemuArgs = @(
	'-accel', 'tcg,thread=single'
	'-vga', 'cirrus,retrace=precise'
	'-display', 'sdl'
	'-audiodev', 'sdl,id=snd0,in.voices=0'
	'-machine', 'pcspk-audiodev=snd0'
	'-device', 'sb16,audiodev=snd0'
	'-device', 'adlib,audiodev=snd0'
	'-boot', $bootDev
) + $drives + $extra

if ($Log) {
	# Attach procdump so an access violation inside QEMU produces a
	# full memory dump even though QEMU's own handler swallows it
	# before Windows Error Reporting gets a chance to run.
	# procdump.exe is a Sysinternals binary, not part of the repo -
	# download it to build\log\ from https://learn.microsoft.com/sysinternals/downloads/procdump
	$procdump = "$logDir\procdump.exe"
	if (-not (Test-Path $procdump)) {
		Write-Host "Missing $procdump - download ProcDump (Sysinternals) there first." -ForegroundColor Red
		exit 1
	}
	$proc = Start-Process -FilePath $QEMU -ArgumentList $qemuArgs `
		-RedirectStandardError "$logDir\qemu-stderr.log" -PassThru
	& $procdump -accepteula -e 1 -f C0000005 -ma `
		$proc.Id "$logDir\dumps\qemu-crash.dmp"
	$proc.WaitForExit()
	Write-Host "QEMU exited with code $($proc.ExitCode)" -ForegroundColor Yellow
} else {
	& $QEMU @qemuArgs
}
