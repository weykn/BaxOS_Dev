![Tuxlet OS](TUXLET-BANNER.png)

# Tuxlet OS

## Features

- **UEFI boot** on x86-64, in QEMU or on real hardware
- **Runs Linux programs unmodified**, with a Linux-style filesystem layout
- **Own kernel, filesystem and shell** (`tsh`)
- **Loadable modules** for storage, input, networking and debugging
- **Networking** through Linux's socket calls
- **Scriptable boot**: startup is plain `tsh` scripts in `/etc/tuxlet`
- **Small**: the OS itself needs about 128 KiB of RAM and 512 KiB of disk

## Requirements

| Resource | Minimum | Notes |
| -------- | ------- | ----- |
| CPU      | x86-64  | |
| Firmware | UEFI    | |
| Memory   | 128 KiB | Plus whatever the firmware takes. |
| Disk     | 512 KiB | Plus free space for your files. |

## Building

```sh
make                        # build/TuxletOS.img
make run                    # boot it in QEMU
make run MEM=64M NET=e1000  # more memory, plus a network card
make FREE=16M               # 16 MiB of free space on the disk for saved files
make DEBUG=1                # also print to QEMU's debug port
make clean                  # remove build/
```
You need `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`, `mtools` and `gptfdisk` to build, and `qemu` with `edk2-ovmf` to run.

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

The shell is `tsh`, built into the kernel. Its built-ins are `tsh`, `cd`, `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `put`, `export`, `exit` and `help`; `put <file> [text]` writes one line to a file, and `tsh <script>` runs a script. The kernel's own commands live in `/ctl`, so put it on your path first:

```sh
export PATH=/ctl:/usr/bin
export MODPATH=/usr/lib/modules
```

`/proc` holds only the Linux files programs read, such as `/proc/net`.

Ctrl-C stops a program and `Ctrl-\` quits it, as on Linux; a program that catches Ctrl-C gets it as a signal. Three Ctrl-C in a row, with the program not reading for a second, end it whatever it does with the signal.

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

### Packages

`tuxpac` (`/usr/bin/tuxpac`, built from `src/tuxpac`) installs Debian packages from the mirrors in `/etc/tuxlet/mirror`, one per line, written the way `sources.list` writes them:

```sh
put /etc/tuxlet/mirror http://deb.debian.org/debian bookworm main
tuxpac -y
tuxpac -s bash
```

| Command              | What it does                                  |
| -------------------- | --------------------------------------------- |
| `tuxpac -y`          | Sync the package list                         |
| `tuxpac -s <package>`| Install a package and its dependencies        |
| `tuxpac -r <package>`| Remove a package and dependencies nothing else needs |
| `tuxpac -n <package>`| The same, deleting configuration files too    |
| `tuxpac -u [package]`| Upgrade every package, or one                 |
| `tuxpac -f <search>` | Find packages in the list                     |
| `tuxpac -q [search]` | List installed packages                       |
| `tuxpac -i <package>`| Show a package, online and installed          |

It needs the network modules loaded. An `https://` mirror works once `curl` and `ca-certificates` are installed, over an `http://` one:

```sh
tuxpac -s curl ca-certificates
put /etc/tuxlet/mirror https://deb.debian.org/debian bookworm main
```

Version constraints in dependencies are ignored and maintainer scripts are not run: the links their `update-alternatives` calls make (`vim`, `editor`, `awk`, `pager`) are made by tuxpac itself, and so is the certificate bundle `ca-certificates` builds. Libraries a program links against but its package does not list are found in the binaries and installed too, as are the few packages every Debian system has that a package needs without saying so (`ncurses-base` for terminal programs, `coreutils` for `fish` and `bash`). Documentation, man pages and translations are not unpacked. The package list takes about 15 MiB of disk for Debian's main archive, so build with `FREE` big enough for it and your packages.

### Taking over from the firmware

`modman takeover` shuts down the UEFI firmware and frees the memory it was using. Before you run it:

- a **disk module** and a **keyboard module** must already be loaded, and
- the **screen mode must be set**, because it cannot be changed afterwards.

## Configuration

At power-on, Tuxlet runs `/etc/tuxlet/boot`, a `tsh` script, then `/etc/tuxlet/shell` if there is one to start the shell - otherwise `tsh`.