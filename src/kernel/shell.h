#pragma once

#include <stdbool.h>

/* Runs /etc/tuxlet/boot, then reads and runs commands. Never returns: the
   machine is switched off or restarted by a command. */
__attribute__((noreturn)) void shell_run(void);

/* A variable of the machine's environment, as `export` set it, or NULL. */
const char *shell_env(const char *name);

/* /ctl/tsh, run by a program rather than by the machine. */
void shell_tsh(char *args);
