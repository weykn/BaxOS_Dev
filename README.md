![Tuxlet](TUXLET.png)

# Tuxlet OS

A small x86-64 operating system that boots through UEFI. It has its own
kernel and its own filesystem, and it implements enough of Linux's syscalls
that programs built for Linux run on it unmodified. Its disk is laid out the
way Linux's is, so those programs find everything where they already look.

```sh
make        # build build/TuxletOS.img
make run    # and boot it in QEMU
```

You need `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`, `mtools` and `gptfdisk`,
plus `edk2-ovmf` for `make run`.

## At a glance

|              |                                                                |
| ------------ | -------------------------------------------------------------- |
| **Boot**     | A UEFI loader that carries the kernel inside itself            |
| **Kernel**   | Ring 0: screen, filesystem, Linux's syscall table, modules     |
| **Programs** | ELF, static or dynamically linked, straight off a Linux system |
| **Shell**    | `tsh`, a small shell built into the kernel                     |
| **Layout**   | `/etc`, `/usr`, `/var` and the rest, with real symbolic links  |

## How it is put together

**The loader** (`src/boot`) is a PE executable that the firmware starts. The
kernel is embedded in it, so the boot partition holds a single file and the
loader needs no filesystem code to find the kernel. It collects the screen,
the memory map and the disk, then jumps to the kernel with them.

**The kernel** (`src/kernel`) runs in ring 0 and does only what nothing else
can: the screen and keyboard as one plain terminal, a flat filesystem of
contiguous files, and a syscall table that uses Linux's numbers and
arguments.

Once the boot script has set up the screen, its `modman takeover` line lets
the firmware go the way any operating system does - the kernel never does it
by itself - and everything the firmware was holding
becomes free memory, its page tables and ACPI's tables included: the kernel
moves onto tables of its own, 12 KB of them. That is about 36 MB on QEMU:
`mem` shows around 6 MB in use after boot, where it used to show 43, and
nearly all of that is what the firmware keeps for good. From then on the kernel drives the
IDE disk and the PS/2 keyboard itself, through the `storage/ide` and
`input/ps2` modules, and the screen stays in the mode it booted in. A machine with neither, such as one with a USB keyboard or an NVMe
disk, keeps the firmware and its drivers, as before.

**Everything else is a program off the disk.** Each program gets a region of
its own: half a terabyte of address space, hung off a spare slot of the
page tables and backed a page at a time as it is touched. The few
commands the kernel provides itself are files under `/proc`, so `mem` and
`/proc/mem` are the same command, because `/proc` is on `PATH`.

### The shell

`tsh` (`/proc/tsh`, in `src/kernel/shell.c`) is the shell the machine
starts. It reads a line, splits it into words on blanks, and runs the first
word - found through `PATH`, a kernel command in `/proc` or a program in
`/usr/bin` - with the rest as its arguments. `cd`, `export`, `exit` and
`help` are built in. `tsh` on its own starts another tsh, until `exit`;
`tsh <file>` runs a script, lines of the same one after another. It is part
of the kernel, so it costs no memory of a program's; another shell,
such as bash, is a program on the disk like any other.

Editing the line is the terminal's job, so every program that reads a line
gets it too: the arrows move through the line, Home, End and Delete do what
they say, up and down go back through the last few lines, and Tab completes
a command or a file name, with a second Tab listing the choices.

### fork, without a scheduler

There is one processor and no scheduler, so the two halves of a fork cannot
run side by side, and they don't need to. `fork` runs the child right away,
to the end, and only returns to the parent once the child has finished.

That is safe because, until it calls `execve`, the child runs in its
parent's memory. Every page is write-protected at the fork, and the first
write to a page copies its old contents aside. When the child finishes, its
changes are undone page by page, and the parent carries on as if it had only
been waiting. At `execve` the child gets a region of its own, and the
parent's is left exactly as the fork found it. A program linked to a fixed
address gets the low memory it was linked for in the same way, swapped in
while it runs and out while a child does.

A pipe is therefore a buffer rather than a channel: the first program fills
it and finishes, then the second reads it. A writer that never finishes
never hands its pipe over.

### Threads

A program's threads do run side by side. Each has its own kernel stack, and
whenever one waits in the kernel (a futex, `poll`, a socket, `sleep`, a key)
the next one runs. Nothing interrupts a thread that spins in its own code
without a syscall. `futex`, `clone3`, `eventfd` and `gettid` are there, so
glibc's threads work; curl resolves names on one.

### Starting a program quickly

On a machine like this, two things make a program slow to start, and neither
is the processor. A firmware disk read costs about the same whatever its
size, so what matters is how many times the disk is asked, not for how much.
And a program off a Linux system brings a loader and a two-megabyte C
library, most of which it never touches.

So a file mapping is a promise rather than a copy: `mmap`, and the program
loader itself, record where the memory is and what belongs there, and nothing
is read until the program touches it.

The rest is the disk cache, the `storage/cache` module, which works like
Linux's page cache: every disk read goes through it, in 64 KiB lines, and
what it read stays. With `cache auto` it grows into memory nobody else is
using, less a reserve, and the moment anything else wants that memory it
gives the lines used longest ago back - so `mem` shows it apart, as free
memory lent out. `cache 64M` caps it instead, and `cache off` empties it.
`cache add <file>` and `cache rm <file>` keep the list of files it reads in
whenever it starts, which `/etc/tuxlet/cache` fills with the loader and the
C library. A cached `curl --version` starts in ten milliseconds.

The disk itself is read by bus-master DMA where the IDE controller has it,
64 KiB a command, and by programmed I/O where it does not. Writes are on the
disk when the call returns; the flush that makes the drive keep them waits,
as on Linux, for the machine to go idle, for `sync` or `fsync`, or for the
power to go.

### Modules

Parts of the kernel that not every machine needs live on the disk as
modules, as `<category>/<name>.kmod` under the folders of `MODPATH` -
`/usr/lib/modules`, as `/etc/tuxlet/env` sets it - and cost nothing until
they are loaded. The format is Tuxlet's own: nothing like Linux's
`.ko`, and neither kind loads on the other's kernel. Every module on the
disk is there to be had, and `modman` turns them on and off:

```sh
modman                          # what there is, and what is enabled
modman enable network/stack     # load it now, and at every boot
modman disable network/stack    # unload it, and not at boot
modman auto network             # enable every one of them that works here
modman takeover                 # let the firmware go; the screen mode is fixed after
```

| Module              | What it is                                   | By default |
| ------------------- | -------------------------------------------- | ---------- |
| `storage/ide`       | the IDE disk driver (DMA, or PIO)            | on         |
| `storage/cache`     | the disk cache, and `cache`                  | on         |
| `input/ps2`         | the PS/2 keyboard driver                     | on         |
| `display/wallpaper` | the picture behind the text, and `set-bg`    | off        |
| `debug/trace`       | every syscall, to `/var/log`, and `log`      | off        |
| `network/stack`     | IPv4, TCP, UDP, ICMP, DHCP, and `net`        | off        |
| `network/e1000`, `network/rtl8139`, `network/virtio` | network cards | off  |

`/etc/tuxlet/modules` is a script like the rest of `/etc/tuxlet`, a
`modman enable` line a module, and the boot script runs it first. The disk
driver takes the disk over from the firmware the moment it loads, which
keeps the rest of the boot fast. `modman takeover` lets the firmware go only
once a disk module and a keyboard module have registered, and says why not
otherwise; the firmware's drivers are kept until then. A module the kernel still depends on,
such as the disk driver after that, refuses to unload.

A module is an ELF shared object, loaded anywhere and linked by name
against what the kernel exports (`src/kernel/module.c`). Modules that work
together meet in a slot the kernel keeps rather than calling each other: a
card driver registers its card, the network stack uses whatever card is
there, and either loads and runs without the other. They are built from
`src/modules`, a file or a folder each.

### The network

`modman auto network` brings it up: it enables `network/stack` and the
driver for whichever card the machine has - or enable them by name. DHCP finds an address the first time the
machine is idle at the prompt, or at first use if that comes sooner, which is
also when the card is started, and the name server goes in
`/etc/resolv.conf`. Programs reach it through Linux's socket calls, so
`ping`, name lookups, and TCP clients and servers work as they do on Linux.
`net` shows the card and its address.

The module also answers netlink (links, addresses, routes and neighbours,
and the socket list), the interface ioctls, raw sockets, ICMP errors on the
error queue (`IP_RECVERR`), and `/proc/net`'s files. The socket syscalls
themselves are the module's: with it off, `socket` answers ENOSYS, as it
does on a Linux built without networking. The disk has `ip`, `ss`,
`ifconfig`, `route`, `arp`, `netstat`, `ping`, `traceroute`, `tracepath`,
`curl`, `wget`, `nc`, `telnet`, `ftp`, `whois`, `getent` and `hostname`.

## On the disk

The layout is Linux's, so no paths are translated anywhere: a program
opening `/etc/passwd` or loading `/lib64/ld-linux-x86-64.so.2` gets exactly
that file.

```text
/
├── bin -> usr/bin      symbolic links, as on any current Linux
├── lib -> usr/lib
├── lib64 -> usr/lib    where an x86-64 program looks for its loader
├── boot/               the kernel, kept to be read like any other file
├── dev/                null, zero, tty and the rest, made up by the kernel
├── etc/                the machine's settings
├── home/               ordinary users' homes, one folder each
├── proc/               the kernel's own commands
├── root/               root's home, where the shell starts
├── run/                runtime state
├── sys/                the system and its devices
├── tmp/                scratch files
├── usr/
│   ├── bin/            programs, cc among them
│   ├── include/        glibc's and Linux's headers, for cc
│   ├── lib/            the libraries they were linked against, and gcc's
│   └── share/          terminfo, vim's runtime, wallpapers, other shared data
└── var/
    ├── cache/
    ├── lib/            state a program keeps between runs
    └── log/            a file of syscalls per program, with debug/trace
```

`/dev`, `/proc` and `/sys` are empty folders on the disk. What appears in
them comes from the kernel.

**Compiling.** `cc` is gcc, with `as`, `ld`, the headers and the start-up
files beside it, so `cc hello.c -o hello` builds and links a program here as
it would on Linux. gcc's own programs are linked to a fixed address, which
is what low memory is for (see fork, above); `cc1` alone wants the 47 MiB
from 4 MiB up, so a machine needs a little more RAM than that free down
there to compile.

**Boot.** Tuxlet OS's own settings live in `/etc/tuxlet/`, apart from the
ones every Linux system has, and every one of them is a tsh script.
`/etc/tuxlet/boot` describes the machine: the environment, the modules, the
screen, the text size, the wallpaper and the disk cache, each a script of
its own beside it. It runs in `/etc/tuxlet`, so it calls them as
`tsh cache` - `/etc/tuxlet/cache` holds the cache's mode (`auto`, a size, or
`off`) and which files it reads in from the start. `/etc/tuxlet/env` exports
`PATH`, `MODPATH`, `HOME` and the rest: where tsh looks for commands and
modman for modules, and what every program tsh starts is given. The kernel
has no paths of its own; libraries are found by the loader a program names,
as on Linux, `LD_LIBRARY_PATH` included. The kernel runs it itself before there
is a shell. `/etc/tuxlet/shell` is the script run next, and what it runs is
the shell: `tsh`, or `bash`. It is run again whenever that shell exits.

**Settings.** A program keeps its settings where Linux software expects, in
dotfiles in the home folder: a file such as `~/.vimrc`, or a folder such as
`~/.config/<program>/`. What applies to every user is in `/etc`.

**Symbolic links** work as they do on Linux. Relative targets resolve from
the folder the link is in, and absolute targets from the root. `open`,
`stat`, `execve` and `chdir` follow links; `lstat`, `readlink` and `unlink`
act on the link itself. More than 40 links in one path is `ELOOP`.

Everything under `src/disk` is copied onto the image as it is, symbolic
links included, so a file you put there is on the machine at the next
`make`. Files saved from inside Tuxlet OS survive a rebuild; `make clean`
wipes them.

## Working on it

The machine can be driven without a screen. Boot the image with
`-display none`, a monitor socket and `-debugcon file:boot.log`, then
`sendkey` at the monitor and read the log. Build with `make DEBUG=1` and
every character the console prints also goes to the debug port, so
`boot.log` is a transcript of the screen.
