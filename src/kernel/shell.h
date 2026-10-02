#pragma once

#include <stdbool.h>

/* Runs /etc/tuxlet/boot, then reads and runs commands. Never returns: the
   machine is switched off or restarted by a command. */
__attribute__((noreturn)) void shell_run(void);

/* Whether the configuration script at path is the one being run: a table
   name, as the filesystem has it, is compared, so path may start with '/'. */
bool shell_running(const char *path);

/* A variable of the machine's environment, as `export` set it, or NULL. */
const char *shell_env(const char *name);

/* /proc/tsh, run by a program rather than by the machine. */
void shell_tsh(char *args);
