#include "tcr.h"

#include <stdarg.h>

/* ---- start-up and syscalls ----------------------------------------------- */

/* Where the kernel starts a TEF program: argc, the arguments and the
 * environment are on the stack the way they are for any Linux program. */
#ifdef __i386__
__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    "_start:\n"
    "    mov (%esp), %eax\n"
    "    lea 4(%esp), %ecx\n"
    "    lea 8(%esp,%eax,4), %edx\n"
    "    and $-16, %esp\n"
    "    sub $4, %esp\n"
    "    push %edx\n"
    "    push %ecx\n"
    "    push %eax\n"
    "    call main\n"
    "    mov %eax, %ebx\n"
    "    mov $252, %eax\n"
    "    int $0x80\n"

    ".section .text.sys,\"ax\"\n"
    ".globl sys\n"
    ".hidden sys\n"
    "sys:\n"
    "    push %ebx\n"
    "    push %esi\n"
    "    mov 12(%esp), %eax\n"
    "    mov 16(%esp), %ebx\n"
    "    mov 20(%esp), %ecx\n"
    "    mov 24(%esp), %edx\n"
    "    mov 28(%esp), %esi\n"
    "    int $0x80\n"
    "    pop %esi\n"
    "    pop %ebx\n"
    "    ret\n"

    /* vfork itself, which takes nothing: the child runs first on this
       stack, so the return address waits in a register. */
    ".section .text.vfork,\"ax\"\n"
    ".globl vfork\n"
    ".hidden vfork\n"
    "vfork:\n"
    "    pop %edx\n"
    "    mov $190, %eax\n"
    "    int $0x80\n"
    "    push %edx\n"
    "    ret\n"

    /* A handler without SA_SIGINFO returns through sigreturn, past the
       signal number the frame holds. */
    ".section .text.tcr_restorer,\"ax\"\n"
    ".globl tcr_restorer\n"
    ".hidden tcr_restorer\n"
    "tcr_restorer:\n"
    "    pop %eax\n"
    "    mov $119, %eax\n"
    "    int $0x80\n"
);
#else
__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    "_start:\n"
    "    mov (%rsp), %edi\n"
    "    lea 8(%rsp), %rsi\n"
    "    lea 16(%rsp,%rdi,8), %rdx\n"
    "    call main\n"
    "    mov %eax, %edi\n"
    "    mov $231, %eax\n"
    "    syscall\n"

    ".section .text.sys,\"ax\"\n"
    ".globl sys\n"
    ".hidden sys\n"
    "sys:\n"
    "    mov %rdi, %rax\n"
    "    mov %rsi, %rdi\n"
    "    mov %rdx, %rsi\n"
    "    mov %rcx, %rdx\n"
    "    mov %r8, %r10\n"
    "    syscall\n"
    "    ret\n"

    /* The child runs first, on this same stack, so the return address is
       kept in a register rather than where the child's calls would write. */
    ".section .text.vfork,\"ax\"\n"
    ".globl vfork\n"
    ".hidden vfork\n"
    "vfork:\n"
    "    pop %rdx\n"
    "    mov $56, %eax\n"
    "    mov $0x4111, %edi\n"           /* CLONE_VM | CLONE_VFORK | SIGCHLD */
    "    xor %esi, %esi\n"
    "    syscall\n"
    "    push %rdx\n"
    "    ret\n"

    ".section .text.tcr_restorer,\"ax\"\n"
    ".globl tcr_restorer\n"
    ".hidden tcr_restorer\n"
    "tcr_restorer:\n"
    "    mov $15, %eax\n"
    "    syscall\n"
);
#endif

/* ---- strings --------------------------------------------------------------- */

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

char *strchr(const char *s, int c) {
    for (; *s != (char)c; s++) {
        if (*s == '\0') {
            return NULL;
        }
    }
    return (char *)s;
}

void *memcpy(void *to, const void *from, size_t n) {
    char *t = to;
    const char *f = from;

    while (n-- > 0) {
        *t++ = *f++;
    }
    return to;
}

void *memset(void *to, int c, size_t n) {
    char *t = to;

    while (n-- > 0) {
        *t++ = (char)c;
    }
    return to;
}

/* ---- memory ---------------------------------------------------------------- */

/* Each block has its size in front of it; a free one keeps the next free one
   where its bytes were. First fit, no merging: the core programs allocate
   little and exit soon. */
struct block {
    size_t size;
    struct block *next;             /* only while free */
};

static struct block *free_list;
static char *heap_top;

void *malloc(size_t n) {
    struct block **at, *b;
    char *top;

    n = (n + 15) & ~(size_t)15;
    for (at = &free_list; *at != NULL; at = &(*at)->next) {
        if ((*at)->size >= n) {
            b = *at;
            *at = b->next;
            return &b->next;
        }
    }
    if (heap_top == NULL) {
        heap_top = (char *)brk(0);
    }
    top = (char *)brk(heap_top + sizeof(size_t) + n);
    if (top < heap_top + sizeof(size_t) + n) {
        return NULL;
    }
    b = (struct block *)heap_top;
    b->size = n;
    heap_top = top;
    return &b->next;
}

void free(void *p) {
    struct block *b;

    if (p != NULL) {
        b = (struct block *)((char *)p - sizeof(size_t));
        b->next = free_list;
        free_list = b;
    }
}

void *realloc(void *p, size_t n) {
    size_t had = p != NULL ? ((size_t *)p)[-1] : 0;
    void *q;

    if (had >= n) {
        return p;
    }
    if ((q = malloc(n)) != NULL && p != NULL) {
        memcpy(q, p, had);
        free(p);
    }
    return q;
}

/* ---- output and paths -------------------------------------------------------- */

void out(int fd, ...) {
    va_list list;
    const char *s;

    va_start(list, fd);
    while ((s = va_arg(list, const char *)) != NULL) {
        write(fd, s, strlen(s));
    }
    va_end(list);
}

int puts(const char *s) {
    out(1, s, "\n", NULL);
    return 0;
}

char *utoa(unsigned long n, char *buf) {
    char *p = buf + 20;

    *p = '\0';
    do {
        *--p = (char)('0' + n % 10);
        n /= 10;
    } while (n != 0);
    return memcpy(buf, p, (size_t)(buf + 21 - p));
}

const char *base_of(const char *path) {
    const char *base = path;

    for (; *path != '\0'; path++) {
        if (*path == '/' && path[1] != '\0') {
            base = path + 1;
        }
    }
    return base;
}

bool is_folder(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR;
}

const char *dest_of(const char *from, const char *to, char *buf, size_t max) {
    const char *base = base_of(from);
    size_t n = strlen(to), b = strlen(base);

    if (!is_folder(to) || n + b + 2 > max) {
        return to;
    }
    memcpy(buf, to, n);
    if (n == 0 || buf[n - 1] != '/') {
        buf[n++] = '/';
    }
    memcpy(buf + n, base, b + 1);
    return buf;
}
