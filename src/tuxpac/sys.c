#include "sys.h"

#include <stdarg.h>

/* ---- syscalls -------------------------------------------------------------- */

#ifdef __i386__
/* i386's: int 0x80, the number in EAX and the arguments in EBX, ECX, EDX,
   ESI, EDI and EBP - which C cannot name as it can x86-64's, so it is
   assembly of its own (below). The numbers are i386's, and so are the
   calls that differ: stat64, pread64 and mmap2, recvfrom for recv. */
long syscall6(long n, long a, long b, long c, long d, long e, long f);

__asm__(".globl syscall6\n"
        ".hidden syscall6\n"
        "syscall6:\n"
        "    push %ebp\n"
        "    push %edi\n"
        "    push %esi\n"
        "    push %ebx\n"
        "    mov 20(%esp), %eax\n"
        "    mov 24(%esp), %ebx\n"
        "    mov 28(%esp), %ecx\n"
        "    mov 32(%esp), %edx\n"
        "    mov 36(%esp), %esi\n"
        "    mov 40(%esp), %edi\n"
        "    mov 44(%esp), %ebp\n"
        "    int $0x80\n"
        "    pop %ebx\n"
        "    pop %esi\n"
        "    pop %edi\n"
        "    pop %ebp\n"
        "    ret\n");

enum { NR_READ = 3, NR_WRITE = 4, NR_OPEN = 5, NR_CLOSE = 6, NR_STAT = 195, NR_FSTAT = 197,
       NR_LSTAT = 196, NR_POLL = 168, NR_PREAD = 180, NR_SOCKET = 359, NR_CONNECT = 362,
       NR_SENDTO = 369, NR_RECVFROM = 371, NR_RENAME = 38, NR_MKDIR = 39, NR_RMDIR = 40,
       NR_UNLINK = 10, NR_SYMLINK = 83, NR_GETDENTS64 = 220, NR_FORK = 2, NR_DUP2 = 63,
       NR_EXECVE = 11, NR_EXIT_GROUP = 252, NR_WAIT4 = 114, NR_MMAP = 192, NR_MUNMAP = 91 };
#else
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

enum { NR_READ = 0, NR_WRITE = 1, NR_OPEN = 2, NR_CLOSE = 3, NR_STAT = 4, NR_FSTAT = 5,
       NR_LSTAT = 6, NR_POLL = 7, NR_PREAD = 17, NR_SOCKET = 41, NR_CONNECT = 42,
       NR_SENDTO = 44, NR_RECVFROM = 45, NR_RENAME = 82, NR_MKDIR = 83, NR_RMDIR = 84,
       NR_UNLINK = 87, NR_SYMLINK = 88, NR_GETDENTS64 = 217, NR_FORK = 57, NR_DUP2 = 33,
       NR_EXECVE = 59, NR_EXIT_GROUP = 231, NR_WAIT4 = 61, NR_MMAP = 9, NR_MUNMAP = 11 };
#endif

#define SC(n, a, b, c) syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

long sys_read(int fd, void *buf, size_t n)            { return SC(NR_READ, fd, buf, n); }
long sys_write(int fd, const void *buf, size_t n)     { return SC(NR_WRITE, fd, buf, n); }
long sys_open(const char *p, int flags, int mode)     { return SC(NR_OPEN, p, flags, mode); }
long sys_close(int fd)                                { return SC(NR_CLOSE, fd, 0, 0); }
long sys_stat(const char *p, struct stat *st)         { return SC(NR_STAT, p, st, 0); }
long sys_fstat(int fd, struct stat *st)               { return SC(NR_FSTAT, fd, st, 0); }
long sys_lstat(const char *p, struct stat *st)        { return SC(NR_LSTAT, p, st, 0); }
long sys_poll(struct pollfd *p, unsigned n, int ms)   { return SC(NR_POLL, p, n, ms); }
long sys_pread(int fd, void *buf, size_t n, uint64_t at) {
#ifdef __i386__
    return syscall6(NR_PREAD, fd, (long)buf, (long)n, (long)(uint32_t)at, (long)(at >> 32), 0);
#else
    return syscall6(NR_PREAD, fd, (long)buf, (long)n, (long)at, 0, 0);
#endif
}
long sys_socket(int domain, int type, int proto)      { return SC(NR_SOCKET, domain, type, proto); }
long sys_connect(int s, const void *addr, unsigned len) { return SC(NR_CONNECT, s, addr, len); }
long sys_sendto(int s, const void *buf, size_t n, const void *addr, unsigned len) {
    return syscall6(NR_SENDTO, s, (long)buf, (long)n, 0, (long)addr, len);
}
long sys_recv(int s, void *buf, size_t n) {
    return syscall6(NR_RECVFROM, s, (long)buf, (long)n, 0, 0, 0);
}
long sys_rename(const char *from, const char *to)     { return SC(NR_RENAME, from, to, 0); }
long sys_mkdir(const char *p)                         { return SC(NR_MKDIR, p, 0755, 0); }
long sys_rmdir(const char *p)                         { return SC(NR_RMDIR, p, 0, 0); }
long sys_unlink(const char *p)                        { return SC(NR_UNLINK, p, 0, 0); }
long sys_symlink(const char *target, const char *p)   { return SC(NR_SYMLINK, target, p, 0); }
long sys_getdents(int fd, void *buf, size_t n)        { return SC(NR_GETDENTS64, fd, buf, n); }

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
    pid = SC(NR_FORK, 0, 0, 0);
    if (pid == 0) {
        /* A maintainer script has nobody to ask, and nothing it leaves
           running behind it may read the keyboard: its input is nothing. */
        long null = extra != NULL ? SC(NR_OPEN, "/dev/null", 0, 0) : -1;

        if (null >= 0) {
            SC(NR_DUP2, null, 0, 0);    /* onto stdin */
            SC(NR_CLOSE, null, 0, 0);
        }
        SC(NR_EXECVE, path, argv, envp);
        SC(NR_EXIT_GROUP, 127, 0, 0);   /* execve failed */
    }
    if (pid < 0) {
        return (int)pid;
    }
    if (syscall6(NR_WAIT4, pid, (long)&st, 0, 0, 0, 0) < 0) {
        return -10;
    }
    return (st & 0x7F) != 0 ? 128 + (st & 0x7F) : (st >> 8) & 0xFF;
}

/* ---- memory ---------------------------------------------------------------- */

void *xalloc(size_t bytes) {
    size_t total = (bytes + 16 + 4095) & ~(size_t)4095;
    long at = syscall6(NR_MMAP, 0, (long)total, 3, 0x22, -1, 0);  /* anonymous, private */

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

        SC(NR_MUNMAP, at, *(size_t *)at, 0);
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
#ifdef __i386__
/* i386's are REL: the addend in the word. */
extern const uint32_t _DYNAMIC[] __attribute__((visibility("hidden")));
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

static void relocate(void) {
    uint32_t base = (uint32_t)__ehdr_start, rel = 0, size = 0;

    for (const uint32_t *d = _DYNAMIC; d[0] != 0; d += 2) {
        rel = d[0] == 17 ? d[1] : rel;          /* DT_REL */
        size = d[0] == 18 ? d[1] : size;        /* DT_RELSZ */
    }
    for (const uint32_t *r = (const uint32_t *)(base + rel); size >= 8; r += 2, size -= 8) {
        if ((r[1] & 0xFF) == 8) {               /* R_386_RELATIVE */
            *(uint32_t *)(base + r[0]) += base;
        }
    }
}
#else
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
#endif

__attribute__((used)) static void start(long *sp) {
    relocate();

    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    environ = argv + argc + 1;
    SC(NR_EXIT_GROUP, main(argc, argv), 0, 0);
}

#ifdef __i386__
__asm__(".globl _start\n"
        "_start:\n"
        "    mov %esp, %eax\n"
        "    and $-16, %esp\n"
        "    sub $12, %esp\n"
        "    push %eax\n"
        "    call start\n");
#else
__asm__(".globl _start\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call start\n");
#endif
