#include "boot.h"
#include "debug.h"
#include "efi_kernel.h"
#include "fs.h"
#include "io.h"
#include "mem.h"
#include "shell.h"
#include "string.h"
#include "syscall.h"
#include "vga.h"

/* The folders the kernel goes to, made at boot if the disk lacks them -
   parents first. /ctl and /dev are the kernel's own; only cd and ls need
   them on the disk. /proc and /sys are Linux's, for the files programs read
   about the machine; /sys/class/drm/card0 is listed for its modes file, and
   /sys/class/graphics/fb0/device/subsystem says the screen is on no PCI bus -
   the firmware's framebuffer - which an X server's fbdev driver checks before
   it will use /dev/fb0.

   The input devices are laid out as Linux's sysfs and udev's database lay
   them out - /sys/class/input/eventN with its numbers and its subsystem,
   /run/udev/data/c13:N saying a keyboard or a mouse - which is how a program
   built for udev, an X server among them, finds them by itself. eventN is a
   folder rather than Linux's link into the device tree: a udev library
   walks a path a part at a time, and opens none of it as a link. */
static const char *const folders[] = {
    "/ctl", "/proc", "/dev", "/etc", "/etc/tuxlet", "/usr", "/usr/lib", "/usr/lib/modules",
    "/var", "/var/log", "/root", "/tmp",
    "/sys", "/sys/class", "/sys/class/drm", "/sys/class/drm/card0", "/sys/class/graphics",
    "/sys/class/graphics/fb0", "/sys/class/graphics/fb0/device",
    "/sys/class/input", "/sys/class/input/event0", "/sys/class/input/event1",
    "/run", "/run/udev", "/run/udev/data",
};

/* Files made the same way: once, when the disk lacks them. */
static const char *const files[][2] = {
    { "/sys/class/input/event0/uevent", "MAJOR=13\nMINOR=64\nDEVNAME=input/event0\n" },
    { "/sys/class/input/event0/dev", "13:64\n" },
    { "/sys/class/input/event1/uevent", "MAJOR=13\nMINOR=65\nDEVNAME=input/event1\n" },
    { "/sys/class/input/event1/dev", "13:65\n" },
    { "/run/udev/data/c13:64", "E:ID_INPUT=1\nE:ID_INPUT_KEY=1\nE:ID_INPUT_KEYBOARD=1\n" },
    { "/run/udev/data/c13:65", "E:ID_INPUT=1\nE:ID_INPUT_MOUSE=1\n" },
};

/* And links: where they point, and where they are. */
static const char *const links[][2] = {
    { "../../../../bus/platform", "/sys/class/graphics/fb0/device/subsystem" },
    { "../../input", "/sys/class/input/event0/subsystem" },
    { "../../input", "/sys/class/input/event1/subsystem" },
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
        struct fs_file there;

        for (unsigned i = 0; i < sizeof folders / sizeof folders[0]; i++) {
            fs_mkdir(folders[i]);   /* nothing written where it is already there */
        }
        for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
            if (fs_stat(files[i][0], &there) != 0) {
                fs_write(files[i][0], files[i][1], strlen(files[i][1]));
            }
        }
        for (unsigned i = 0; i < sizeof links / sizeof links[0]; i++) {
            fs_symlink(links[i][0], links[i][1]);
        }
    }
    syscall_init();

    shell_run();                    /* never returns: poweroff is a command */
}
