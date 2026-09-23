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
          -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
          -fno-stack-protector -fno-asynchronous-unwind-tables \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections

# The kernel is linked through gcc so LTO can run. --no-warn-rwx-segments: a
# flat binary has no segment permissions to enforce.
LDFLAGS := -static -no-pie \
           -Wl,-n,--no-warn-rwx-segments,--gc-sections,--build-id=none,-T,src/kernel/kernel.ld

# The loader is built by a compiler that emits PE and speaks the calling
# convention UEFI uses; subsystem 10 is what makes firmware accept the file.
EFIFLAGS := -std=gnu11 -Oz -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
            -fno-stack-protector -mno-red-zone -fshort-wchar \
            -e efi_main -Wl,--subsystem,10

# legacy/ holds the drivers the BIOS path used, and is not built; see the
# README there.
KSRCS := $(shell find src/kernel -name '*.c')
KASMS := $(shell find src/kernel -name '*.asm' ! -name start.asm)
KOBJS := $(KSRCS:src/%.c=$(BUILD)/%.o) $(KASMS:src/%.asm=$(BUILD)/%.o)

# start.o must link first: its _start has to sit at the front of the binary,
# because that is the address the loader jumps to.
START_OBJ := $(BUILD)/kernel/start.o

# Everything under src/disk goes onto the disk exactly as it is, keeping the
# folders it sits in - the folders themselves included, so that one with
# nothing in it yet still arrives. src/disk/ROBOT.md says what each is for.
# Nothing here is built: programs are compiled on the host by hand.
DISK_ROOT  := src/disk
DISK_FILES := $(shell find $(DISK_ROOT) -mindepth 1 \( -type f -o -type d \) 2>/dev/null)

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_OBJ := $(BUILD)/kernel_blob.o
LOADER     := $(BUILD)/BOOTX64.EFI
MKFS       := $(BUILD)/mkfs
FS_IMG     := $(BUILD)/fs.img
IMAGE      := $(BUILD)/TuxletOS.img

# Sizes, in the units their names give. FS_SECTORS is FS_MIB as sectors. Nothing may follow these on the line:
# a trailing comment leaves its spaces inside the value, and these get stuck
# straight onto sector numbers and onto sgdisk's "+48M".
FS_SECTORS := 131072
ESP_MIB    := 48
FS_MIB     := 64
DISK_MIB   := 128
ESP_LBA    := 2048

FS_LBA      := $(shell expr $(ESP_LBA) + $(ESP_MIB) \* 2048)
ESP_SECTORS := $(shell expr $(ESP_MIB) \* 2048)
ESP_AT      := $(IMAGE)@@$(ESP_LBA)s

.PHONY: all clean run

all: $(IMAGE)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: src/%.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

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
$(FS_IMG): $(KERNEL_BIN) $(MKFS) $(DISK_FILES)
	$(MKFS) $@ $(FS_SECTORS) $(DISK_ROOT) $(KERNEL_BIN) $(DISK_FILES)

$(IMAGE): $(LOADER) $(FS_IMG)
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=1M count=$(DISK_MIB) status=none
	@sgdisk -o -n 1:$(ESP_LBA):+$(ESP_MIB)M -t 1:ef00 -c 1:"EFI System" \
	        -n 2:$(FS_LBA):+$(FS_MIB)M -t 2:8300 -c 2:"Tuxlet OS" $@ > /dev/null
	@mformat -i $(ESP_AT) -T $(ESP_SECTORS) -F -v TUXLET ::
	@mmd -i $(ESP_AT) ::/EFI ::/EFI/BOOT
	@mcopy -i $(ESP_AT) $(LOADER) ::/EFI/BOOT/BOOTX64.EFI
	@dd if=$(FS_IMG) of=$@ bs=512 seek=$(FS_LBA) conv=notrunc status=none
	@echo "$@: EFI system partition + filesystem"

-include $(KOBJS:.o=.d)

# OVMF stands in for a real machine's firmware. The variables file is copied
# so that boot entries written by the firmware do not dirty the system's.
OVMF_CODE := /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS := /usr/share/edk2/x64/OVMF_VARS.4m.fd

# Hardware virtualisation where the machine has it. Without it QEMU interprets
# every instruction, and a program built for Linux spends most of its startup
# being interpreted rather than run: the same command takes five times as long.
ACCEL := $(shell test -w /dev/kvm && echo "-enable-kvm -cpu host")

run: $(IMAGE)
	@cp -n $(OVMF_VARS) $(BUILD)/ovmf_vars.fd 2>/dev/null || true
	qemu-system-x86_64 $(ACCEL) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -drive format=raw,file=$(IMAGE) -net none -m 256M

clean:
	rm -rf $(BUILD)
