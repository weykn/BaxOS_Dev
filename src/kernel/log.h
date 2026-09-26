#pragma once

#include <stddef.h>
#include <stdint.h>

/* A record of the syscalls programs make, and who made them.
 *
 * Every call through syscall_dispatch is kept here, in a ring, and written
 * out to /var/log/<program>.log - one file per executable, which is what makes a
 * program's own calls readable without the rest of the machine's in the way.
 *
 * When that happens is the whole of the difference between a machine that
 * feels quick and one that does not. Writing a line as each call is made is
 * out of the question: the firmware charges about ten milliseconds for a
 * write however small it is, and SYS_WRITE would be re-entering the very
 * code doing the logging. So the ring is written when the machine is next
 * idle - which is the moment after a command finishes, when nobody is
 * waiting on it.
 *
 * A program that makes more calls than the ring holds before it ever goes
 * idle loses the oldest of them, and its file keeps the last LOG_SIZE - which
 * is where a program that went wrong went wrong. Writing them out as the ring
 * filled would be a disk write in the middle of a running program, and that
 * is felt as one. */

/* Entries held before they go to disk. Enough that what a program did
   survives until the machine is next idle: a command makes a few dozen calls
   and the shell a few more before anything is written, and a ring smaller
   than the two together loses the command's before its file is made. */
#define LOG_SIZE 64
#define LOG_LINE 128            /* one formatted line, at its longest */
#define LOG_NAME 20             /* a program's name in the log, with its NUL */

struct log_entry {
    uint64_t a, b, c;           /* the call's arguments */
    uint64_t result;
    uint32_t number;            /* the syscall number, as in syscall.h */
    uint8_t  who;               /* which program: log_who spells it out */
    uint8_t  returned;          /* false while the call is still running */
};

/* Names the program whose calls are being recorded from here on, and gives
   back the name that was being recorded before - which a program that starts
   another puts back when that one ends. NULL is the kernel's own. */
const char *log_program(const char *name);

/* The name that goes with an entry, or "kernel". */
const char *log_who(const struct log_entry *entry);

/* Records the start of a call and returns the entry to finish, or NULL if
   logging is off. */
struct log_entry *log_begin(uint32_t number, uint64_t a, uint64_t b, uint64_t c);

/* Fills in what the call returned. A call that never returns - SYS_EXIT, or a
   program killed mid-call - simply keeps its entry unfinished. */
void log_end(struct log_entry *entry, uint64_t result);

/* Entries held right now, at most LOG_SIZE. */
unsigned log_count(void);

/* Syscalls made since boot, including any already written out. */
unsigned log_total(void);

/* How many were dropped rather than kept, because the ring filled before the
   machine was next idle. */
unsigned log_dropped(void);

/* Entry i, counting 0 as the oldest one still held. */
const struct log_entry *log_get(unsigned i);

/* Writes one record into buf, which must have room for LOG_LINE bytes, and
   returns its length. */
size_t log_format(const struct log_entry *entry, char *buf);

/* Appends what is held to /var/log/<program>.log, a file per program, and empties
   the ring. Called when the ring fills and when a program ends; a folder
   named /var/log is made if there is none. */
void log_flush(void);

void log_clear(void);

/* Whether calls are being recorded. Logging starts on. */
int  log_enabled(void);
void log_enable(int on);

/* The name of a syscall number, or NULL if this kernel has none. */
const char *log_name(uint64_t number);

/* The name of a negative result - "ENOSYS" and the like - or NULL if the
   call succeeded. */
const char *log_error(uint64_t result);
