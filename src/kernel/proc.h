#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The kernel's own commands, as files under /ctl (for control).
 *
 * Everything the shell cannot do for itself - the screen's size, the text's
 * size, what memory and the clock say, the syscall log - is
 * kernel state, and a command that changes it has to run in the kernel. So
 * each one is a file: `mem` at the prompt and /ctl/mem are the same thing, because the shell looks
 * the name up in /ctl like it looks any other command up on the path, and
 * running the file runs the command.
 *
 * /ctl is not on the disk. It is answered here: listing it gives the
 * commands, reading one gives its usage line, and running one calls it. */

struct proc_cmd {
    const char *name;
    const char *usage;              /* starts with '<' if an argument is required */
    void      (*run)(char *args);
};

/* Command i, or NULL past the last, for listing them. */
const struct proc_cmd *proc_at(unsigned i);

/* The command a path names - "/ctl/mem", or "mem" relative to /ctl -
   or NULL if the path is not one of them. */
const struct proc_cmd *proc_command(const char *path);

/* Whether a path names /ctl itself. */
bool proc_folder(const char *path);

/* What reading the file gives: its name and usage, one line. Returns how
   many bytes that is, and copies at most max of them from offset on. */
size_t proc_read(const struct proc_cmd *cmd, unsigned offset, char *out, size_t max);

/* A command a module brings, until it takes it away again. False if there
   is no room for another. */
bool proc_add(const struct proc_cmd *cmd);
void proc_remove(const struct proc_cmd *cmd);

/* Runs one, with the rest of the command line as its arguments. */
void proc_run(const struct proc_cmd *cmd, char *args);

/* Prints text as echo does - {bold} in and out of the highlight colour,
   {n} a new line, {uptime} the time since boot - and a newline. */
void proc_print(const char *text);

/* What greet set, printed that way, if it set anything: the boot calls
   it once its script is through. */
void proc_greet(void);

/* A labelled bar of used out of total, in unit, as `mem` draws one - for the
   commands modules bring. */
void usage_bar(const char *label, unsigned used, unsigned total, const char *unit);
