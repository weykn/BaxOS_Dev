#pragma once

#include <stddef.h>

/* GCC may emit calls to memset, memcpy, memmove and memcmp even under
   -ffreestanding. Only the ones in use exist; add the others here if the
   linker ever asks for them. */
void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dest, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* Splits the first word off *text: NUL-terminates it, points *text at
   whatever follows the spaces after it, and returns the word. */
char  *str_word(char **text);
