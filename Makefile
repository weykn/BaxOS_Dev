# Tuxlet OS - a UEFI-booted x86-64 kernel.
#
# The disk image is a GPT disk with two partitions: an EFI system partition
# holding the loader, which carries the kernel inside it, and a partition
# holding the filesystem - the kernel and every file in src/disk. tools/mkfs
# builds the second one; mtools and sgdisk build the first.

CC      := gcc
EFICC   := x86_64-w64-mingw32-gcc
HOSTCC  := gcc
NASM    := nasm
OBJCOPY := objcopy
EFIOBJCOPY := x86_64-w64-mingw32-objcopy

BUILD := build

# -mgeneral-regs-only keeps GCC away from SSE, which would fault because
# nothing has enabled it; -mno-red-zone is required for any kernel code.
# -fno-tree-loop-distribute-patterns stops GCC turning our own memset into a
# call to itself. -Oz, link-time optimisation, and per-function sections with
# --gc-sections keep the kernel - which sits in RAM whole - as small as
# possible: anything unreferenced is dropped.
# DEBUG=1 turns on src/kernel/debug.c, which writes to the emulator's debug
# port and nowhere else - and mirrors everything the console prints there, so
# that the machine can be driven and read without a screen. It is off by
# default because it is not free: a write to a port is a trap out of the
# virtual machine, and a program that starts by printing half a kilobyte of
# what it loaded spends longer on those traps than on the loading.

CFLAGS := -Isrc/kernel $(if $(filter 1,$(DEBUG)),-DDEBUG) -std=gnu11 -Oz -flto -Wall -Wextra \
          -ffreestanding -fno-builtin -nostdlib \
          -fpie -mno-red-zone -mgeneral-regs-only \
          -fno-stack-protector -fno-asynchronous-unwind-tables \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections

# The kernel is linked through gcc so LTO can run, as a static PIE at 0 that
# relocates itself (start.asm). --no-warn-rwx-segments: a flat binary has no
# segment permissions to enforce.
# -z pack-relative-relocs: the relocations as RELR, a bitmap of where they
# are, rather than 24 bytes apiece - a fortieth of the size, in an image
# that has to hold them for good.
LDFLAGS := -static-pie \
           -Wl,-n,--no-warn-rwx-segments,--gc-sections,--build-id=none,-T,src/kernel/kernel.ld \
           -Wl,-z,pack-relative-relocs

# The loader is built by a compiler that emits PE and speaks the calling
# convention UEFI uses; subsystem 10 is what makes firmware accept the file.
EFIFLAGS := -std=gnu11 -Oz -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
            -fno-stack-protector -mno-red-zone -fshort-wchar \
            -e efi_main -Wl,--subsystem,10

# legacy/ holds the drivers the BIOS path used, and is not built; see the
# README there.
KSRCS := $(shell find src/kernel -name '*.c')

# Kernel modules, Tuxlet's own format rather than Linux's: src/modules/
# <category>/<name>.c - or every .c in the folder <category>/<name>/ -
# becomes /usr/lib/modules/<category>/<name>.kmod on the disk, loaded by
# modman. Each is a shared object linked against nothing: what it calls is
# resolved by name when it is loaded (src/kernel/module.c). Hidden by
# default, so only what a module marks MODULE_EXPORT is seen by the others;
# SysV hashing, since the loader counts symbols by the hash table's chain.
MSRCS    := $(shell find src/modules -name '*.c')
MODULES  := $(sort $(foreach c,$(MSRCS),$(BUILD)/$(shell echo $(c:src/%=%) | \
                cut -d/ -f1-3 | sed 's/\.c$$//').kmod))
# No PLT and no CET landing pads: a call into the kernel goes through the GOT,
# and every byte counts, since a module is loaded in whole pages.
MCFLAGS  := $(filter-out -flto -fpie,$(CFLAGS)) -fPIC -fvisibility=hidden -fno-plt \
            -fcf-protection=none
# Its segments are packed rather than page-aligned: nothing maps a module,
# it is copied into RAM whole, and the alignment was most of what one cost.
MLDFLAGS := -shared -nostdlib -s \
            -Wl,--hash-style=sysv,-z,noseparate-code,--gc-sections,--build-id=none \
            -Wl,-z,max-page-size=16,-z,common-page-size=16,-z,norelro
# The package manager, src/tuxpac, is /usr/bin/tuxpac on the disk: a
# program like any other, but built here with the kernel - a static PIE,
# freestanding and alone, there being no libc on the disk to link against.
TUXPAC   := $(BUILD)/tuxpac
PCFLAGS  := -std=gnu11 -Oz -Wall -Wextra -ffreestanding -fno-builtin -nostdlib -static-pie \
            -fpie -fno-stack-protector -fno-asynchronous-unwind-tables \
            -ffunction-sections -fdata-sections -fcf-protection=none -Wa,-mx86-used-note=no \
            -Wl,--gc-sections,--build-id=none,-z,noexecstack,-z,noseparate-code,-z,norelro -s

KASMS := $(shell find src/kernel -name '*.asm' ! -name start.asm)
KOBJS := $(KSRCS:src/%.c=$(BUILD)/%.o) $(KASMS:src/%.asm=$(BUILD)/%.o)

# start.o must link first: its _start has to sit at the front of the binary,
# because that is the address the loader jumps to.
START_OBJ := $(BUILD)/kernel/start.o

# Everything under src/disk goes onto the disk exactly as it is, keeping the
# folders it sits in - the folders themselves included, so that one with
# nothing in it yet still arrives, and the symbolic links - /bin is one.
# Nothing here is built: programs are compiled on the host by hand.
DISK_ROOT  := src/disk
DISK_FILES := $(shell find $(DISK_ROOT) -mindepth 1 \( -type f -o -type d -o -type l \) 2>/dev/null)

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_OBJ := $(BUILD)/kernel_blob.o
LOADER     := $(BUILD)/BOOTX64.EFI
MKFS       := $(BUILD)/mkfs
FS_IMG     := $(BUILD)/fs.img
IMAGE      := $(BUILD)/TuxletOS.img

# The disk is as big as what is on it: the filesystem holds the kernel, the
# modules and src/disk, plus FREE for files saved from inside Tuxlet (any
# size numfmt reads: 0, 512K, 64M). Nothing may follow these on the line: a
# trailing comment leaves its spaces inside the value, and these get stuck
# straight onto sector numbers and onto sgdisk's "+1M".
FREE       ?= 1M

.PHONY: all clean run
.DELETE_ON_ERROR:                   # a recipe that fails leaves no half-built file

all: $(IMAGE)

# What the last build was built with. Objects depend on it, so changing a
# flag - DEBUG above all - rebuilds everything that flag reaches rather than
# leaving a half-and-half kernel that fails to link.
FLAGS_FILE := $(BUILD)/flags
$(shell mkdir -p $(BUILD); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = "$(CFLAGS)" ] || \
        printf '%s' "$(CFLAGS)" > $(FLAGS_FILE))

# And what the disk was sized with: a different FREE or src/disk resizes it.
SIZE_FILE := $(BUILD)/size
$(shell [ "$$(cat $(SIZE_FILE) 2>/dev/null)" = "$(FREE) $(DISK_ROOT)" ] || \
        printf '%s' "$(FREE) $(DISK_ROOT)" > $(SIZE_FILE))

$(BUILD)/%.o: src/%.c $(FLAGS_FILE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: src/%.asm $(FLAGS_FILE)
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/modules/%.o: src/modules/%.c $(FLAGS_FILE)
	@mkdir -p $(@D)
	$(CC) $(MCFLAGS) -MMD -MP -c $< -o $@

# A module's objects: its one file, or everything in its folder.
mod_objs = $(patsubst src/%.c,$(BUILD)/%.o,$(wildcard src/modules/$(1).c src/modules/$(1)/*.c))

.SECONDEXPANSION:
$(BUILD)/modules/%.kmod: $$(call mod_objs,$$*)
	$(CC) $(MLDFLAGS) $^ -o $@

$(TUXPAC): $(wildcard src/tuxpac/*.c src/tuxpac/*.h) Makefile
	@mkdir -p $(@D)
	$(CC) $(PCFLAGS) $(filter %.c,$^) -o $@

$(KERNEL_ELF): $(START_OBJ) $(KOBJS) src/kernel/kernel.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(START_OBJ) $(KOBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# The kernel goes inside the loader, so the boot partition holds one file and
# the loader needs no filesystem code to find it.
$(KERNEL_OBJ): $(KERNEL_BIN)
	cd $(BUILD) && $(EFIOBJCOPY) -I binary -O pe-x86-64 -B i386:x86-64 \
	    --redefine-sym _binary_kernel_bin_start=kernel_start \
	    --redefine-sym _binary_kernel_bin_end=kernel_end \
	    kernel.bin kernel_blob.o

$(LOADER): src/boot/uefi.c $(KERNEL_OBJ) src/kernel/boot.h src/kernel/efi.h
	$(EFICC) $(EFIFLAGS) src/boot/uefi.c $(KERNEL_OBJ) -o $@

# mkfs runs on the host but is built from the kernel's own fs.c. -iquote keeps
# the kernel's string.h from shadowing libc's.
$(MKFS): tools/mkfs.c src/kernel/fs.c src/kernel/fs.h src/kernel/ata.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -iquote src/kernel tools/mkfs.c src/kernel/fs.c -o $@

# The filesystem partition is updated in place rather than recreated, so files
# saved from inside Tuxlet OS survive a rebuild. `make clean` wipes them.
$(FS_IMG): $(KERNEL_BIN) $(MKFS) $(DISK_FILES) $(MODULES) $(TUXPAC) $(SIZE_FILE)
	$(MKFS) $@ $(shell numfmt --from=iec $(FREE)) $(DISK_ROOT) $(KERNEL_BIN) $(DISK_FILES) \
	    $(foreach m,$(MODULES),usr/lib/$(m:$(BUILD)/%=%)=$(m)) \
	    usr/bin/tuxpac=$(TUXPAC)

# Laid end to end, with nothing between: the GPT (sectors 0-33), the EFI
# system partition sized to the loader - plus 64 sectors for FAT12's own
# boot sector, tables, root folder and \EFI\BOOT - then the filesystem,
# then the backup GPT's 33. Nothing in the OS assumes where a partition
# starts: the loader reads its own, and storage/ide finds Tuxlet's by its
# magic number. sgdisk -a 1 after -o, which resets it, or it rounds each
# start up to 1 MiB.
$(IMAGE): $(LOADER) $(FS_IMG)
	@rm -f $@
	@esp=$$(( ($$(stat -c %s $(LOADER)) + 511) / 512 + 64 )); \
	 fs=$$(( $$(stat -c %s $(FS_IMG)) / 512 )); at=$$(( 34 + esp )); \
	 truncate -s $$(( (at + fs + 33) * 512 )) $@ && \
	 sgdisk -o -a 1 -n 1:34:+$$esp -t 1:ef00 -c 1:"EFI System" \
	        -n 2:$$at:+$$fs -t 2:8300 -c 2:"Tuxlet OS" $@ > /dev/null && \
	 mformat -i $@@@34s -T $$esp -v TUXLET :: && \
	 mmd -i $@@@34s ::/EFI ::/EFI/BOOT && \
	 mcopy -i $@@@34s $(LOADER) ::/EFI/BOOT/BOOTX64.EFI && \
	 dd if=$(FS_IMG) of=$@ bs=512 seek=$$at conv=notrunc status=none
	@echo "$@: $$(( $$(stat -c %s $@) / 1024 )) KiB, EFI system partition + filesystem"

-include $(KOBJS:.o=.d) $(patsubst src/%.c,$(BUILD)/%.d,$(MSRCS))

# OVMF stands in for a real machine's firmware. The variables file is copied
# so that boot entries written by the firmware do not dirty the system's.
OVMF_CODE := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS := /usr/share/edk2/x64/OVMF_VARS.4m.fd

# Hardware virtualisation where the machine has it. Without it QEMU interprets
# every instruction, and a program built for Linux spends most of its startup
# being interpreted rather than run: the same command takes five times as long.
ACCEL := $(shell test -w /dev/kvm && echo "-enable-kvm -cpu host")

# make run MEM=<size> NET=<card>: the machine's RAM, as QEMU takes it (39M,
# 1G), and its network card - e1000, rtl8139, virtio, the ones there are
# modules for, or none. The card comes without its network-boot ROM, which
# the firmware would otherwise load and keep: at 39M that is the memory the
# kernel needed. virtio has a driver in OVMF itself, ROM or not, and needs
# 42M. QEMU adds a card of its own unless told none.
MEM ?= 39M
NET ?= none

NET_DEVICE = $(if $(filter virtio,$(NET)),virtio-net-pci,$(NET))
NIC = $(if $(filter none,$(NET)),-nic none,-netdev user$(comma)id=n0 \
      -device $(NET_DEVICE)$(comma)netdev=n0$(comma)romfile=)
comma := ,

run: $(IMAGE)
	@cp -n $(OVMF_VARS) $(BUILD)/ovmf_vars.fd 2>/dev/null || true
	qemu-system-x86_64 $(ACCEL) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -drive format=raw,file=$(IMAGE) $(NIC) -m $(MEM)

clean:
	rm -rf $(BUILD)
