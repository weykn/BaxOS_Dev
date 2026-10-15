#include "tef.h"

#include <stdarg.h>

/* Where the kernel starts a TEF64 program: argc, the arguments and the
 * environment are on the stack the way they are for any Linux program. */
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
    ".section .text.tef_vfork,\"ax\"\n"
    ".globl tef_vfork\n"
    ".hidden tef_vfork\n"
    "tef_vfork:\n"
    "    pop %rdx\n"
    "    mov $56, %eax\n"
    "    mov $0x4111, %edi\n"           /* CLONE_VM | CLONE_VFORK | SIGCHLD */
    "    xor %esi, %esi\n"
    "    syscall\n"
    "    push %rdx\n"
    "    ret\n"

    ".section .text.tef_restorer,\"ax\"\n"
    ".globl tef_restorer\n"
    ".hidden tef_restorer\n"
    "tef_restorer:\n"
    "    mov $15, %eax\n"
    "    syscall\n"
);

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

void out(int fd, ...) {
    va_list list;
    const char *s;

    va_start(list, fd);
    while ((s = va_arg(list, const char *)) != NULL) {
        write(fd, s, strlen(s));
    }
    va_end(list);
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
