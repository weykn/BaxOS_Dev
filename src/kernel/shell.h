#pragma once

#include <stdbool.h>

/* Starts the shell, /usr/bin/tsh, which runs /etc/tuxlet/boot, and starts
   it again whenever it ends. Never returns: the machine is switched off or
   restarted by a command. */
__attribute__((noreturn)) void shell_run(void);

/* PATH or MODPATH as the last execve that had it gave it, or NULL. */
const char *shell_env(const char *name);

/* Keeps PATH and MODPATH out of an execve's environment. */
void shell_env_take(unsigned envc, const char *const *envv);
