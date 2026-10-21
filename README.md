![Tuxlet OS](TUXLET-BANNER.png)

# Tuxlet OS

> A small x86-64 operating system with its own kernel, shell and filesystem that runs unmodified Linux programs.

---

## Overview

Tuxlet OS has its own kernel, its own shell (`tsh`) and its own filesystem, and it answers Linux system calls. Debian's packages therefore install and run on it as they are, with no recompiling. It goes from a bare prompt in a few hundred KB up to an X11 desktop running Firefox.

The kernel keeps only what everything needs. Drivers, networking, Linux extras and IPC are modules that are loaded when the boot script asks for them, so the idle system stays around **512 KB of RAM** and **1 MB on disk**.

---

## Key Features

* **Linux ABI:** Unmodified Linux ELF programs run directly: glibc, `fork`/`exec`, signals, pipes, threads, `mmap`, sockets, `epoll`, `/proc` and `/dev`.
* **Modular kernel:** Storage, USB, input, networking, IPC, Linux compatibility and tracing are loadable modules, managed with `modman`.
* **Modern PC drivers:** AHCI (SATA), NVMe, xHCI (USB keyboards and sticks) and Realtek gigabit Ethernet, alongside IDE, PS/2, e1000, RTL8139 and VirtIO.
* **Graphical desktop:** `/dev/fb0` and evdev input let Debian's X.Org server, `openbox`, terminals and browsers run unmodified.
* **UEFI and BIOS:** The same image boots on either firmware, on real hardware or in QEMU.
* **Package manager:** `tuxpac` fetches, resolves and installs packages from Debian's main repository, over HTTP or HTTPS.
* **Own executable format:** `tsh` and the coreutils are [TEF64](doc/tef64.md) programs built on [TCR](doc/tcr.md), Tuxlet's own C runtime. `tsh` takes a single page.
* **Scriptable boot:** `/etc/tuxlet/boot` is a plain `tsh` script that loads drivers and starts whatever the machine should run.
* **Firmware takeover:** Once its own drivers are up, the kernel can leave the firmware behind and reclaim its memory.

---

## System Requirements

| Resource | Minimum | Notes |
| --- | --- | --- |
| **CPU** | x86-64 | Any 64-bit PC |
| **Firmware** | UEFI or BIOS | The same image boots on both |
| **Memory** | **512 KB** | The OS's own use; the firmware needs its share on top |
| **Disk** | **1 MB** | The core image, without user files or packages |

---

## Quick Start

```bash
make        # build/TuxletOS.img
make run    # boot it in QEMU
```

A fresh image boots to a bare `tsh` prompt. Set up the environment first, or run `/usr/bin/tsh /etc/tuxlet/setup` for a full install:

```sh
export PATH=/ctl:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin
export MODPATH=/usr/lib/modules
```

Files placed in `src/disk/` are copied onto the root filesystem as they are when the image is built.

---

## Documentation

| Document | Contents |
| --- | --- |
| [Building & Running](doc/building.md) | Prerequisites, make targets and variables, flashing a USB stick |
| [Shell (`tsh`)](doc/shell.md) | Built-ins, tuxlet-coreutils, scripts, keys, environment |
| [Kernel Commands](doc/kernel-commands.md) | The `/ctl` commands: `mem`, `modman`, `mode`, ... |
| [Kernel Modules](doc/modules.md) | `modman` and every driver module |
| [Package Management (`tuxpac`)](doc/tuxpac.md) | Mirrors, HTTPS and the package commands |
| [Advanced Operations](doc/advanced.md) | Firmware takeover and the boot scripts |
| [TEF64](doc/tef64.md) | Tuxlet executable format |
| [TCR](doc/tcr.md) | Tuxlet C runtime |
