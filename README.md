![Tuxlet OS](TUXLET-BANNER.png)

# Tuxlet OS

> A small x86 operating system with its own kernel, shell and filesystem that runs unmodified Linux programs, on 64-bit and 32-bit PCs.

---

## Contents

- [Overview](#overview)
- [Key Features](#key-features)
- [System Requirements](#system-requirements)
- [Building & Running](#building--running)
- [Shell (`tsh`)](#shell-tsh)
- [Kernel Commands](#kernel-commands)
- [Kernel Modules](#kernel-modules)
- [Package Management (`tuxpac`)](#package-management-tuxpac)
- [Firmware Takeover](#firmware-takeover)
- [Boot Configuration](#boot-configuration)
- [32-bit Programs on the 64-bit OS](#32-bit-programs-on-the-64-bit-os)
- [The 32-bit OS](#the-32-bit-os)
- [TEF: Tuxlet Executable Format](#tef-tuxlet-executable-format)
- [TCR: Tuxlet C Runtime](#tcr-tuxlet-c-runtime)

---

## Overview

Tuxlet OS has its own kernel, its own shell (`tsh`) and its own filesystem, and it answers Linux system calls, 64-bit and 32-bit. Debian's and Alpine's packages therefore install and run on it as they are, with no recompiling. It goes from a bare prompt in a few hundred KB up to an X11 desktop running Firefox.

The kernel keeps only what everything needs. Drivers, networking, Linux extras and IPC are modules that are loaded when the boot script asks for them, so the idle system stays around **512 KB of RAM** and **1 MB on disk**.

---

## Key Features

* **Linux ABI:** Unmodified Linux ELF programs run directly: glibc and musl, `fork`/`exec`, signals, pipes, threads, `mmap`, sockets, `epoll`, `/proc` and `/dev`.
* **32-bit programs:** i386 programs run beside 64-bit ones, in the processor's compatibility mode, with `compat/ia32` translating their calls. See [32-bit Programs on the 64-bit OS](#32-bit-programs-on-the-64-bit-os).
* **32-bit PCs:** `make ARCH=i386` builds the whole OS for a 32-bit processor, from the same sources: PAE paging, BIOS or 32-bit UEFI, Debian's `i386` and Alpine's `x86` packages. See [The 32-bit OS](#the-32-bit-os).
* **Modular kernel:** Storage, USB, input, networking, IPC, Linux compatibility and tracing are loadable modules, managed with `modman`.
* **Modern PC drivers:** AHCI (SATA), NVMe, xHCI (USB keyboards and sticks) and Realtek gigabit Ethernet, alongside IDE, PS/2, e1000, RTL8139 and VirtIO.
* **Graphical desktop:** `/dev/fb0` and evdev input let Debian's X.Org server, `openbox`, terminals and browsers run unmodified.
* **UEFI and BIOS:** The same image boots on either firmware, on real hardware or in QEMU; the 32-bit image on a BIOS or a 32-bit UEFI.
* **Package manager:** `tuxpac` fetches, resolves and installs packages from Debian and Alpine repositories, 64-bit or 32-bit, over HTTP or HTTPS, with numbered mirror files deciding which wins.
* **Own executable format:** `tsh` and the coreutils are [TEF](#tef-tuxlet-executable-format) programs built on [TCR](#tcr-tuxlet-c-runtime), Tuxlet's own C runtime, 64-bit or 32-bit. `tsh` takes a single page.
* **Scriptable boot:** `/etc/tuxlet/boot` is a plain `tsh` script that loads drivers and starts whatever the machine should run.
* **Firmware takeover:** Once its own drivers are up, the kernel can leave the firmware behind and reclaim its memory.

---

## System Requirements

| Resource | Minimum | Notes |
| --- | --- | --- |
| **CPU** | x86-64, or i686 with PAE | Any 64-bit PC; the `ARCH=i386` build runs on a 32-bit one (Pentium II, Athlon or newer) |
| **Firmware** | UEFI or BIOS | The same image boots on both |
| **Memory** | **512 KB** | The OS's own use; the firmware needs its share on top |
| **Disk** | **1 MB** | The core image, without user files or packages |

---

## Building & Running

### Prerequisites

| | 64-bit (default) | 32-bit (`ARCH=i386`) |
| --- | --- | --- |
| **Compiler** | `gcc`, `nasm`, `x86_64-w64-mingw32-gcc` | `gcc` with `-m32` support (multilib), `nasm`, `i686-w64-mingw32-gcc` |
| **Disk tools** | `mtools`, `gptfdisk` | `mtools`, `gptfdisk` |
| **Emulator** *(optional)* | `qemu-system-x86_64`, with `edk2-ovmf` for UEFI | `qemu-system-i386`, with a 32-bit OVMF in `/usr/share/edk2/ia32` for UEFI |

SeaBIOS, for `BOOT=bios`, comes with QEMU.

### Build

```bash
make                # 64-bit:  build/TuxletOS.img
make ARCH=i386      # 32-bit:  build/i386/TuxletOS-i386.img
make clean          # start over
```

Everything in `src/disk/` goes onto the root filesystem as it is, folders and symbolic links included. The filesystem is updated in place, so files saved from inside Tuxlet survive a rebuild until `make clean`.

### Run in QEMU

```bash
make run                        # UEFI, 39 MB of RAM
make run BOOT=bios              # the same image under SeaBIOS
make run MEM=1G NET=e1000       # more memory, and a network card
make run ARCH=i386 BOOT=bios    # the 32-bit OS, on a Pentium III
```

### First Boot

A fresh image boots to a bare `tsh` prompt, with no `PATH` yet. For a full install - Debian's base system, an X desktop and a user `tux` - run:

```sh
/usr/bin/tsh /etc/tuxlet/setup
```

Or set the environment by hand:

```sh
export PATH=/ctl:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin
export MODPATH=/usr/lib/modules
```

### Write to a Disk

```bash
sudo dd if=build/TuxletOS.img of=/dev/sdX bs=4M conv=fsync
```

For a 32-bit PC, write `build/i386/TuxletOS-i386.img` instead.

> ⚠️ **Warning:** this overwrites everything on `/dev/sdX`. Check the drive with `lsblk` first.

### Make Variables

| Variable | Default | Values | Effect |
| --- | --- | --- | --- |
| `ARCH` | `x86_64` | `x86_64`, `i386` | The processor the OS is built for. `i386` builds everything 32-bit into `build/i386`. |
| `FREE` | `1M` | a size, e.g. `16M`, `2G` | Free space on the disk for files saved from inside Tuxlet. |
| `DEBUG` | off | `1` | Kernel debug output, and a copy of the screen, to QEMU's debug port. |
| `TEF_BITS` | `64` (`32` with `ARCH=i386`) | `64`, `32` | Builds `tsh` and the coreutils as 64-bit or 32-bit TEF programs. |
| `BOOT` | `uefi` | `uefi`, `bios` | Firmware for `make run`: OVMF or SeaBIOS. The same image boots either way. |
| `MEM` | `39M` | a QEMU size, e.g. `64M`, `1G` | RAM for `make run`. |
| `NET` | `none` | `none`, `e1000`, `rtl8139`, `virtio` | Network card for `make run`. |

---

## Shell (`tsh`)

Tuxlet boots into `tsh` (`/usr/bin/tsh`), a minimal shell that the kernel starts as its login shell. It runs `/etc/tuxlet/boot`, then `/etc/tuxlet/shell` if it exists, and otherwise gives you a prompt.

* **Built-in Commands:** `cd`, `export`, `exit`, `help`.
* **tuxlet-coreutils:** `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir [-p] [-m mode]` and `put` are programs in `/usr/bin`, replaced by the GNU ones when a package installs them.
* **Writing Files:** Use `put <file> [text]` to write a line of text.
* **Running Scripts:** `tsh <script>` runs a script's lines one by one, printing each as it runs.
* **Syntax:** A line is split into words on blanks, and `#` starts a comment. There are no pipes, redirections, variables or quoting; install `bash` for those.
* **TEF:** `tsh` and the coreutils are TEF files built on TCR, Tuxlet's own executable format and C runtime. `tsh` is under 3 KB and takes one page of memory; each coreutil is under 1 KB. See [TEF](#tef-tuxlet-executable-format) and [TCR](#tcr-tuxlet-c-runtime).

### Process Controls

| Key | Effect |
| --- | --- |
| `Ctrl + C` | Sends SIGINT to the foreground program. |
| `Ctrl + C` (3x repeatedly) | Kills a program that has stopped reading its keys. |
| `Ctrl + \` | Sends SIGQUIT. |

### Environment

A fresh image has no `PATH`, so only full paths work until you set one. The boot script normally does this:

```sh
export PATH=/ctl:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin
export MODPATH=/usr/lib/modules
```

---

## Kernel Commands

The kernel's own commands live in `/ctl`. They are not files on the disk: running one runs it inside the kernel, and its output can still be piped like any program's.

| Command | Usage | Description |
| --- | --- | --- |
| `mem` | `mem usage\|all\|top\|peak <program>` | `usage` shows the OS's own memory use. `all` adds the firmware's share; `top` lists every process, biggest first; `peak` runs a program and, when it ends, prints the most memory it (with its children) and the whole system had in use. |
| `mode` / `scale` / `font` | `mode [size]`, `scale [size\|off]`, `font [size]` | Sets the screen resolution, console scaling and font size. Alone, each lists its choices. `mode` only works before `modman takeover`. |
| `clear` / `echo` | `echo [text]` | Clears the screen, or prints text with `{bold}`, `{n}` *(newline)* and `{uptime}` markup. |
| `greet` | `greet [text]` | Sets the welcome line, with the same markup as `echo`; alone, prints it. |
| `modman` | `modman [enable\|disable <module>\|auto <category>\|takeover]` | Loads and unloads modules. Alone, it lists them. See [Kernel Modules](#kernel-modules). |
| `reboot` / `poweroff` | | Writes out pending disk data, then restarts or powers off. |

Modules can add commands of their own: `cache` (`storage/cache`) and `net` (`network/stack`). Make sure `/ctl` is on your `PATH` (see [Shell](#environment)).

---

## Kernel Modules

Drivers and optional kernel features are modules (`.kmod`), loaded with `modman`:

```sh
modman enable <module>   # Load a module
modman disable <module>  # Unload a module
modman auto <category>   # Load whichever drivers in a category find their hardware
modman                   # List modules and what is loaded
```

`modman` looks for modules in the folder named by `MODPATH`, normally `/usr/lib/modules`. Without `MODPATH`, nothing can load, including `compat/linux`, which most Linux programs need.

### Available Driver Matrix

| Category | Module | Provided Feature / Service |
| --- | --- | --- |
| **`storage`** | `storage/ide` | Low-level IDE disk controller support |
|  | `storage/ahci` | SATA disks on an AHCI controller |
|  | `storage/nvme` | NVMe SSDs |
|  | `storage/usb` | USB sticks and disks (bulk-only mass storage) |
|  | `storage/cache` | In-memory block cache (exposes `cache` command) |
| **`usb`** | `usb/xhci` | USB 3 (xHCI) host controller, used by the USB class drivers |
| **`input`** | `input/ps2` | PS/2 keyboard and mouse support |
|  | `input/usbkbd` | USB keyboard support (needs `usb/xhci`) |
| **`network`** | `network/stack` | TCP/IP network protocol stack (exposes `net` command) |
|  | `network/e1000` | Intel e1000 Gigabit NIC driver |
|  | `network/rtl8139` | Realtek RTL8139 Fast Ethernet driver |
|  | `network/rtl8169` | Realtek RTL8168/8111 Gigabit and RTL8125 2.5G Ethernet driver |
|  | `network/virtio` | Para-virtualized VirtIO network driver |
| **`ipc`** | `ipc/unix` | `AF_UNIX` stream sockets and device-event netlink |
|  | `ipc/pty` | Pseudo-terminals (`/dev/ptmx`, `/dev/pts/N`) for terminal emulators |
| **`sched`** | `sched/timer` | Timer interrupt for programs (after `modman takeover`): Ctrl-C stops busy loops, programs share the CPU |
| **`compat`** | `compat/linux` | Extra Linux system calls; loaded automatically on first use |
|  | `compat/ia32` | 32-bit (i386) programs' system calls; loaded automatically with the first one - on the 32-bit OS, every program's, and never dropped |
| **`debug`** | `debug/trace` | Low-level kernel tracing tool |

---

## Package Management (`tuxpac`)

Tuxlet includes a custom package manager located at `/usr/bin/tuxpac` that directly parses and installs Debian packages (`.deb`) and Alpine packages (`.apk`), 64-bit or 32-bit.

### 1. Configure Mirrors

Each file `/etc/tuxlet/<prio>.<backend>.mirror` is a set of repositories, one per line, for one backend: `debian` or `alpine`. A ready-made one ships as `/etc/tuxlet/100.debian.mirror-example`.

```sh
# /etc/tuxlet/100.debian.mirror
http://deb.debian.org/debian trixie main

# /etc/tuxlet/200.alpine.mirror
http://dl-cdn.alpinelinux.org/alpine v3.22 main community
```

* **Priority:** A package more than one file offers comes from the lowest number; within one file, the newest version wins. Dependencies are resolved across all of them.
* **Architecture:** `[arch=...]` in front of the URL takes another architecture's packages: `[arch=i386]` for Debian or `[arch=x86]` for Alpine gives 32-bit ones. Without it, `amd64` and `x86_64` - or, on the 32-bit OS, `i386` and `x86`.
* **Alpine packages:** Their install and remove scripts run as Debian's maintainer scripts do.

After changing the files, run `tuxpac -y`.

**HTTPS Support:** To use `https://` mirrors, first install `curl` via HTTP (it brings `ca-certificates`), then update your mirror file.

### 2. Package Management Commands

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

---

## Firmware Takeover

`modman takeover` makes Tuxlet OS stop using the firmware (UEFI boot services or BIOS interrupts) and hand the firmware's memory to programs. After takeover, timer-driven scheduling (`sched/timer`) becomes available.

Before invoking takeover mode, ensure:

1. A **storage module** (`storage/ide`, `storage/ahci`, `storage/nvme` or `storage/usb`) is active.
2. An **input module** (`input/ps2` or `input/usbkbd`) is loaded.
3. Your display **`mode`** is configured (video resolution cannot be altered post-takeover).

```sh
modman enable storage/ide
modman enable input/ps2
modman takeover
```

---

## Boot Configuration

Startup is two `tsh` scripts:

1. **`/etc/tuxlet/boot`**: Runs first, from its own folder. Use it to set the environment, load driver modules, set the display mode and start services. `/etc/tuxlet/boot-example` is a complete one that ends by starting X.
2. **`/etc/tuxlet/shell`**: Runs after `boot`, from `$HOME`, and runs again every time it ends. Without it, you get an interactive `tsh` prompt.

---

## 32-bit Programs on the 64-bit OS

Tuxlet runs 32-bit (i386) Linux programs next to 64-bit ones: Debian's `i386` packages, Alpine's `x86` ones, static or dynamic, glibc or musl, and 32-bit TEF programs.

- **Compatibility mode:** the kernel stays 64-bit. A 32-bit program runs in the processor's compatibility mode, on a 32-bit code segment of its own, and enters the kernel with `int 0x80`, as on i386 Linux.
- **Memory:** everything a 32-bit program has (its image, its loader, the heap, `mmap` and the stack) sits in the first gigabyte, the low memory the kernel already gives programs linked to a fixed address. Its pages are bought as they are touched, like a 64-bit program's, each at its own physical address where that is free. The kernel keeps its own memory out of the way: above the gigabyte where the machine has RAM there, otherwise below 128 MiB, where no 32-bit program goes.
- **System calls:** `compat/ia32` turns each call into the 64-bit one. Most only change number; the ones whose structures are laid out differently (file status, times, I/O vectors, messages, signal actions, resource limits) are converted both ways, and 64-bit values split over two registers are put back together. The kernel loads the module with the first 32-bit program and drops it when the last one ends.
- **Threads:** a thread's TLS lives in a GDT entry that follows whichever thread runs, so both glibc and musl threads work.
- **Signals:** handlers get i386's signal frames, with or without `SA_SIGINFO`, and return through `sigreturn` or `rt_sigreturn`.
- **Mixing:** a 64-bit program can start a 32-bit one and the other way round; fork, exec, pipes and job control behave as they do for 64-bit programs.

The core programs can be built 32-bit too: `make TEF_BITS=32`.

Unlike a 64-bit program's, a fork copies the memory a 32-bit program has rather than sharing it until written, and its libraries' pages are its own rather than shared with other processes.

A UEFI machine whose RAM ends between 768 MiB and 1 GiB keeps its firmware where a 32-bit program's stack goes, so 32-bit programs run there only after `modman takeover`.

---

## The 32-bit OS

`make ARCH=i386` builds the same OS for a 32-bit PC, from the same sources, into `build/i386/TuxletOS-i386.img`: the kernel, every module, the loaders, `tsh`, the coreutils and `tuxpac`, all 32-bit.

- **Processor:** i686 with PAE and FXSR: a Pentium II, an Athlon or anything newer, 64-bit processors included. Each loader checks and says so on a machine without them (a Pentium M that hides its PAE is refused). The 64-bit image likewise says "needs a 64-bit processor" on a 32-bit one rather than restarting.
- **Firmware:** a BIOS, through the same boot code, now entering 32-bit protected mode instead of long mode; or a 32-bit UEFI, through `BOOTIA32.EFI` - the firmware some 64-bit tablets have too, which runs the 32-bit OS.
- **Paging:** PAE, the same page entries as x86-64 in three levels, mapping the four gigabytes one to one. RAM above 4 GiB is not used.
- **Programs:** every one is a 32-bit program, with the layout 32-bit programs have on the 64-bit OS: all of it in the first gigabyte of its own address space. `int 0x80` is the only way in, and `compat/ia32` the kernel's answer to every call; it is loaded with the first program, `tsh`, and stays. 64-bit programs are refused.
- **Thread pointers:** with no `FS_BASE` or `GS_BASE` registers, a thread's TLS is its GDT entry alone, loaded into GS again on every return to the program.
- **Interrupts:** a 32-bit firmware's interrupt handlers only work in ring 0, so while it is in charge programs run with interrupts off and its timers run during calls into it. After `modman takeover` and `sched/timer`, programs are preempted as on the 64-bit OS.
- **Packages:** `tuxpac` takes Debian's `i386` and Alpine's `x86` packages without being told.
- **Memory:** the kernel keeps its own memory clear of the programs' gigabyte, as on the 64-bit OS. A 32-bit UEFI firmware's memory may be anywhere in it, so for the length of every call into the firmware it sees its own memory again, whatever program is running; a page of the firmware's the kernel itself reads is mapped for the kernel alone.
- **Size:** the kernel is about 142 KB here, against 119 KB 64-bit: 64-bit numbers cost more on a 32-bit processor.

As for 32-bit programs on the 64-bit OS, a fork copies a program's memory rather than sharing it until written, and a library's pages are each process's own.

---

## TEF: Tuxlet Executable Format

TEF is the format of the preinstalled programs, `tsh` and tuxlet-coreutils; everything else, such as Debian's and Alpine's packages, is ordinary ELF. It is small and simple:

- 32-bit or 64-bit: the header says which, and the kernel runs the program in the matching mode.
- An 8-byte header: a marker, the class, and how much memory the program needs; the program starts right after it.
- Static only: no dynamic linking, no relocations, no interpreter.
- No sections, segments, symbol tables or anything else that is not needed to run it, which keeps the files small. `tsh` is under 3 KB and each coreutil is under 1 KB.
- A tiny loader. Files from before the class byte (TEF64) still load.

---

## TCR: Tuxlet C Runtime

TCR is a small C runtime that the core programs (`tsh` and tuxlet-coreutils) use in place of glibc or musl; everything else runs on its own libc. Each program links it statically, and the linker drops any part the program does not use.

- **Start-up:** takes the arguments and environment from the stack the kernel builds for any Linux program, runs `main` and exits with its result.
- **System calls:** made directly, with thin wrappers for the few the core tools need. A call answers as the kernel does, with a negated errno on failure, so there is no `errno`.
- **Memory:** a first-fit allocator over the program break; the heap never shrinks.
- **Strings, output and paths:** the handful of helpers the tools share, with no `printf` and no `FILE *`.
- **Both widths:** the same source builds 64-bit, or 32-bit with `-m32` (i386's call numbers, `int 0x80`, and 32-bit structures), and the result is a TEF of either class.

It is deliberately minimal: no locale, no threads and no stdio.
