#!/bin/sh
# Builds one Tuxlet OS program on the host, next to its source. The disk folder is
# embedded as it is, so this is run by hand and its output kept:
#
#   ./compile.sh hello_elf.asm      -> hello_elf,   an ELF64 executable
#   ./compile.sh -r hello_world.asm -> hello_world, a flat binary
#
# No special linker flags: ld's default address, 0x400000, is where the
# kernel's program window starts.
set -e

if [ "$1" = "-r" ]; then
    nasm -f bin "$2" -o "${2%.asm}"       # the source sets its own ORG 0x400000
else
    nasm -f elf64 "$1" -o "$1.o"
    ld -o "${1%.asm}" "$1.o"
    rm -f "$1.o"                          # or it lands on the disk image too
fi
