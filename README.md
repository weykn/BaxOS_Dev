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
| **Kernel**   | Ring 0: screen, keyboard, filesystem, Linux's syscall table    |
| **Programs** | ELF, static or dynamically linked, straight off a Linux system |
| **Shell**    | `tsh`, a small shell of its own                                |
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

Once the boot script has set up the screen, the kernel lets the firmware go
the way any operating system does, and everything the firmware was holding
becomes free memory. That is about 36 MB on QEMU: `mem` shows around 6 MB in
use after boot, where it used to show 43. From then on the kernel drives the
IDE disk and the PS/2 keyboard itself, and the screen stays in the mode it
booted in. A machine with neither, such as one with a USB keyboard or an NVMe
disk, keeps the firmware and its drivers, as before.

**Everything else is a program off the disk.** Each program gets a region of
its own: half a terabyte of address space, hung off a spare slot of the
firmware's page tables and backed a page at a time as it is touched. The few
commands the kernel provides itself are files under `/proc`, so `mem` and
`/proc/mem` are the same command, because `/proc` is on `PATH`.

### The shell

`tsh` (`src/tsh`) is the shell the machine starts. It reads a line, splits it
into words on blanks, and runs the first word as a program, looked up on
`PATH`, with the rest as its arguments. `cd` and `exit` are built in. It
talks to the kernel through plain syscalls and needs no C library, so it is
a single static file of about five kilobytes.

Editing the line is the terminal's job, so every program that reads a line
gets it too: the arrows move through the line, Home, End and Delete do what
they say, up and down go back through the last few lines, and Tab completes
a command or a file name, with a second Tab listing the choices.

It is built by hand, like everything else on the disk:

```sh
src/tsh/build.sh    # writes src/disk/usr/bin/tsh
```

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
address has nowhere to keep its parent's copy, so it cannot fork. That is
why `tsh` is built position-independent.

A pipe is therefore a buffer rather than a channel: the first program fills
it and finishes, then the second reads it. A writer that never finishes
never hands its pipe over.

### Starting a program quickly

On a machine like this, two things make a program slow to start, and neither
is the processor. A firmware disk read costs about the same whatever its
size, so what matters is how many times the disk is asked, not for how much.
And a program off a Linux system brings a loader and a two-megabyte C
library, most of which it never touches.

So a file mapping is a promise rather than a copy: `mmap`, and the program
loader itself, record where the memory is and what belongs there, and nothing
is read until the program touches it.

The rest is the disk cache, which stays off until `cache on` asks for it,
because megabytes are not something a kernel should spend without being
asked. `cache 3M` sets its size, three megabytes being what it has unless
told otherwise, and `cache off` turns it off again. With the cache on, the
loader and the C library are read once, and
every command after that starts without touching the disk: about two
milliseconds, against seventeen without it and a hundred before any of this.

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
│   ├── bin/            programs
│   ├── lib/            the libraries they were linked against
│   └── share/          terminfo, wallpapers, other shared data
└── var/
    ├── cache/
    ├── lib/            state a program keeps between runs
    └── log/            a file of syscalls per program, written while idle
```

`/dev`, `/proc` and `/sys` are empty folders on the disk. What appears in
them comes from the kernel.

**Boot.** `/etc/boot` describes the machine: the screen, the text size, the
wallpaper and the disk cache, each from a file of its own beside it -
`/etc/cache` holds the cache's size and whether it is on. The kernel runs it
itself before there is a shell. `/etc/shell` then names the program to
start, which is `/usr/bin/tsh`.

**Settings.** A program keeps its settings where Linux software expects:
dotfiles directly in the home folder, and everything else in
`~/.config/<program>/`.

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
