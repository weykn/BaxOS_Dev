#include "sys.h"

#include <stdarg.h>

/* ---- syscalls -------------------------------------------------------------- */

static long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SC(n, a, b, c) syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

long sys_read(int fd, void *buf, size_t n)            { return SC(0, fd, buf, n); }
long sys_write(int fd, const void *buf, size_t n)     { return SC(1, fd, buf, n); }
long sys_open(const char *p, int flags, int mode)     { return SC(2, p, flags, mode); }
long sys_close(int fd)                                { return SC(3, fd, 0, 0); }
long sys_stat(const char *p, struct stat *st)         { return SC(4, p, st, 0); }
long sys_fstat(int fd, struct stat *st)               { return SC(5, fd, st, 0); }
long sys_lstat(const char *p, struct stat *st)        { return SC(6, p, st, 0); }
long sys_poll(struct pollfd *p, unsigned n, int ms)   { return SC(7, p, n, ms); }
long sys_pread(int fd, void *buf, size_t n, uint64_t at) {
    return syscall6(17, fd, (long)buf, (long)n, (long)at, 0, 0);
}
long sys_socket(int domain, int type, int proto)      { return SC(41, domain, type, proto); }
long sys_connect(int s, const void *addr, unsigned len) { return SC(42, s, addr, len); }
long sys_sendto(int s, const void *buf, size_t n, const void *addr, unsigned len) {
    return syscall6(44, s, (long)buf, (long)n, 0, (long)addr, len);
}
long sys_recv(int s, void *buf, size_t n) {
    return syscall6(45, s, (long)buf, (long)n, 0, 0, 0);
}
long sys_rename(const char *from, const char *to)     { return SC(82, from, to, 0); }
long sys_mkdir(const char *p)                         { return SC(83, p, 0755, 0); }
long sys_rmdir(const char *p)                         { return SC(84, p, 0, 0); }
long sys_unlink(const char *p)                        { return SC(87, p, 0, 0); }
long sys_symlink(const char *target, const char *p)   { return SC(88, target, p, 0); }
long sys_getdents(int fd, void *buf, size_t n)        { return SC(217, fd, buf, n); }

static char **environ;

int run(const char *path, const char *const *argv) {
    return run_env(path, argv, NULL);
}

int run_env(const char *path, const char *const *argv, const char *const *extra) {
    static const char *envp[96];
    unsigned n = 0;
    long pid;
    int st = 0;

    for (unsigned i = 0; extra != NULL && extra[i] != NULL && n < 32; i++) {
        envp[n++] = extra[i];
    }
    for (unsigned i = 0; environ != NULL && environ[i] != NULL && n < 95; i++) {
        envp[n++] = environ[i];
    }
    envp[n] = NULL;
    pid = SC(57, 0, 0, 0);          /* fork */
    if (pid == 0) {
        SC(59, path, argv, envp);
        SC(231, 127, 0, 0);         /* exit_group: execve failed */
    }
    if (pid < 0) {
        return (int)pid;
    }
    if (syscall6(61, pid, (long)&st, 0, 0, 0, 0) < 0) {    /* wait4 */
        return -10;
    }
    return (st & 0x7F) != 0 ? 128 + (st & 0x7F) : (st >> 8) & 0xFF;
}

/* ---- memory ---------------------------------------------------------------- */

void *xalloc(size_t bytes) {
    size_t total = (bytes + 16 + 4095) & ~(size_t)4095;
    long at = syscall6(9, 0, (long)total, 3, 0x22, -1, 0);    /* anonymous, private */

    if (at < 0 && at > -4096) {
        fail("tuxpac: out of memory\n");
        return NULL;
    }
    *(size_t *)at = total;
    return (char *)at + 16;
}

void xfree(void *p) {
    if (p != NULL) {
        char *at = (char *)p - 16;

        SC(11, at, *(size_t *)at, 0);   /* munmap */
    }
}

/* ---- strings --------------------------------------------------------------- */

void *memcpy(void *d, const void *s, size_t n) {
    char *o = d;
    const char *i = s;

    while (n-- > 0) {
        *o++ = *i++;
    }
    return d;
}

void *memset(void *d, int c, size_t n) {
    char *o = d;

    while (n-- > 0) {
        *o++ = (char)c;
    }
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    char *o = d;
    const char *i = s;

    if (o < i) {
        return memcpy(d, s, n);
    }
    while (n-- > 0) {
        o[n] = i[n];
    }
    return d;
}

size_t strlen(const char *s) {
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *d, const char *s) {
    return memcpy(d, s, strlen(s) + 1);
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;

    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (*s == '\0') {
            return (char *)last;
        }
    }
}

char *str_word(char **text) {
    char *word = *text, *p = word;

    while (*p != '\0' && *p != ' ') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    while (*p == ' ') {
        p++;
    }
    *text = p;
    return word;
}

/* ---- output ---------------------------------------------------------------- */

int status;

static char *put_at, *put_end;     /* put_end NULL: no end to watch for */

static void put(char c) {
    if (put_end == NULL || put_at < put_end) {
        *put_at++ = c;
    }
}

static void vformat(const char *fmt, va_list args) {
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            put(*fmt);
        } else if (*++fmt == 's') {
            for (const char *s = va_arg(args, const char *); *s != '\0'; s++) {
                put(*s);
            }
        } else {
            char digits[24];
            unsigned base = *fmt == 'x' ? 16 : 10, pad = 0, i = 0;
            uint64_t v;

            if (*fmt == '0') {
                pad = (unsigned)(fmt[1] - '0');
                fmt += 2;
            }
            v = base == 16 ? va_arg(args, uint64_t) : va_arg(args, unsigned);
            do {
                digits[i++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v != 0);
            while (i < pad) {
                digits[i++] = '0';
            }
            while (i > 0) {
                put(digits[--i]);
            }
        }
    }
    *put_at = '\0';
}

void format(char *out, const char *fmt, ...) {
    va_list args;

    put_at = out;
    put_end = NULL;
    va_start(args, fmt);
    vformat(fmt, args);
    va_end(args);
}

/* A line is short; a long one - a description, a path - is cut. */
static void emit(int fd, const char *fmt, va_list args) {
    static char line[1024];

    put_at = line;
    put_end = line + sizeof line - 1;
    vformat(fmt, args);
    sys_write(fd, line, strlen(line));
}

void print(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    emit(1, fmt, args);
    va_end(args);
}

void fail(const char *fmt, ...) {
    va_list args;

    status = 1;
    va_start(args, fmt);
    emit(2, fmt, args);
    va_end(args);
}

const char *errstr(long err) {
    static char other[16];

    switch (-err) {
    case 2:  return "no such file or folder";
    case 5:  return "disk error";
    case 12: return "out of memory";
    case 17: return "already there";
    case 21: return "is a folder";
    case 28: return "no space";
    case 36: return "name too long";
    case 39: return "not empty";
    }
    format(other, "error %u", (unsigned)-err);
    return other;
}

/* ---- the start ------------------------------------------------------------- */

int main(int argc, char **argv);

/* Built as a static PIE, it is put high in the program's own region rather
   than at a fixed low address, where the firmware may still be using the memory
   before `modman takeover`. Nothing has applied its relocations, so it does
   that first, before touching anything that holds a pointer. */
extern const uint64_t _DYNAMIC[] __attribute__((visibility("hidden")));
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

static void relocate(void) {
    uint64_t base = (uint64_t)__ehdr_start, rela = 0, size = 0;

    for (const uint64_t *d = _DYNAMIC; d[0] != 0; d += 2) {
        rela = d[0] == 7 ? d[1] : rela;         /* DT_RELA */
        size = d[0] == 8 ? d[1] : size;         /* DT_RELASZ */
    }
    for (const uint64_t *r = (const uint64_t *)(base + rela); size >= 24; r += 3, size -= 24) {
        if ((uint32_t)r[1] == 8) {              /* R_X86_64_RELATIVE */
            *(uint64_t *)(base + r[0]) = base + r[2];
        }
    }
}

__attribute__((used)) static void start(long *sp) {
    relocate();

    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    environ = argv + argc + 1;
    SC(231, main(argc, argv), 0, 0);
}

__asm__(".globl _start\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call start\n");
