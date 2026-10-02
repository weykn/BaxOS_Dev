#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Kernel modules: parts of the kernel kept on the disk and loaded only when
 * asked for, from <folder>/<category>/<name>.kmod for a folder of MODPATH
 * (/usr/lib/modules, as /etc/tuxlet/env has it). The format is
 * Tuxlet's own - nothing like Linux's .ko, and no Linux module loads here.
 *
 * A module is an ELF shared object, built with -fPIC and linked -shared.
 * The loader puts it anywhere, applies its relocations, and resolves what
 * it calls by name: first against the kernel's exports (module.c), then
 * against the modules already loaded, whatever they mark MODULE_EXPORT.
 * Modules that work together - a card driver and the network stack - meet
 * in a slot the kernel keeps (driver.h, net.h) rather than calling each
 * other, so each one loads and runs on its own.
 *
 * It starts with module_init, which answers 0, or a negated errno - -ENODEV
 * when the hardware it drives is not there - to be unloaded again at once.
 * module_exit, if it has one, runs before it is unloaded and has to leave
 * nothing of it referenced - or answers -EBUSY, and the module stays. */

#define MODULE_EXPORT __attribute__((visibility("default")))

/* What a module defines. */
MODULE_EXPORT int  module_init(void);
MODULE_EXPORT int  module_exit(void);

#define MODULES_CONF "/etc/tuxlet/modules"

/* The script that loads them at boot, `modman enable` a line - run first by
   /etc/tuxlet/boot, before the firmware is let go of, since the disk and
   keyboard drivers the kernel needs for that are modules. modman enable and
   disable edit it. */
/* RAM the loaded modules' own images take, in bytes. */
uint32_t module_memory(void);

/* /proc/modman: every module on the disk is there to be had; enable loads
   one, now and at every boot, and disable unloads it; auto enables every
   module of a category that starts on this machine, and takeover lets the
   firmware go once the disk and keyboard are modules' - never by itself,
   only from where the boot script says so. With nothing, lists them.
   [enable|disable <category/name>|auto <category>|takeover] */
void module_command(char *args);
