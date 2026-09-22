#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The kernel's own commands, as files under /proc.
 *
 * Everything the shell cannot do for itself - the screen's size, the text's
 * size, the wallpaper, what memory and the clock say, the folders standing
 * in for other folders, the syscall log - is kernel state, and a command
 * that changes it has to run in the kernel. So each one is a file: `remap`
 * at the prompt and /proc/remap are the same thing, because the shell looks
 * the name up in /proc like it looks any other command up on the path, and
 * running the file runs the command.
 *
 * /proc is not on the disk. It is answered here: listing it gives the
 * commands, reading one gives its usage line, and running one calls it. */

struct proc_cmd {
    const char *name;
    const char *usage;              /* starts with '<' if an argument is required */
    void      (*run)(char *args);
};

/* Command i, or NULL past the last, for listing them. */
const struct proc_cmd *proc_at(unsigned i);

/* The command a path names - "/proc/remap", or "remap" relative to /proc -
   or NULL if the path is not one of them. */
const struct proc_cmd *proc_command(const char *path);

/* Whether a path names /proc itself. */
bool proc_folder(const char *path);

/* What reading the file gives: its name and usage, one line. Returns how
   many bytes that is, and copies at most max of them from offset on. */
size_t proc_read(const struct proc_cmd *cmd, unsigned offset, char *out, size_t max);

/* Runs one, with the rest of the command line as its arguments. */
void proc_run(const struct proc_cmd *cmd, char *args);
