![Tuxlet](TUXLET.png)

# Tuxlet OS

## Features

- **UEFI boot** on x86-64, in QEMU or on real hardware
- **Runs Linux programs unmodified**, with a Linux-style filesystem layout
- **Own kernel, filesystem and shell** (`tsh`)
- **Loadable modules** for storage, input, networking and debugging
- **Networking** through Linux's socket calls
- **Scriptable boot**: startup is plain `tsh` scripts in `/etc/tuxlet`
- **Small**: the OS itself needs about 104 KiB of RAM and 283 KiB of disk

## Quick start

```sh
make        # builds build/TuxletOS.img
make run    # boots it in QEMU
```

You need `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`, `mtools` and `gptfdisk` to build, and `qemu` with `edk2-ovmf` to run.

## Requirements

| Resource | Minimum | Notes |
| -------- | ------- | ----- |
| CPU      | x86-64  | |
| Firmware | UEFI    | |
| Memory   | 104 KiB | Plus whatever the firmware takes. |
| Disk     | 283 KiB | Plus free space for your files. |

## Building

```sh
make                        # build/TuxletOS.img
make run                    # boot it in QEMU
make run MEM=64M NET=e1000  # more memory, plus a network card
make FREE=16M               # 16 MiB of free space on the disk for saved files
make DEBUG=1                # also print to QEMU's debug port
make clean                  # remove build/
```

| Variable | Default | Values |
| -------- | ------- | ------ |
| `MEM`    | `39M`   | Any size QEMU accepts. |
| `NET`    | `none`  | `none`, `e1000`, `rtl8139`, `virtio`. `virtio` needs 42 MiB. |
| `FREE`   | `1M`    | Free space added to the disk. The disk is the size of its contents plus `FREE`. |
| `DEBUG`  | off     | `DEBUG=1` also prints to QEMU's debug port. |

Everything in `src/disk` is copied onto the image on each `make`, so that is where to put your own files and programs.

### Running on real hardware

Write the image to a disk and boot it with UEFI:

```sh
sudo dd if=build/TuxletOS.img of=/dev/sdX bs=4M conv=fsync
```

> **Warning:** this overwrites the target disk completely. Double-check `/dev/sdX` before running it.

## Using Tuxlet

The shell is `tsh`. Its built-ins are `cd`, `export`, `exit` and `help`. The kernel's own commands live in `/proc`, so put it on your path first:

```sh
export PATH=/proc
export MODPATH=/usr/lib/modules
```

### Kernel commands

| Command                      | What it does                                  |
| ---------------------------- | --------------------------------------------- |
| `mem [all]`                  | Show memory in use                            |
| `uptime`                     | Time since power-on                           |
| `mode`, `scale`, `font`      | Set screen mode, scaling and text size        |
| `clear`, `echo`              | Clear the screen, print text                  |
| `tsh [script]`               | Start another shell, or run a script          |
| `modman`                     | List modules                                  |
| `modman enable <module>`     | Load a module                                 |
| `modman disable <module>`    | Unload a module                               |
| `modman auto <category>`     | Load every module in a category that works on this machine |
| `modman takeover`            | Shut down the firmware and take over the machine (see below) |
| `reboot`, `poweroff`         | Restart or power off                          |

### Modules

| Category  | Module          | Provides                |
| --------- | --------------- | ----------------------- |
| `storage` | `storage/ide`   | IDE disk access         |
| `storage` | `storage/cache` | Disk cache; command `cache` |
| `input`   | `input/ps2`     | PS/2 keyboard           |
| `network` | `network/stack` | Network stack; command `net` |
| `network` | `network/e1000` | Intel e1000 driver      |
| `network` | `network/rtl8139` | Realtek RTL8139 driver |
| `network` | `network/virtio`  | virtio network driver |
| `debug`   | `debug/trace`   | Debug tracing           |

### Taking over from the firmware

`modman takeover` shuts down the UEFI firmware and frees the memory it was using. Before you run it:

- a **disk module** and a **keyboard module** must already be loaded, and
- the **screen mode must be set**, because it cannot be changed afterwards.

## Configuration

At power-on, Tuxlet runs `/etc/tuxlet/boot`, then `/etc/tuxlet/shell` to start the shell. Both are `tsh` scripts.

**`/etc/tuxlet/boot`** (example):

```sh
export PATH=/proc:/usr/bin
export MODPATH=/usr/lib/modules
export HOME=/root
modman enable storage/ide
modman enable input/ps2
modman auto network
mode 1920x1080
modman takeover
```

**`/etc/tuxlet/shell`** (example):

```sh
tsh
```