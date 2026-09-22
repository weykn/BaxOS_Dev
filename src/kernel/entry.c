#include "boot.h"
#include "debug.h"
#include "efi_kernel.h"
#include "fs.h"
#include "io.h"
#include "shell.h"
#include "status.h"
#include "syscall.h"
#include "vga.h"

/* Where the UEFI loader leaves us, with the boot information it gathered.
   Nothing before vga_start can be shown, so a machine that gets this far and
   fails there has no way to say so. */
void kernel_main(struct boot_info *info) {
    if (info == NULL || info->magic != BOOT_MAGIC) {
        halt_forever();
    }
    efi_init(info);
    dbg("baxos: kernel up\n");
    if (vga_start(info) < 0) {
        halt_forever();
    }

    int err = fs_init();
    if (err < 0) {
        kprintf("fs: %s\n", fs_error(err));
    }
    syscall_init();
    status_init();

    /* The mouse waits with the drivers it may need: the shell starts both
       once it is idle, rather than the machine starting slower for them. */
    shell_run();                    /* never returns: poweroff is a command */
}
