#include "boot.h"
#include "debug.h"
#include "efi_kernel.h"
#include "fs.h"
#include "io.h"
#include "mem.h"
#include "shell.h"
#include "syscall.h"
#include "vga.h"

/* The folders the kernel goes to, made at boot if the disk lacks them -
   parents first. /ctl and /dev are the kernel's own; only cd and ls need
   them on the disk. /proc is Linux's, for the /proc/net files programs read. */
static const char *const folders[] = {
    "/ctl", "/proc", "/dev", "/etc", "/etc/tuxlet", "/usr", "/usr/lib", "/usr/lib/modules",
    "/var", "/var/log", "/root", "/tmp",
};

/* Where the UEFI loader leaves us, with the boot information it gathered.
   Nothing before vga_start can be shown, so a machine that gets this far and
   fails there has no way to say so. */
void kernel_main(struct boot_info *info) {
    if (info == NULL || info->magic != BOOT_MAGIC) {
        halt_forever();
    }
    efi_init(info);
    mem_trim_kernel();
    dbg("tuxlet: kernel up\n");
    if (vga_start(info) < 0) {
        halt_forever();
    }

    fs_cache(true);                 /* the firmware reads, for now */
    int err = fs_init();
    if (err < 0) {
        kprintf("fs: %s\n", fs_error(err));
    } else {
        for (unsigned i = 0; i < sizeof folders / sizeof folders[0]; i++) {
            fs_mkdir(folders[i]);   /* nothing written where it is already there */
        }
    }
    syscall_init();

    shell_run();                    /* never returns: poweroff is a command */
}
