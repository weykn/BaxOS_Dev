#pragma once

/* Runs /etc/tuxlet/boot, then reads and runs commands. Never returns: the
   machine is switched off or restarted by a command. */
__attribute__((noreturn)) void shell_run(void);

/* /proc/tsh, run by a program rather than by the machine. */
void shell_tsh(char *args);
