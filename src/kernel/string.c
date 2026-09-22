#include "string.h"

#include <stdint.h>

void *memset(void *dest, int value, size_t count) {
    uint8_t *d = dest;
    while (count--) {
        *d++ = (uint8_t)value;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (count--) {
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
