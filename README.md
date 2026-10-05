
![Tuxlet OS](TUXLET-BANNER.png)

# Tuxlet OS

> A lightweight, custom x86-64 operating system featuring its own kernel, shell, and filesystem while providing ABI compatibility to run unmodified Linux binaries.

---

## Overview

**Tuxlet OS** bridges the gap between educational bare-metal kernels and production operating systems. It implements an independent kernel, custom shell (`tsh`), and native filesystem layout, yet supports standard Linux system calls to run unmodified Debian packages natively.

Designed to be hyper-lightweight, Tuxlet OS requires only **256 KiB of RAM** and **512 KiB of storage** for its core components.

---

## Key Features

* **Linux ABI Compatibility:** Runs standard, uncompiled Linux programs natively via Linux-style socket calls and `/proc` hooks.
* **Custom Modular Architecture:** Dedicated kernel, shell (`tsh`), and dynamic loadable modules (storage, input, networking, and debugging).
* **UEFI and BIOS:** One image boots on either firmware, on x86-64 hardware and in virtualized environments (QEMU).
* **Embedded Package Manager:** Includes `tuxpac`, a dedicated tool for fetching, resolving, and installing Debian main repository packages.
* **Scriptable Startup:** Simple, human-readable shell scripts (`/etc/tuxlet/boot`) control the initial boot sequence.
* **Bare-Metal Takeover:** Ability to leave the firmware (UEFI or BIOS) behind to reclaim memory once essential drivers are initialized.

---

## System Requirements

| Resource | Minimum Requirement | Notes |
| --- | --- | --- |
| **CPU** | x86-64 | Standard 64-bit architecture |
| **Firmware** | UEFI or BIOS | The same image boots on both |
| **Memory** | **256 KiB** | System allocation (excluding firmware overhead) |
| **Disk** | **512 KiB** | Core system image size (excluding user files) |

---

## Building & Running

### Prerequisites

Ensure you have the required build tools and target tools installed on your host system:

* **Compiler & Toolchain:** `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`
* **Disk Utilities:** `mtools`, `gptfdisk`
* **Emulator (Optional):** `qemu-system-x86_64` with `edk2-ovmf` (UEFI firmware image) or SeaBIOS (bundled with QEMU, for `BOOT=bios`)

### Build Commands

```bash
# Build the core image (outputs to build/TuxletOS.img)
make

# Boot the image inside QEMU
make run

# Boot QEMU with extended memory and networking enabled
make run MEM=64M NET=e1000

# Boot QEMU with BIOS (SeaBIOS) instead of UEFI
make run BOOT=bios

# Allocate 16 MiB of writeable free space on the target disk image
make FREE=16M

# Compile with debugging enabled (outputs to QEMU debug port)
make DEBUG=1

# Clean build artifacts
make clean
```

> **Tip:** Any files placed inside the `src/disk/` directory are automatically copied onto the root filesystem image during build.

### Makefile Variables

| Variable | Default | Allowed Values | Description |
| --- | --- | --- | --- |
| `MEM` | `39M` | Any valid QEMU memory size (e.g., `64M`, `1G`) | Sets RAM for the QEMU instance. |
| `NET` | `none` | `none`, `e1000`, `rtl8139`, `virtio` | Network interface driver. |
| `BOOT` | `uefi` | `uefi`, `bios` | Firmware for `make run`: OVMF (UEFI) or SeaBIOS (BIOS). The same image boots either way. |
| `FREE` | `1M` | Size notation (e.g., `16M`, `100M`) | Extra unallocated disk space appended to the final image. |
| `DEBUG` | *Off* | `1` | Enables verbose debug output to the QEMU debug port. |

### Flashing to Real Hardware

To write the compiled disk image directly to a USB stick or target disk:

```bash
sudo dd if=build/TuxletOS.img of=/dev/sdX bs=4M conv=fsync
```

> ⚠️ **WARNING:** This command will permanently overwrite all existing data on `/dev/sdX`. Verify your target drive path using `lsblk` before proceeding.

---

## Usage Guide

### Shell (`tsh`) Basics

Tuxlet boots into `tsh`, a lightweight built-in shell.

* **Built-in Commands:** `cd`, `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `put`, `export`, `exit`, `help`.
* **Writing Files:** Use `put <file> [text]` to write a line of text.
* **Running Scripts:** Execute `tsh <script_path>`.
* **Process Controls:**
* `Ctrl + C`: Sends a interrupt signal to the foreground application.
* `Ctrl + C` (3x repeatedly): Forcefully terminates a non-responsive process.
* `Ctrl + \`: Immediately quits the process.

To access kernel commands and binaries, ensure your shell environment variables are exported:

```sh
export PATH=/ctl:/usr/bin
export MODPATH=/usr/lib/modules
```

---

### Kernel Commands

System control executables are located in `/ctl`:

| Command | Usage | Description |
| --- | --- | --- |
| `mem` | `mem [all]` | Displays current system memory allocation and usage. |
| `mode` / `scale` / `font` | Standard options | Configures frame-buffer resolution, UI scaling, and console font. |
| `clear` / `echo` | Text output | Clears terminal screen or prints text; Supports `{bold}`, `{n}` *(newline)* and `{uptime}`. |
| `modman` | Management | Loads, unloads, and inspects module states (see details below). |
| `reboot` / `poweroff` | Power control | Restarts or safely shuts down the machine. |

---

### Kernel Modules

Modules are loaded on demand via `modman`:

```sh
modman enable <module>   # Load a module
modman disable <module>  # Unload a module
modman auto <category>   # Auto-detect and load compatible hardware drivers
```

#### Available Driver Matrix

| Category | Module | Provided Feature / Service |
| --- | --- | --- |
| **`storage`** | `storage/ide` | Low-level IDE disk controller support |
|  | `storage/cache` | In-memory block cache (exposes `cache` command) |
| **`input`** | `input/ps2` | PS/2 keyboard interface support |
| **`network`** | `network/stack` | TCP/IP network protocol stack (exposes `net` command) |
|  | `network/e1000` | Intel e1000 Gigabit NIC driver |
|  | `network/rtl8139` | Realtek RTL8139 Fast Ethernet driver |
|  | `network/virtio` | Para-virtualized VirtIO network driver |
| **`debug`** | `debug/trace` | Low-level kernel tracing tool |

---

### Package Management (`tuxpac`)

Tuxlet includes a custom package manager located at `/usr/bin/tuxpac` that directly parses and installs standard Debian binary archives (`.deb`).

#### 1. Configure Mirrors

Repositories are defined in `/etc/tuxlet/mirror` (one entry per line):

```sh
put /etc/tuxlet/mirror http://deb.debian.org/debian trixie main
```

**HTTPS Support:** To use `https://` mirrors, first install `curl` via HTTP (it brings `ca-certificates`), then update your mirror file:

```sh
tuxpac -y
tuxpac -s curl
put /etc/tuxlet/mirror https://deb.debian.org/debian trixie main
```

#### 2. Package Management Commands

| Command | Action |
| --- | --- |
| `tuxpac -y` | Sync local package indexes with configured mirrors |
| `tuxpac -s <pkg>` | Install a package along with its required dependencies |
| `tuxpac -e` | Install Debian essential packages |
| `tuxpac -re` | Remove Debian essential packages |
| `tuxpac -r <pkg>` | Remove a package and unneeded orphaned dependencies |
| `tuxpac -n <pkg>` | Purge a package along with its configuration files |
| `tuxpac -u [pkg]` | Upgrade a specific package or all installed software |
| `tuxpac -f <query>` | Search available repository packages |
| `tuxpac -q [query]` | List installed packages on the system |
| `tuxpac -i <pkg>` | Show detailed package metadata |

> **Note on Compatibility:** `tuxpac` automatically resolves shared library dependencies (`.so`) and builds runtime symlinks (e.g. `vim`, `awk`, `editor`). Dependency version constraints and maintainer post-install scripts are ignored. Non-essential content like `man` pages and documentation are omitted to save storage space.

---

## Advanced Operations

### Firmware Takeover

Running `modman takeover` instructs Tuxlet OS to stop using the firmware - UEFI Runtime Services or BIOS interrupts - freeing up motherboard firmware memory for user space execution.

⚠️ **PRE-TAKEOVER CHECKLIST:**
Before invoking takeover mode, ensure:
1. A **storage module** (`storage/ide`) is active.
2. An **input module** (`input/ps2`) is loaded.
3. Your display **`mode`** is configured (video resolution cannot be altered post-takeover).

```sh
modman enable storage/ide
modman enable input/ps2
modman takeover
```

### Boot Configuration

The startup lifecycle is fully scriptable using `tsh`:

1. **`/etc/tuxlet/boot`**: Executes first upon system initialization. Use this script to load necessary driver modules, set display resolutions, and mount filesystems.
2. **`/etc/tuxlet/shell`**: Executes immediately after `boot`. If this script is omitted, Tuxlet defaults to spawning an interactive `tsh` prompt.