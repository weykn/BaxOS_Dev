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
 * it calls by name against the kernel's exports (module.c). Only its code
 * and data stay in memory: the tables that link it are read from the file.
 * Modules that work together - a card driver and the network stack - meet
 * in a slot the kernel keeps (driver.h, net.h) rather than calling each
 * other, so each one loads and runs on its own.
 *
 * It starts with module_init, which answers 0, or a negated errno - -ENODEV
 * when the hardware it drives is not there - to be unloaded again at once.
 * module_exit, if it has one, runs before it is unloaded and has to leave
 * nothing of it referenced - or answers -EBUSY, and the module stays. */

#define MODULE_EXPORT __attribute__((visibility("default")))

/* What a module defines, and all it marks MODULE_EXPORT that is used. */
MODULE_EXPORT int  module_init(void);
MODULE_EXPORT int  module_exit(void);

/* Loads a module for the kernel's own use - true if it is in, now or
   already - and unloads it again. */
bool module_need(const char *name);
void module_drop(const char *name);

/* RAM the loaded modules' own images take, in bytes. */
uint32_t module_memory(void);

/* /ctl/modman: every module on the disk is there to be had; enable loads
   one and disable unloads it, nothing saved - what loads at boot is
   whatever the boot script enables; auto enables every
   module of a category that starts on this machine, and takeover lets the
   firmware go once the disk and keyboard are modules' - never by itself,
   only from where the boot script says so. With nothing, lists them.
   [enable|disable <category/name>|auto <category>|takeover] */
void module_command(char *args);
