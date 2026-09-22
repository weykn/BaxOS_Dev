#include "util.h"

/* What the machine is, at a glance: the name in block letters, and beside it
 * the handful of figures worth knowing. Everything here is asked for through
 * an ordinary syscall - uname, sysinfo, statfs, and the terminal's own size -
 * so nothing about it is privileged, and a program off a Linux system could
 * ask the same questions and get the same answers. */

/* The column the figures start in: the widest row of the logo, and two
   spaces to hold the two columns apart. Worth re-measuring if the logo
   changes shape. */
#define LOGO_W 23
#define SHELL_CONF "/conf/sys/shell.conf"

static const char *const logo[] = {
    "",
    "  ____    _    __  __",
    " | __ )  / \\   \\ \\/ /",
    " |  _ \\ / _ \\   \\  /",
    " | |_) / ___ \\  /  \\",
    " |____/_/   \\_\\/_/\\_\\",
    "",
};

#define LOGO_ROWS (sizeof logo / sizeof logo[0])

static unsigned row;            /* how far down the two columns have got */

/* Starts a line: the logo's row for it, padded out to where the figures go. */
static void begin(void) {
    const char *art = row < LOGO_ROWS ? logo[row] : "";

    put(ACCENT);
    put(art);
    put(PLAIN);
    put_spaces(ulen(art) < LOGO_W ? LOGO_W - ulen(art) : 1);
    row++;
}

static void blank(void) {
    begin();
    put("\n");
}

/* A figure: its name, then what it is. */
static void label(const char *name) {
    begin();
    put(ACCENT);
    put(name);
    put(DIM);
    put_spaces(ulen(name) < 12 ? 12 - ulen(name) : 1);
    put(PLAIN);
}

static void field(const char *name, const char *value) {
    label(name);
    put(value);
    put("\n");
}

/* A number, then whatever follows it - a unit, or the second half of "x of
   y". Printing the number needs a call of its own, so a line is built out of
   pieces rather than formatted. */
static void number(uint64_t value, const char *after) {
    put_number(value, 0);
    put(DIM);
    put(after);
    put(PLAIN);
}

/* Blocks to KiB, rounded up - which is how the title bar counts them too,
   so that the two agree: half a kilobyte in use is a kilobyte that is not
   free. */
static uint64_t kib_used(uint64_t blocks, int64_t bsize) {
    return (blocks * (uint64_t)bsize + 1023) / 1024;
}

/* ---- what there is to tell ---------------------------------------------- */

/* The processor's own name for itself, which it will spell out in three
   twelve-byte pieces if it is willing to at all. */
static bool cpu_name(char *out) {
    unsigned a, b, c, d;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (a < 0x80000004u) {
        return false;
    }
    for (unsigned leaf = 0; leaf < 3; leaf++) {
        unsigned *at = (unsigned *)(out + leaf * 16);

        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000002u + leaf));
        at[0] = a;
        at[1] = b;
        at[2] = c;
        at[3] = d;
    }
    out[48] = '\0';
    while (*out == ' ') {           /* they pad the front of it */
        for (char *p = out; *p != '\0'; p++) {
            p[0] = p[1];
        }
    }
    return true;
}

/* The first line of the file naming the shell, which is the shell. */
static bool shell_name(char *out, long max) {
    long got;
    int fd = (int)sys_open(SHELL_CONF, O_RDONLY);

    if (fd < 0) {
        return false;
    }
    got = sys_read(fd, out, max - 1);
    sys_close(fd);
    if (got <= 0) {
        return false;
    }
    out[got] = '\0';
    for (long i = 0; i < got; i++) {
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = '\0';
            break;
        }
    }
    /* The last part of the path: the shell is `sh`, not where it lives. */
    for (long i = (long)ulen(out); i > 0; i--) {
        if (out[i - 1] == '/') {
            ucopy(out, out + i, ulen(out + i) + 1);
            break;
        }
    }
    return out[0] != '\0';
}

static void uptime(int64_t seconds) {
    if (seconds >= 3600) {
        number((uint64_t)seconds / 3600, "h ");
    }
    if (seconds >= 60) {
        number((uint64_t)(seconds / 60 % 60), "m ");
    }
    number((uint64_t)(seconds % 60), "s");
    put("\n");
}

int main(int argc, char **argv) {
    struct utsname machine;
    struct sysinfo info;
    struct winsize screen;
    struct statfs disk;
    char text[64];

    (void)argc;
    (void)argv;
    blank();
    if (sys_uname(&machine) == 0) {
        size_t n = ulen(machine.nodename);

        begin();
        put(BRIGHT);
        put(machine.nodename);
        put(PLAIN "\n");
        begin();
        put(DIM);
        for (size_t i = 0; i < n && i < sizeof text - 1; i++) {
            text[i] = '-';
        }
        text[n < sizeof text - 1 ? n : sizeof text - 1] = '\0';
        put(text);
        put(PLAIN "\n");
        field("os", machine.sysname);
        field("arch", machine.machine);
        field("kernel", machine.release);
    }
    if (sys_sysinfo(&info) == 0) {
        label("uptime");
        uptime(info.uptime);
        label("memory");
        number((info.totalram - info.freeram) / 1024, " of ");
        number(info.totalram / 1024, " KiB");
        put("\n");
    }
    if (sys_statfs("/", &disk) == 0) {
        label("disk");
        number(kib_used(disk.blocks - disk.bfree, disk.bsize), " of ");
        number((uint64_t)disk.blocks * (uint64_t)disk.bsize / 1024, " KiB");
        put("\n");
    }
    if (sys_winsize(&screen) == 0) {
        label("screen");
        number(screen.pixel_w, "x");
        number(screen.pixel_h, "");
        put("\n");
        label("console");
        number(screen.columns, "x");
        number(screen.rows, " cells");
        put("\n");
    }
    if (shell_name(text, sizeof text)) {
        field("shell", text);
    }
    if (cpu_name(text)) {
        field("cpu", text);
    }
    while (row < LOGO_ROWS) {
        blank();
    }
    return 0;
}
