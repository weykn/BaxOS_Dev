#include "console.h"

#include "ata.h"

#include "driver.h"
#include "efi_kernel.h"
#include "fs.h"
#include "keyboard.h"
#include "net.h"
#include "proc.h"
#include "shell.h"
#include "string.h"
#include "thread.h"
#include "vga.h"

/* When the last key arrived, so that work put off until the machine is idle
   waits for it to be idle rather than merely between two keystrokes. */
#define QUIET_MS 400
static uint64_t last_key;

/* What has to keep happening while nothing is running. There are no timer
   interrupts, so it happens when it is looked at - and waiting for a key is
   the only time anything is looking. */
static char idle(void) {
    vga_follow();                /* the firmware may have taken the screen
                                    back; this notices and repaints */
    /* The syscalls of whatever ran last, to /log - but only once the
       keyboard has been quiet for a moment. Writing costs a tenth of a
       second, and doing it the instant a command finishes put that delay in
       front of whoever was already typing the next one. */
    if (efi_uptime_ms() - last_key > QUIET_MS) {
        if (tracer != NULL) {
            tracer->flush();
        }
        ata_sync();              /* what was written, made to last */
        if (net != NULL) {
            net->idle();         /* an address, before anything asks */
        }
    }

    return 0;
}

/* ---- the settings --------------------------------------------------------
 *
 * A libc asks what the terminal is doing before its program says anything:
 * a shell told that its input is not a terminal reads it as it would a
 * script, silently and with no prompt. So the screen answers, and keeps the
 * settings it is handed. What matters of them is whether a line is gathered
 * here before the program sees it, and whether typing shows on the way.
 *
 * The shape is the kernel's own struct termios, which is what a libc puts on
 * the wire - not its own, which carries the line speeds as well. */

#define NCCS 19

struct termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line, cc[NCCS];
    uint32_t ispeed, ospeed;
};

_Static_assert(sizeof(struct termios) == 44, "struct termios2 is what Linux's is");

#define ICRNL  0x0100       /* a typed return arrives as a newline */
#define IXON   0x0400
#define OPOST  0x0001
#define ONLCR  0x0004
#define B38400 0x000F
#define CS8    0x0030
#define CREAD  0x0080
#define ISIG   0x0001
#define ICANON 0x0002       /* a line at a time, gathered here */
#define ECHO   0x0008       /* what is typed shows as it is typed */
#define ECHOE  0x0010
#define ECHOK  0x0020
#define IEXTEN 0x8000

#define VERASE 2
#define VEOF   4
#define VMIN   6

static struct termios settings;

static unsigned handed, ready;      /* a finished line not yet all read */

/* What has been typed and not yet read: taken off the keyboard as soon as
   anything looks, so that a Ctrl-C is seen however much is ahead of it -
   which, as on Linux, throws away what was ahead of it, and is never read
   as a key itself while ISIG is on. */
#define TYPED 32
#define VINTR 3                     /* Ctrl-C: SIGINT */
#define VQUIT 0x1C                  /* Ctrl-\: SIGQUIT */

static char     typed[TYPED];
static unsigned typed_n;
static int      signalled;          /* SIGINT or SIGQUIT, typed and not yet taken */

/* Ctrl-Cs typed in a row that nothing has read: three of them end the
   program whatever it does with Ctrl-C - the way out of one that has
   stopped reading its keys, which nothing on one console gives otherwise. */
#define FORCE_AFTER 3
#define FORCE_IDLE  1000            /* ms with nothing read: not merely typed fast */
static unsigned unread_intr;
static uint64_t last_read;

/* KDSKBMODE K_OFF: keys still reach /dev/input, but none is typed here -
   only the three Ctrl-Cs that end a program get through. */
static bool keys_off;

void console_keys(bool on) {
    keys_off = !on;
}

static void gather(void) {
    char c;

    while ((c = keyboard_poll_char(idle)) != 0) {
        last_key = efi_uptime_ms();
        unread_intr = c == VINTR ? unread_intr + 1 : 0;
        if (unread_intr >= FORCE_AFTER && efi_uptime_ms() - last_read >= FORCE_IDLE) {
            signalled = CONSOLE_FORCE;
            typed_n = 0;
        } else if (keys_off) {
            continue;
        } else if ((settings.lflag & ISIG) != 0 && (c == VINTR || c == VQUIT)) {
            signalled = c == VINTR ? 2 : 3;
            typed_n = 0;
        } else if (typed_n < TYPED) {
            typed[typed_n++] = c;
        }
    }
}

/* The next key, waiting for one - or 0 once a signal has been typed. */
static char take_key(void) {
    gather();
    while (typed_n == 0 && signalled == 0) {
        /* While other threads can run, the wait is theirs as well as the
           keyboard's. */
        if (thread_only()) {
            __asm__ volatile("pause");
        } else {
            thread_yield();
        }
        gather();
    }
    if (signalled != 0) {
        return 0;
    }
    char c = typed[0];

    memmove(typed, typed + 1, --typed_n);
    unread_intr = 0;                /* it is reading its keys */
    last_read = efi_uptime_ms();
    return c;
}

bool console_ready(void) {
    gather();
    return typed_n > 0 || signalled != 0;
}

int console_signal(void) {
    int sig;

    gather();
    sig = signalled;
    signalled = 0;
    if (sig == CONSOLE_FORCE) {
        unread_intr = 0;
    }
    return sig;
}

void console_reset(void) {
    memset(&settings, 0, sizeof settings);
    typed_n = 0;
    signalled = 0;
    unread_intr = 0;
    last_read = efi_uptime_ms();
    handed = ready = 0;             /* what the last one left unread goes */
    settings.iflag = ICRNL | IXON;
    settings.oflag = OPOST | ONLCR;
    settings.cflag = B38400 | CS8 | CREAD;
    settings.lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    settings.cc[VERASE] = '\b';     /* what the keyboard sends for it */
    settings.cc[VEOF] = 4;
    settings.cc[VMIN] = 1;
    vga_set_crlf(true);
    vga_text_reset();
}

void console_get(void *out, size_t size) {
    memcpy(out, &settings, size > sizeof settings ? sizeof settings : size);
}

void console_set(const void *in, size_t size) {
    memcpy(&settings, in, size > sizeof settings ? sizeof settings : size);
    vga_set_crlf((settings.oflag & (OPOST | ONLCR)) == (OPOST | ONLCR));
}

/* ---- the line editor -----------------------------------------------------
 *
 * A cooked read hands a program a finished line, so the editing is here and
 * every program that reads a line gets it: the arrows move through the line
 * and back through the last few, Home, End and Delete do what they say, and
 * Tab completes a name - a command in a folder of PATH for the first word,
 * a file for the rest, and a second Tab lists the choices.
 *
 * Where the line starts on screen is remembered, and every move is made
 * relative to it with the same escape sequences a program would send, so a
 * line that wraps, or scrolls the screen, is still edited in place. */

#define LINE_MAX    256     /* a line, the newline included */
#define HISTORY     512     /* past lines, each ending in a NUL, oldest first */

static char     line[LINE_MAX];
static unsigned have, pos;          /* its length, and where the cursor is in it */
static char     history[HISTORY];
static unsigned history_len;
static bool     drawing;            /* echo on: the screen follows the line */

static void draw(const char *text, unsigned n) {
    if (drawing) {
        while (n-- > 0) {
            vga_putc(*text++);
        }
    }
}

/* Moves the cursor from one place in the line to another. */
static void move(unsigned from, unsigned to) {
    unsigned width = vga_width();
    unsigned start = (unsigned)(vga_at() - from);   /* where the line began */
    unsigned row = (start + from) / width, want = (start + to) / width;
    char seq[16];

    if (!drawing || from == to) {
        return;
    }
    if (want != row) {
        ksprintf(seq, want > row ? "\033[%uB" : "\033[%uA",
                 want > row ? want - row : row - want);
        vga_puts(seq);
    }
    ksprintf(seq, "\033[%uG", (start + to) % width + 1);
    vga_puts(seq);
}

/* Redraws the line from pos to its end, blanking what an edit left behind
   past it, and puts the cursor back at pos. */
static void redraw(unsigned was) {
    draw(line + pos, have - pos);
    for (unsigned i = have; i < was; i++) {
        draw(" ", 1);
    }
    move(was > have ? was : have, pos);
}

static void insert(const char *text, unsigned n) {
    if (have + n >= LINE_MAX) {
        return;                     /* room for the newline, always */
    }
    memmove(line + pos + n, line + pos, have - pos);
    memcpy(line + pos, text, n);
    have += n;
    pos += n;
    draw(line + pos - n, n);
    redraw(have);
}

static void erase_at(unsigned at) {
    unsigned was = have;

    move(pos, at);
    pos = at;
    memmove(line + at, line + at + 1, have - at - 1);
    have--;
    redraw(was);
}

/* Puts text in place of the whole line, the cursor at its end. */
static void replace(const char *text, unsigned n) {
    unsigned was = have;

    move(pos, 0);
    pos = 0;
    memcpy(line, text, n);
    have = n;
    redraw(was);
    move(0, have);
    pos = have;
}

/* ---- history ---- */

static void remember(void) {
    unsigned n = have;

    if (n == 0 || n + 1 > HISTORY) {
        return;
    }
    while (history_len + n + 1 > HISTORY) {         /* the oldest make room */
        unsigned first = (unsigned)strlen(history) + 1;

        memmove(history, history + first, history_len - first);
        history_len -= first;
    }
    memcpy(history + history_len, line, n);
    history[history_len + n] = '\0';
    history_len += n + 1;
}

/* The line back steps from the newest, or NULL past the oldest. */
static const char *recalled(unsigned back) {
    unsigned end = history_len;

    while (end > 0) {
        unsigned start = end - 1;

        while (start > 0 && history[start - 1] != '\0') {
            start--;
        }
        if (back-- == 1) {
            return history + start;
        }
        end = start;
    }
    return NULL;
}

/* ---- completion ---- */

static char     common[FS_NAME_LEN];    /* what every match starts with */
static unsigned matches;

/* Takes one name that might complete what was typed; lists it instead when
   listing. */
static void candidate(const char *name, size_t n, const char *typed, size_t typed_n,
                      bool list) {
    if (n < typed_n || n >= sizeof common) {
        return;
    }
    for (size_t i = 0; i < typed_n; i++) {
        if (name[i] != typed[i]) {
            return;
        }
    }
    if (list) {
        draw(name, (unsigned)n);
        draw("  ", 2);
        return;
    }
    if (matches++ == 0) {
        memcpy(common, name, n);
        common[n] = '\0';
        return;
    }
    size_t same = 0;

    while (same < n && common[same] != '\0' && common[same] == name[same]) {
        same++;
    }
    common[same] = '\0';
}

/* Every name in folder, through candidate. /ctl is the kernel's commands. */
static void candidates(const char *folder, const char *typed, size_t typed_n, bool list) {
    struct fs_file entry;
    size_t cursor = 0, index;

    if (proc_folder(folder)) {
        for (unsigned i = 0; proc_at(i) != NULL; i++) {
            candidate(proc_at(i)->name, strlen(proc_at(i)->name), typed, typed_n, list);
        }
        return;
    }
    while (fs_list(folder, &cursor, &entry, &index) == 0) {
        const char *leaf = fs_inside(folder, entry.name);

        candidate(leaf, strlen(leaf), typed, typed_n, list);
    }
}

/* The same for the first word: every folder of PATH, or folder alone. */
static void choices(const char *folder, bool command, const char *typed, size_t typed_n,
                    bool list) {
    const char *dirs = command ? shell_env("PATH") : NULL;
    char dir[FS_NAME_LEN], name[FS_NAME_LEN];

    if (!command) {
        candidates(folder, typed, typed_n, list);
        return;
    }
    while (dirs != NULL && *dirs != '\0') {
        const char *end = strchr(dirs, ':');
        size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);

        if (n > 0 && n < sizeof dir) {
            memcpy(dir, dirs, n);
            dir[n] = '\0';
            if (proc_folder(dir)) {
                candidates(dir, typed, typed_n, list);
            } else if (fs_folder(dir, name, sizeof name) == 0) {
                candidates(name, typed, typed_n, list);
            }
        }
        dirs = end != NULL ? end + 1 : dirs + n;
    }
}

static void complete(bool list) {
    char folder[FS_NAME_LEN], dir[LINE_MAX];
    unsigned word = pos, slash;
    bool command = true;

    while (word > 0 && line[word - 1] != ' ') {
        word--;
    }
    for (unsigned i = 0; i < word; i++) {
        command = command && line[i] == ' ';
    }
    for (slash = pos; slash > word && line[slash - 1] != '/'; slash--) {
    }
    /* The folder named before the last slash of the word, or the working
       one; a command is looked for where the commands are. */
    if (slash > word) {
        memcpy(dir, line + word, slash - word);
        dir[slash - word] = '\0';
        command = false;
        if (fs_folder(dir, folder, sizeof folder) != 0) {
            return;
        }
    } else if (command) {
        folder[0] = '\0';          /* PATH's folders, in choices */
    } else {
        strcpy(folder, fs_cwd());
    }

    const char *typed = line + slash;
    size_t typed_n = pos - slash;

    if (list) {
        /* The choices under the line, then the prompt and the line again:
           the prompt is still on screen, so it is copied from there. */
        unsigned width = vga_width();
        size_t start = vga_at() - pos;
        unsigned column = (unsigned)(start % width), row = (unsigned)(start / width);
        char prompt[160];
        unsigned n = column < sizeof prompt ? column : sizeof prompt - 1;

        for (unsigned i = 0; i < n; i++) {
            prompt[i] = (char)vga_get(column - n + i, row);
        }
        move(pos, have);
        draw("\n", 1);
        choices(folder, command, typed, typed_n, true);
        draw("\n", 1);
        draw(prompt, n);
        draw(line, have);
        move(have, pos);
        return;
    }
    matches = 0;
    choices(folder, command, typed, typed_n, false);
    if (matches == 0) {
        return;
    }
    size_t n = strlen(common);
    bool folder_match = n > 0 && common[n - 1] == '/';

    insert(common + typed_n, (unsigned)(n - typed_n));
    if (matches == 1 && !folder_match) {
        /* A link to a folder is still a folder to go on into. */
        memcpy(dir, line + word, pos - word);
        dir[pos - word] = '\0';
        insert(!command && fs_folder_at(dir, &(unsigned){ 0 }) == 0 ? "/" : " ", 1);
    }
}

/* ---- reading ---- */

/* The key after an escape: the arrows and the rest, as "[" and a letter, or
   "[", a number and "~". Returns the letter, or the digit for a "~" one, or
   0 if it was not one of those - in which case the key after the escape is
   left in *other to be taken as typed. */
static char escape_key(char *other) {
    char c = take_key();

    *other = 0;
    if (c != '[') {
        *other = c;
        return 0;
    }
    c = take_key();
    if (c >= '0' && c <= '9') {
        char digit = c;

        while ((c = take_key()) != '~' && c >= '0' && c <= '9') {
        }
        return digit;
    }
    return c;
}

/* Edits a line until Enter, which is then what the reads hand out: 1, or 0
   if Ctrl-D came with nothing typed - the end of the input - or -1 if a
   signal was typed instead. */
static int edit_line(void) {
    unsigned back = 0;              /* how far into the history, 0 for the new line */
    bool tabbed = false;

    have = pos = 0;
    for (;;) {
        char c = take_key(), other = 0;
        bool tab = false;

        if (c == 0 && signalled != 0) {
            have = pos = 0;
            return -1;              /* the line goes with it */
        }
        if (c == 0x1B) {
            char key = escape_key(&other);

            if (key == 'A' || key == 'B') {
                const char *old = recalled(key == 'A' ? back + 1 : back - 1);

                if (key == 'A' && old != NULL) {
                    back++;
                } else if (key == 'B' && back > 0) {
                    back--;
                    old = back == 0 ? "" : recalled(back);
                } else {
                    old = NULL;
                }
                if (old != NULL) {
                    replace(old, (unsigned)strlen(old));
                }
            } else if (key == 'C' && pos < have) {
                move(pos, pos + 1);
                pos++;
            } else if (key == 'D' && pos > 0) {
                move(pos, pos - 1);
                pos--;
            } else if (key == 'H' || key == '1' || key == '7') {
                move(pos, 0);
                pos = 0;
            } else if (key == 'F' || key == '4' || key == '8') {
                move(pos, have);
                pos = have;
            } else if (key == '3' && pos < have) {
                erase_at(pos);
            }
            if (other == 0) {
                tabbed = false;
                continue;
            }
            c = other;              /* not a sequence: the key after it counts */
        }
        if (c == (char)settings.cc[VEOF]) {
            if (have == 0) {
                return 0;
            }
            continue;
        }
        if (c == (char)settings.cc[VERASE] || c == 0x7F) {
            if (pos > 0) {
                erase_at(pos - 1);
            }
        } else if (c == '\t') {
            complete(tabbed);
            tab = true;
        } else if (c == 0x15) {     /* Ctrl-U: the line before the cursor goes */
            unsigned n = pos;

            move(pos, 0);
            memmove(line, line + n, have - n);
            have -= n;
            pos = 0;
            redraw(have + n);
        } else if (c == '\n') {
            move(pos, have);
            draw("\n", 1);
            remember();
            line[have++] = '\n';
            return 1;
        } else if ((unsigned char)c >= ' ') {
            insert(&c, 1);
        }
        tabbed = tab;
    }
}

/* A line typed at the keyboard, edited as above, ending in the newline. A
   line longer than the buffer comes back in pieces, as it would on Linux.
   With ICANON turned off - which is what a program doing its own line
   editing does - a character comes back the moment it is typed and nothing
   is echoed unless ECHO says so. */
uint64_t console_read(char *buf, uint64_t count) {
    if ((settings.lflag & ICANON) == 0) {
        /* Whatever has been typed, up to count - so a key that sends a
           sequence arrives whole - waiting for the first only if VMIN asks
           for one. Return is "\r" unless ICRNL turns it into "\n". */
        uint64_t n = 0;

        if (settings.cc[VMIN] == 0 && !console_ready()) {
            return 0;
        }
        do {
            char c = take_key();

            if (c == 0 && signalled != 0) {
                return n > 0 ? n : CONSOLE_SIGNAL;
            }
            if (c == '\n' && (settings.iflag & ICRNL) == 0) {
                c = '\r';
            }
            buf[n++] = c;
            if ((settings.lflag & ECHO) != 0) {
                vga_putc(c);
            }
        } while (n < count && console_ready());
        return n;
    }
    if (handed == ready) {
        drawing = (settings.lflag & ECHO) != 0;
        handed = 0;
        int got = edit_line();

        ready = got > 0 ? have : 0;
        if (got < 0) {
            return CONSOLE_SIGNAL;
        }
        if (ready == 0) {
            return 0;               /* Ctrl-D on an empty line */
        }
    }
    uint64_t n = ready - handed < count ? ready - handed : count;

    memcpy(buf, line + handed, n);
    handed += (unsigned)n;
    return n;
}
