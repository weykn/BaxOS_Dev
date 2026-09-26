#pragma once

/* Runs /etc/boot, then reads and runs commands. Never returns: the
   machine is switched off or restarted by a command. */
__attribute__((noreturn)) void shell_run(void);
