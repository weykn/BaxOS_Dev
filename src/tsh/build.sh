#!/bin/sh
# Builds tsh into the disk, as /usr/bin/tsh. Run by hand, like everything on
# the disk: the Makefile embeds src/disk as it is and builds nothing in it.
# Position-independent, because fork refuses a program linked to a fixed
# address: there is only the one window to keep a parent in.
set -e
cd "$(dirname "$0")"
gcc -std=gnu11 -Os -static -nostdlib -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-asynchronous-unwind-tables -fpie -static-pie \
    -ffunction-sections -fdata-sections -Wl,--gc-sections,--build-id=none,-z,noseparate-code \
    -Wall -Wextra -s tsh.c -o ../disk/usr/bin/tsh
