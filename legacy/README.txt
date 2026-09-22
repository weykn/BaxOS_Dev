The BIOS boot path. Kept, not built.

BaxOS boots through UEFI now: the screen comes from the firmware's Graphics
Output Protocol, the keyboard from its text input protocol, the disk from its
block I/O protocol and the clock from its runtime services - all of which is
in src/kernel/efi.c, with the loader in src/boot/uefi.c.

These are the files that did those jobs when the machine was started by a
512-byte boot sector instead:

  boot.asm      the boot sector: real mode, INT 13h to read the disk, INT 10h
                for the VGA BIOS fonts, then straight into long mode
  vbe.c, vbe.h  graphics through the Bochs display registers, which only ever
                existed in QEMU, Bochs and VirtualBox - never on real hardware
  ata.c         the disk over legacy IDE ports, which modern machines do not
                have
  keyboard.c    the PS/2 controller, likewise, and scancode tables for it

Nothing here is referenced by the build. They are kept because between them
they are the only record of how the machine used to start, and because a BIOS
target could be rebuilt from them: ata.c and keyboard.c answer exactly the
same ata.h and keyboard.h that efi.c does now.
