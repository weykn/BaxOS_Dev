# Building & Running

## Prerequisites

Install these on the build machine:

* **Compiler & Toolchain:** `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`
* **Disk Utilities:** `mtools`, `gptfdisk`
* **Emulator (Optional):** `qemu-system-x86_64` with `edk2-ovmf` (UEFI firmware image) or SeaBIOS (bundled with QEMU, for `BOOT=bios`)

## Build Commands

```bash
# Build the core image (outputs to build/TuxletOS.img)
make

# Boot the image inside QEMU
make run

# Boot QEMU with extended memory and networking enabled
make run MEM=64M NET=e1000

# Boot QEMU with BIOS (SeaBIOS) instead of UEFI
make run BOOT=bios

# Allocate 16 MB of writeable free space on the target disk image
make FREE=16M

# Compile with debugging enabled (outputs to QEMU debug port)
make DEBUG=1

# Clean build artifacts
make clean
```

> **Tip:** Files in `src/disk/` are copied onto the root filesystem as they are, folders and symbolic links included. The filesystem is updated in place, so files saved from inside Tuxlet survive a rebuild until `make clean`.

## Makefile Variables

| Variable | Default | Allowed Values | Description |
| --- | --- | --- | --- |
| `MEM` | `39M` | Any valid QEMU memory size (e.g., `64M`, `1G`) | Sets RAM for the QEMU instance. |
| `NET` | `none` | `none`, `e1000`, `rtl8139`, `virtio` | Network interface driver. |
| `BOOT` | `uefi` | `uefi`, `bios` | Firmware for `make run`: OVMF (UEFI) or SeaBIOS (BIOS). The same image boots either way. |
| `FREE` | `1M` | Size notation (e.g., `16M`, `100M`) | Extra unallocated disk space appended to the final image. |
| `DEBUG` | *Off* | `1` | Sends kernel debug output, and a copy of everything on screen, to the QEMU debug port. |

## Flashing to Real Hardware

To write the compiled disk image directly to a USB stick or target disk:

```bash
sudo dd if=build/TuxletOS.img of=/dev/sdX bs=4M conv=fsync
```

> ⚠️ **WARNING:** This command will permanently overwrite all existing data on `/dev/sdX`. Verify your target drive path using `lsblk` before proceeding.
