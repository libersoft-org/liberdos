# LiberDOS - build and installation instructions

## Table of contents

- [**Prerequisites**](#prerequisites)
- [**Build**](#build)
- [**Create disk images**](#create-disk-images)
- [**Run**](#run)

## Prerequisites

LiberDOS is built with free, cross-platform tools.

The build script compiles the OS into the `build` directory. The image script then assembles bootable disk images from those artifacts.

### Windows

**Install the tools:**

- **NASM**: download the installer from https://www.nasm.us/ (default location `C:\Program Files\NASM`).
- **Open Watcom 2.0**: download the installer from https://github.com/open-watcom/open-watcom-v2/releases and install it. Set `$WATCOM` in `build.ps1` to the install directory (the build uses the `binnt64` host binaries).
- **PowerShell 7**: `winget install Microsoft.PowerShell`
- **QEMU**: download the installer from https://qemu.weilnetz.de/w64/ (default location `C:\Program Files\qemu`). Note: the `winget` package carries development snapshots - the installer from the link above is a stable release.

### Linux

**Install the tools:**

```sh
sudo apt install nasm qemu-system-x86 mtools snapd
snap install snapd
snap install open-watcom --beta
export WATCOM=/snap/open-watcom/current
```

## Build

The build script compiles the whole OS - boot sectors, kernel, shell and utilities - into the `build` directory.

**The build produces:**

- `build/bin` - the OS binaries (`KERNEL.SYS`, `COMMAND.COM`, utilities, test programs)
- `build/boot` - the boot sectors (floppy, MBR, hard disk)
- `build/cfg` - generated `CONFIG.SYS` / `AUTOEXEC.BAT` variants for the images
- `build/obj` - intermediate compile artifacts

The build itself does not create any disk images - see the next section.

### Windows

```powershell
.\build.ps1
```

### Linux

```sh
./build.sh
```

## Create disk images

The image script assembles bootable disk images from the build artifacts. Run it without arguments to print usage. At least one of `--fdd` / `--hdd` is required; with both, both images are created.

### Windows

```powershell
.\image.ps1 --fdd .\image\floppy.img --hdd .\image\disk.img --size 512M --add .\hdd\
```

### Linux

```sh
./image.sh --fdd ./image/floppy.img --hdd ./image/disk.img --size 512M --add ./hdd/
```

This creates a bootable 1.44 MB FAT12 floppy image at `image/floppy.img` and a bootable 512 MB FAT16 hard disk image at `image/disk.img`, with the contents of the `hdd` directory copied onto the hard disk root.

`--size` accepts a plain number (bytes) or a number with a `K`/`M`/`G` suffix (1024-based units, e.g. `100M` = 104857600 bytes) and applies to the hard disk image only (default 32M). FAT16 limits the size to roughly 9 MB - 2 GB.

`--add` copies the contents of a host directory onto the hard disk image root (recursively); repeat the option for multiple directories.

## Run

The run script launches LiberDOS in an interactive QEMU window. Run it without arguments to print usage. At least one of `--fdd` / `--hdd` is required; the machine boots from the floppy when one is attached, otherwise from the hard disk.

### Windows

```powershell
.\run.ps1 --fdd floppy.img     # attach a floppy image
.\run.ps1 --hdd disk.img       # attach a hard disk image
.\run.ps1 --log                # collect crash diagnostics (QEMU log, serial output)
```

### Linux

```sh
./run.sh --fdd floppy.img      # attach a floppy image
./run.sh --hdd disk.img        # attach a hard disk image
./run.sh --log                 # collect crash diagnostics (QEMU log, serial output)
```
