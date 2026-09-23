#include "string.h"

#include <stdint.h>

/* Eight bytes at a time, with the odd bytes at either end done singly.
 *
 * These two move more memory than everything else in the kernel put together:
 * every page a program is lent is cleared here, and every library it maps is
 * read through here. A byte at a time made starting a program cost several
 * times what loading it did. */

void *memset(void *dest, int value, size_t count) {
    uint8_t *d = dest;
    uint8_t  b = (uint8_t)value;
    uint64_t word = (uint64_t)b * 0x0101010101010101ull;

    while (count > 0 && ((uintptr_t)d & 7) != 0) {
        *d++ = b;
        count--;
    }
    for (; count >= 8; count -= 8, d += 8) {
        *(uint64_t *)d = word;
    }
    while (count-- > 0) {
        *d++ = b;
    }
    return dest;
}

/* Forwards, as memcpy is: the caller promises the two do not overlap. */
void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *d = dest;
    const uint8_t *s = src;

    /* Only worth the word loop when the two can be word-aligned together;
       otherwise one side is always unaligned and the copy stays bytewise. */
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (count > 0 && ((uintptr_t)d & 7) != 0) {
            *d++ = *s++;
            count--;
        }
        for (; count >= 8; count -= 8, d += 8, s += 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
        }
    }
    while (count-- > 0) {
        *d++ = *s++;
    }
    return dest;
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
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;

    while ((*d++ = *src++) != '\0') {
    }
    return dest;
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
    const char *found = NULL;

    for (;; s++) {
        if (*s == (char)c) {
            found = s;
        }
        if (*s == '\0') {
            return (char *)found;
        }
    }
}

char *str_word(char **text) {
    char *word = *text;
    char *p = word;

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
