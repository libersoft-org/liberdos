# ============================================================
# image.ps1 - builds bootable disk images from build artifacts
#
# Windows counterpart of image.sh - same argument syntax.
# Run .\build.ps1 first; this script only assembles the images
# from build\bin, build\boot and build\cfg.
#
# The FAT filesystem builder is self-contained: the floppy
# (FAT12) and the hard disk (FAT16) share the FAT chain,
# cluster and directory writers below and differ only in the
# geometry and the boot blocks.
# ============================================================
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$bin  = Join-Path $root 'build\bin'
$boot = Join-Path $root 'build\boot'
$cfg  = Join-Path $root 'build\cfg'

function Show-Usage {
	Write-Host @'
Usage: .\image.ps1 [--fdd floppy.img] [--hdd disk.img] [--size N[K|M|G]] [--add directory]...

  --fdd floppy.img    create a bootable 1.44 MB FAT12 floppy image
  --hdd disk.img      create a bootable FAT16 hard disk image
  --size N[K|M|G]     hard disk image size (with --hdd only);
                      a plain number is bytes, the K/M/G suffix
                      selects 1024-based units (default 32M)
  --add directory     copy the directory's contents onto the hard
                      disk image root (with --hdd only; may be
                      repeated)
  --help              show this help

At least one of --fdd / --hdd is required; with both, both
images are created.
'@
	exit 1
}

# args parsed manually so the syntax matches image.sh
$Fdd = ''
$Hdd = ''
$Size = ''
$AddDirs = @()
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
		'--size' {
			if ($i + 1 -ge $args.Count) { Show-Usage }
			$i++
			$Size = $args[$i]
		}
		'--add' {
			if ($i + 1 -ge $args.Count) { Show-Usage }
			$i++
			$AddDirs += $args[$i]
		}
		default { Show-Usage }
	}
	$i++
}

if (-not $Fdd -and -not $Hdd) { Show-Usage }
if ($Size -and -not $Hdd) {
	Write-Host '--size is only valid together with --hdd' -ForegroundColor Red
	exit 1
}
if ($AddDirs.Count -and -not $Hdd) {
	Write-Host '--add is only valid together with --hdd' -ForegroundColor Red
	exit 1
}
foreach ($d in $AddDirs) {
	if (-not (Test-Path $d -PathType Container)) {
		Write-Host "--add: not a directory: $d" -ForegroundColor Red
		exit 1
	}
}

# --size: plain number = bytes, K/M/G suffix = 1024-based units
$SizeBytes = 32MB
if ($Size) {
	if ($Size -notmatch '^(\d+)([KkMmGg])?$') {
		Write-Host "Bad size '$Size' - expected a number with an optional K/M/G suffix." -ForegroundColor Red
		exit 1
	}
	$SizeBytes = [long]$Matches[1]
	switch (($Matches[2] + '').ToUpper()) {
		'K' { $SizeBytes *= 1KB }
		'M' { $SizeBytes *= 1MB }
		'G' { $SizeBytes *= 1GB }
	}
}

if (-not (Test-Path "$bin\KERNEL.SYS")) {
	Write-Host "Missing $bin\KERNEL.SYS - run .\build.ps1 first." -ForegroundColor Red
	exit 1
}

# OS name from version.h (single source); padded to BPB widths
$verH   = Get-Content "$root\src\version.h" -Raw
$OSNAME = [regex]::Match($verH, 'OS_NAME\s+"([^"]+)"').Groups[1].Value

# ============================================================
# shared FAT image builder
#
# Initialize-Image allocates the in-memory image and records
# the geometry; the helpers below write FAT chains (FAT12 or
# FAT16), data clusters and directory entries. Content comes
# from DEST=SRC specs: DEST is the path on the image
# (subdirectories are created as needed), SRC a host file or
# directory (directories are copied recursively). Clusters are
# allocated contiguously from cluster 2.
# ============================================================

function Initialize-Image([long]$totalSecs, [int]$fatBits, [int]$fatLba,
	[int]$secsPerClus, [int]$numFats, [int]$secsPerFat, [int]$rootEntries) {
	$script:img         = New-Object byte[] ($totalSecs * 512)
	$script:FatBits     = $fatBits
	$script:Eoc         = if ($fatBits -eq 12) { 0xFFF } else { 0xFFFF }
	$script:FatLba      = $fatLba
	$script:SecsPerClus = $secsPerClus
	$script:NumFats     = $numFats
	$script:SecsPerFat  = $secsPerFat
	$script:RootEntries = $rootEntries
	$script:RootLba     = $fatLba + $numFats * $secsPerFat
	$script:DataLba     = $script:RootLba + [int]($rootEntries * 32 / 512)
	$script:ClusBytes   = $secsPerClus * 512
	$script:MaxClus     = [long][math]::Floor(($totalSecs - $script:DataLba) / $secsPerClus) + 2
	$script:nextClus    = 2
	$script:used        = 0
	$script:count       = 0
}

# write one FAT entry into all FAT copies (FAT12 packed pairs
# or FAT16 words)
function Set-FatEntry([int]$cl, [int]$val) {
	for ($f = 0; $f -lt $NumFats; $f++) {
		$base = ($FatLba + $f * $SecsPerFat) * 512
		if ($FatBits -eq 12) {
			$idx = $base + [int][math]::Floor($cl * 3 / 2)
			if ($cl % 2 -eq 0) {
				$img[$idx]     = $val -band 0xFF
				$img[$idx + 1] = ($img[$idx + 1] -band 0xF0) -bor (($val -shr 8) -band 0x0F)
			} else {
				$img[$idx]     = ($img[$idx] -band 0x0F) -bor (($val -shl 4) -band 0xF0)
				$img[$idx + 1] = ($val -shr 4) -band 0xFF
			}
		} else {
			$off = $base + $cl * 2
			$img[$off]     = $val -band 0xFF
			$img[$off + 1] = ($val -shr 8) -band 0xFF
		}
	}
}

# allocate a linked chain of $n clusters, return the first one
function New-Chain([int]$n) {
	if ($n -le 0) { return 0 }
	if ($script:nextClus + $n -gt $MaxClus) { throw "image full" }
	$first = $script:nextClus
	for ($i = 0; $i -lt $n; $i++) {
		$cl = $first + $i
		if ($i -eq $n - 1) { Set-FatEntry $cl $Eoc }
		else { Set-FatEntry $cl ($cl + 1) }
	}
	$script:nextClus += $n
	$script:used += $n
	return $first
}

function Write-Clusters([byte[]]$data, [int]$first) {
	if ($data.Length -gt 0) {
		$dst = ($DataLba + ($first - 2) * $SecsPerClus) * 512
		[Array]::Copy($data, 0, $img, $dst, $data.Length)
	}
}

function Get-Name11([string]$name) {
	$base = [IO.Path]::GetFileNameWithoutExtension($name).ToUpper()
	$ext  = [IO.Path]::GetExtension($name).TrimStart('.').ToUpper()
	if ($base.Length -gt 8 -or $ext.Length -gt 3) {
		throw "not an 8.3 name: $name"
	}
	return $base.PadRight(8) + $ext.PadRight(3)
}

# write one 32-byte directory entry into $buf at byte offset $off
function Write-DirEnt([byte[]]$buf, [int]$off, [string]$n11, [int]$attr,
	[datetime]$lw, [int]$first, [long]$size) {
	[Text.Encoding]::ASCII.GetBytes($n11).CopyTo($buf, $off)
	$buf[$off + 11] = $attr
	$t = ($lw.Hour -shl 11) -bor ($lw.Minute -shl 5) -bor [int]($lw.Second / 2)
	$d = (($lw.Year - 1980) -shl 9) -bor ($lw.Month -shl 5) -bor $lw.Day
	[BitConverter]::GetBytes([uint16]$t).CopyTo($buf, $off + 22)
	[BitConverter]::GetBytes([uint16]$d).CopyTo($buf, $off + 24)
	[BitConverter]::GetBytes([uint16]$first).CopyTo($buf, $off + 26)
	[BitConverter]::GetBytes([uint32]$size).CopyTo($buf, $off + 28)
}

# inject a host file's data, return its first cluster
function Add-File([System.IO.FileInfo]$item) {
	$data = [IO.File]::ReadAllBytes($item.FullName)
	$first = New-Chain ([int][math]::Ceiling($data.Length / $ClusBytes))
	Write-Clusters $data $first
	$script:count++
	return $first
}

# copy a host directory into the image, return its first cluster
function Add-Tree([string]$localDir, [int]$parentClus) {
	$items = @(Get-ChildItem $localDir)
	$nclus = [Math]::Max(1, [int][math]::Ceiling((($items.Count + 2) * 32) / $ClusBytes))
	$myClus = New-Chain $nclus
	$buf = New-Object byte[] ($nclus * $ClusBytes)
	$lw = (Get-Item $localDir).LastWriteTime
	Write-DirEnt $buf 0  ('.'.PadRight(11))  0x10 $lw $myClus 0
	Write-DirEnt $buf 32 ('..'.PadRight(11)) 0x10 $lw $parentClus 0
	$slot = 2
	foreach ($item in $items) {
		$n11 = Get-Name11 $item.Name
		if ($item.PSIsContainer) {
			$sub = Add-Tree $item.FullName $myClus
			Write-DirEnt $buf ($slot * 32) $n11 0x10 $item.LastWriteTime $sub 0
		} else {
			$first = Add-File $item
			Write-DirEnt $buf ($slot * 32) $n11 0x20 $item.LastWriteTime $first $item.Length
		}
		$slot++
	}
	Write-Clusters $buf $myClus
	return $myClus
}

# parse DEST=SRC specs into a virtual tree; node = ordered map:
# name -> @{Type='file'|'hostdir'|'dir'; ...}
function Build-Tree([string[]]$specs) {
	$tree = [ordered]@{}
	foreach ($spec in $specs) {
		$eq = $spec.IndexOf('=')
		if ($eq -lt 1) { throw "bad file spec, expected DEST=SRC: $spec" }
		$dest = $spec.Substring(0, $eq).Trim('/', '\')
		$src  = $spec.Substring($eq + 1)
		$parts = $dest -split '[\\/]+'
		$node = $tree
		for ($i = 0; $i -lt $parts.Count - 1; $i++) {
			$d = $parts[$i].ToUpper()
			if (-not $node.Contains($d)) {
				$node[$d] = @{ Type = 'dir'; Children = [ordered]@{} }
			} elseif ($node[$d].Type -ne 'dir') {
				throw "conflicting spec: $d is both a file and a directory"
			}
			$node = $node[$d].Children
		}
		$leaf = $parts[-1].ToUpper()
		if ($node.Contains($leaf)) { throw "duplicate destination: $dest" }
		$item = Get-Item $src
		if ($item.PSIsContainer) {
			$node[$leaf] = @{ Type = 'hostdir'; Src = $item.FullName }
		} else {
			$node[$leaf] = @{ Type = 'file'; Src = $item.FullName }
		}
	}
	return $tree
}

# emit a virtual directory node, return its first cluster
function Add-VTree($children, [int]$parentClus) {
	$nclus = [Math]::Max(1, [int][math]::Ceiling((($children.Count + 2) * 32) / $ClusBytes))
	$myClus = New-Chain $nclus
	$buf = New-Object byte[] ($nclus * $ClusBytes)
	$lw = Get-Date
	Write-DirEnt $buf 0  ('.'.PadRight(11))  0x10 $lw $myClus 0
	Write-DirEnt $buf 32 ('..'.PadRight(11)) 0x10 $lw $parentClus 0
	$slot = 2
	foreach ($name in $children.Keys) {
		$slot32 = $slot * 32
		$n11 = Get-Name11 $name
		$entry = $children[$name]
		switch ($entry.Type) {
			'dir' {
				$sub = Add-VTree $entry.Children $myClus
				Write-DirEnt $buf $slot32 $n11 0x10 $lw $sub 0
			}
			'hostdir' {
				$sub = Add-Tree $entry.Src $myClus
				Write-DirEnt $buf $slot32 $n11 0x10 (Get-Item $entry.Src).LastWriteTime $sub 0
			}
			'file' {
				$item = Get-Item $entry.Src
				$first = Add-File $item
				Write-DirEnt $buf $slot32 $n11 0x20 $item.LastWriteTime $first $item.Length
			}
		}
		$slot++
	}
	Write-Clusters $buf $myClus
	return $myClus
}

# emit the whole virtual tree; the top level lands in the root
# directory area, everything else in data clusters
function Write-Root($tree) {
	if ($tree.Keys.Count -gt $RootEntries) { throw "root directory full" }
	$rootIdx = 0
	foreach ($name in $tree.Keys) {
		$de = $RootLba * 512 + $rootIdx * 32
		$n11 = Get-Name11 $name
		$entry = $tree[$name]
		switch ($entry.Type) {
			'dir' {
				$sub = Add-VTree $entry.Children 0
				Write-DirEnt $img $de $n11 0x10 (Get-Date) $sub 0
			}
			'hostdir' {
				$sub = Add-Tree $entry.Src 0
				Write-DirEnt $img $de $n11 0x10 (Get-Item $entry.Src).LastWriteTime $sub 0
			}
			'file' {
				$item = Get-Item $entry.Src
				$first = Add-File $item
				Write-DirEnt $img $de $n11 0x20 $item.LastWriteTime $first $item.Length
			}
		}
		$rootIdx++
	}
}

function Save-Image([string]$path) {
	if (-not [IO.Path]::IsPathRooted($path)) {
		$path = Join-Path (Get-Location).Path $path
	}
	$dir = Split-Path -Parent $path
	if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
	[IO.File]::WriteAllBytes($path, $img)
	Write-Host "$(Split-Path -Leaf $path): $($script:count) file(s), $($script:used) cluster(s) used"
}

function Read-Sector([string]$path) {
	$bytes = [IO.File]::ReadAllBytes($path)
	if ($bytes.Length -ne 512) { throw "boot sector must be 512 bytes: $path" }
	return $bytes
}

# ============================================================
# image types
# ============================================================

# 1.44 MB FAT12 floppy: geometry matching the BPB carried by
# the boot sector (src\boot\boot.asm): 512 B/sector, 1 reserved
# sector, 2 FATs x 9 sectors, 224 root entries, 2880 sectors.
function New-FloppyImage([string]$path, [string[]]$specs) {
	Initialize-Image -totalSecs 2880 -fatBits 12 -fatLba 1 `
		-secsPerClus 1 -numFats 2 -secsPerFat 9 -rootEntries 224

	$vbr = Read-Sector "$boot\boot.bin"
	[Array]::Copy($vbr, 0, $img, 0, 512)

	Set-FatEntry 0 0xFF0  # FAT[0] = media descriptor (0xF0)
	Set-FatEntry 1 0xFFF  # FAT[1] = end-of-chain marker

	Write-Root (Build-Tree $specs)
	Save-Image $path
}

# FAT16 hard disk: MBR with one active type 06h partition at
# LBA 63; the sectors-per-cluster value follows the DOS FAT16
# table and the FAT size is derived from the partition size.
# The boot code (src\boot\boothdd.asm) carries placeholder BPB
# values - the real ones are patched in below.
function New-HddImage([string]$path, [string[]]$specs, [long]$sizeBytes) {
	if ($sizeBytes -ge 2GB) { throw "disk size must be under 2 GB (FAT16)" }
	$totalSecs = [long][math]::Ceiling($sizeBytes / 512)
	$partStart = 63
	$partSecs  = $totalSecs - $partStart
	$spc = if ($sizeBytes -le 128MB) { 4 }
	elseif ($sizeBytes -le 256MB) { 8 }
	elseif ($sizeBytes -le 512MB) { 16 }
	elseif ($sizeBytes -le 1GB) { 32 }
	else { 64 }
	$spf = [int][math]::Ceiling(($partSecs - 1 - 32) / (256 * $spc + 2))
	$clusters = [long][math]::Floor(($partSecs - 1 - 32 - 2 * $spf) / $spc)
	if ($clusters -lt 4085) { throw "disk too small for FAT16 - use at least 9M" }

	Initialize-Image -totalSecs $totalSecs -fatBits 16 -fatLba ($partStart + 1) `
		-secsPerClus $spc -numFats 2 -secsPerFat $spf -rootEntries 512

	# MBR: bootstrap code bytes 0..445, partition entry, signature
	$mbrBin = Read-Sector "$boot\mbr.bin"
	[Array]::Copy($mbrBin, 0, $img, 0, 446)
	$p = 0x1BE
	$img[$p + 0] = 0x80                                       # active
	$img[$p + 1] = 0x01; $img[$p + 2] = 0x01                  # CHS start (dummy)
	$img[$p + 4] = 0x06                                       # FAT16B
	$img[$p + 5] = 0xFE; $img[$p + 6] = 0xFF; $img[$p + 7] = 0xFF  # CHS end
	[BitConverter]::GetBytes([uint32]$partStart).CopyTo($img, $p + 8)
	[BitConverter]::GetBytes([uint32]$partSecs).CopyTo($img, $p + 12)
	$img[0x1FE] = 0x55; $img[0x1FF] = 0xAA

	# partition boot sector: real boot code + the real BPB values
	$bs = $partStart * 512
	$vbr = Read-Sector "$boot\boothdd.bin"
	[Array]::Copy($vbr, 0, $img, $bs, 512)
	$oemId  = $OSNAME.PadRight(8).Substring(0, 8)
	$volLab = "$($OSNAME.ToUpper())-HD".PadRight(11).Substring(0, 11)
	[Text.Encoding]::ASCII.GetBytes($oemId).CopyTo($img, $bs + 3)
	[BitConverter]::GetBytes([uint16]512).CopyTo($img, $bs + 11)      # bytes/sector
	$img[$bs + 13] = $spc
	[BitConverter]::GetBytes([uint16]1).CopyTo($img, $bs + 14)        # reserved
	$img[$bs + 16] = 2                                                # FATs
	[BitConverter]::GetBytes([uint16]512).CopyTo($img, $bs + 17)      # root entries
	if ($partSecs -le 0xFFFF) {
		[BitConverter]::GetBytes([uint16]$partSecs).CopyTo($img, $bs + 19)
	} else {
		[BitConverter]::GetBytes([uint16]0).CopyTo($img, $bs + 19)
		[BitConverter]::GetBytes([uint32]$partSecs).CopyTo($img, $bs + 32)
	}
	$img[$bs + 21] = 0xF8                                             # media
	[BitConverter]::GetBytes([uint16]$spf).CopyTo($img, $bs + 22)
	[BitConverter]::GetBytes([uint16]63).CopyTo($img, $bs + 24)       # SPT
	[BitConverter]::GetBytes([uint16]16).CopyTo($img, $bs + 26)       # heads
	[BitConverter]::GetBytes([uint32]$partStart).CopyTo($img, $bs + 28)
	$img[$bs + 36] = 0x80; $img[$bs + 38] = 0x29
	[Text.Encoding]::ASCII.GetBytes($volLab).CopyTo($img, $bs + 43)
	[Text.Encoding]::ASCII.GetBytes("FAT16   ").CopyTo($img, $bs + 54)
	$img[$bs + 510] = 0x55; $img[$bs + 511] = 0xAA

	Set-FatEntry 0 0xFFF8  # FAT[0] = media descriptor (0xF8)
	Set-FatEntry 1 0xFFFF  # FAT[1] = end-of-chain marker

	Write-Root (Build-Tree $specs)
	Save-Image $path
}

# ============================================================
# image contents
# ============================================================

# --- floppy image (drive A:) ---
if ($Fdd) {
	New-FloppyImage $Fdd @(
		"KERNEL.SYS=$bin\KERNEL.SYS"
		"COMMAND.COM=$bin\COMMAND.COM"
		"MORE.COM=$bin\MORE.COM"
		"ATTRIB.COM=$bin\ATTRIB.COM"
		"CHKDSK.COM=$bin\CHKDSK.COM"
		"CONFIG.SYS=$cfg\fdd-config.sys"
		"AUTOEXEC.BAT=$cfg\fdd-autoexec.bat"
	)
	Write-Host "Image OK -> $Fdd" -ForegroundColor Green
}

# --- hard disk image (drive C:) ---
# System files in the root, utilities in DOS\ (on PATH via
# AUTOEXEC.BAT), test programs in TEST\, extra payload
# directories from --add (copied recursively).
if ($Hdd) {
	$hddFiles = @(
		"KERNEL.SYS=$bin\KERNEL.SYS"
		"COMMAND.COM=$bin\COMMAND.COM"
		"CONFIG.SYS=$cfg\hdd-config.sys"
		"AUTOEXEC.BAT=$cfg\hdd-autoexec.bat"
		"DOS/MORE.COM=$bin\MORE.COM"
		"DOS/ATTRIB.COM=$bin\ATTRIB.COM"
		"DOS/CHKDSK.COM=$bin\CHKDSK.COM"
		"TEST/TESTS.COM=$bin\TESTS.COM"
		"TEST/SPEAKER.COM=$bin\SPEAKER.COM"
		"TEST/HELLO.COM=$bin\HELLO.COM"
		"TEST/TSR.COM=$bin\TSR.COM"
		"TEST/HELLOX.EXE=$bin\HELLOX.EXE"
	)
	# The contents of each --add directory land in the root of C:
	# (game payloads and other extra content).
	foreach ($d in $AddDirs) {
		foreach ($item in Get-ChildItem $d) {
			$hddFiles += "$($item.Name)=$($item.FullName)"
		}
	}
	New-HddImage $Hdd $hddFiles $SizeBytes
	Write-Host "Image OK -> $Hdd" -ForegroundColor Green
}

exit 0
