![Tuxlet](TUXLET.png)

# Tuxlet OS

A small x86-64 operating system that boots through UEFI: its own kernel, its
own filesystem, and enough of Linux's syscalls that programs built for Linux
run on it unmodified. There is no shell of its own - the shell is `bash`, off
an ordinary Linux system, and so is everything it runs.

    make        # build build/TuxletOS.img
    make run    # and boot it in QEMU

Needs `gcc`, `nasm`, `x86_64-w64-mingw32-gcc`, `mtools`, `gptfdisk`, and
`edk2-ovmf` for `make run`.

## How it is put together

The loader (`src/boot`) is a PE executable the firmware starts. It carries
the kernel inside itself, so the boot partition holds one file and the loader
needs no filesystem code to find it. It gathers the screen, the memory map
and the disk, then jumps to the kernel with that.

The kernel (`src/kernel`) runs in ring 0 and keeps the parts that only it
can: the screen and keyboard as one plain terminal, a flat filesystem of
contiguous files, and a syscall table using Linux's numbers and Linux's
arguments.

Everything else is a program off the disk, `bash` included. A program gets a
region of its own - half a terabyte of address space hung off a spare slot of
the firmware's page tables, bought a page at a time as it is touched. The few
commands that are the kernel's own are files under `/proc`: `mem` and
`/proc/mem` are the same thing, because `/proc` is on `PATH`.

### fork, without a scheduler

There is one processor and no scheduler, so the two halves of a fork cannot
run side by side - but they do not have to. `fork` runs its child there and
then, to the end, and only answers its parent once the child has finished.

What makes that safe is that the child, until it calls `execve`, is running in
its parent's own memory: every page of it is write-protected at the fork, and
the first write to one copies what was under it aside. The child's changes are
undone page by page when it finishes, and the parent carries on as if it had
only been waiting. `execve` is where the child stops being its parent - it
takes a region of its own, and the parent's is left exactly as the fork found
it.

A pipe is a buffer rather than a channel, for the same reason: the first
program fills it and finishes, and the second reads it. `a | b` works, and so
does `$(...)`; a pipeline whose first half never ends does not.

### Starting a program quickly

Two things make a program start slowly on a machine like this, and neither is
the processor. A firmware disk read costs about the same whatever its size -
three hundred microseconds of round trip - so what matters is how many times
the disk is asked, not for how much. And a program off a Linux system arrives
with a loader and a two-megabyte C library, most of which it never touches.

So a file mapping is a promise rather than a copy: `mmap`, and the program
loader itself, write down where the memory is and what belongs there, and
nothing is read until the program touches it. And what is read is kept - the
same C library serves every command after the first, from memory (`ata.h`).
Between them, a command starts in about seven milliseconds where it used to
take a hundred.

## On the disk

    /conf/sys    the machine's settings, and the ones a Linux program looks
                 for in /etc; `boot` is run by the kernel itself
    /conf/pkg    a program's own settings
    /dev         null, zero, tty and the rest, which are not on the disk
    /home        yours, and where .bashrc lives
    /log         a file of syscalls per program, written while the machine is idle
    /pkg         programs, and the libraries they were linked against
    /proc        the kernel's commands, which are not on the disk
    /tmp         scratch files programs make for themselves

`/conf/sys/boot` is the machine - the screen, the text, the remaps, the
wallpaper - and the kernel reads and runs it before there is a shell.
`/conf/sys/shell` then says which program to start, which is `bash`.

`/conf/sys/remap` is what makes a Linux program feel at home: `/bin`,
`/lib`, `/etc` and the rest are folders standing in for the ones this disk
actually has. The swap happens inside the filesystem, so it holds for every
program and for reading, writing and listing alike.

Everything under `src/disk` is copied onto the image exactly as it is, so a
file put there is on the machine at the next `make`. Files saved from inside
Tuxlet OS survive a rebuild; `make clean` wipes them.

## Working on it

`legacy/` holds the drivers the BIOS path used, and is not built.

The machine can be driven without a screen: boot the image with `-display
none`, a monitor socket and `-debugcon file:boot.log`, then `sendkey` at the
monitor and read the log. Under `-DDEBUG` every character the console prints
goes to the debug port too, so `boot.log` is a transcript of the screen.
