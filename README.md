# BaxOS

A small x86-64 operating system that boots through UEFI: its own kernel, its
own filesystem, its own shell, and enough of Linux's syscalls that programs
built for Linux - `bash`, `ed`, `grep`, `sed` - run on it unmodified.

    make        # build build/BaxOS.img
    make run    # and boot it in QEMU

Needs `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`, `mtools`, `gptfdisk`, and
`edk2-ovmf` for `make run`.

## How it is put together

The loader (`src/boot`) is a PE executable the firmware starts. It carries
the kernel inside itself, so the boot partition holds one file and the loader
needs no filesystem code to find it. It gathers the screen, the memory map
and the disk, then jumps to the kernel with that.

The kernel (`src/kernel`) runs in ring 0 and keeps the parts that only it
can: the screen and keyboard, a flat filesystem of contiguous files, and a
syscall table using Linux's numbers and Linux's arguments. There is one
program at a time and no fork.

Everything else is a program. The shell is `src/util/sh.c` and runs in ring 3
like anything else - reading a line, splitting it into words, looking a name
up on the path and running what it finds are all its own. The commands that
do need the kernel are files under `/proc`: `remap` and `/proc/remap` are the
same thing, because the shell looks the name up in the folders on its path
and `/proc` is one of them.

A program starts a program through `SYS_SPAWN`. There being no second address
space, the kernel copies out the pages the caller has actually touched, runs
the new program in the window, and puts them back when it ends.

## On the disk

    /conf/sys    the machine's settings; boot.conf is run by the kernel itself
    /conf/pkg    a program's own settings, the shell's among them
    /home        yours
    /log         a file of syscalls per program, written while the machine is idle
    /pkg         programs: bax-coreutils are ours, linux-coreutils are not
    /proc        the kernel's commands, which are not on the disk
    /tmp         scratch files programs make for themselves

`/conf/sys/boot.conf` is the machine - the screen, the text, the remaps, the
wallpaper - and the kernel reads and runs it before there is a shell.
`/conf/sys/shell.conf` then says which program to start, and that program
reads its own settings from `/conf/pkg`.

Everything under `src/disk` is copied onto the image exactly as it is, so a
file put there is on the machine at the next `make`. Files saved from inside
BaxOS survive a rebuild; `make clean` wipes them.

## Working on it

`legacy/` holds the drivers the BIOS path used, and is not built.

The machine can be driven without a screen: boot the image with `-display
none`, a monitor socket and `-debugcon file:boot.log`, then `sendkey` at the
monitor and read the log. Under `-DDEBUG` every character the console prints
goes to the debug port too, so `boot.log` is a transcript of the screen.
