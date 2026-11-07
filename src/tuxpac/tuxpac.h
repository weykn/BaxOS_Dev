#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sys.h"

/* /usr/bin/tuxpac: Debian and Alpine packages, from the mirrors in
 * /etc/tuxlet/<prio>.<backend>.mirror.
 * A program like any other, built with the kernel and put on the disk. */

/* The packages that suit the machine: built for the 32-bit OS, i386's. */
#ifdef __i386__
#define DEB_ARCH   "i386"
#define APK_ARCH   "x86"
#define LIB_TRIPLE "i386-linux-gnu"
#else
#define DEB_ARCH   "amd64"
#define APK_ARCH   "x86_64"
#define LIB_TRIPLE "x86_64-linux-gnu"
#endif

/* Where a decoder's input comes from: a buffer that fill refills - from a
   stretch of a file, or a socket. A read past the end gives zeroes
   and sets bad, which every decoder loop checks. */
struct source {
    uint8_t  *buf;
    uint32_t  pos, len, cap;
    uint64_t  count;                /* bytes taken so far */
    bool      bad;
    uint32_t (*fill)(struct source *s);
    uint32_t  at, end;              /* a file: bytes at..end left of it */
    int       fd;                   /* the file or the socket */
    uint64_t  left;                 /* a response: body bytes still to come, or ~0 to the close */
};

uint8_t src_byte(struct source *s);
bool    src_read(struct source *s, void *out, uint32_t n);     /* out NULL skips */
void    src_file(struct source *s, int fd, uint32_t at, uint32_t end);

/* Where decoded bytes go; false stops the decoder. */
typedef bool (*sink_fn)(const uint8_t *data, uint32_t n);

/* gzip, and xz (one LZMA2 filter, as dpkg makes them). size is what the xz
   data unpacks to, if known, else 0: the window is no bigger than that. */
bool gunzip(struct source *in, sink_fn out);
bool unxz(struct source *in, uint64_t size, sink_fn out);

/* An http:// URL, redirects followed, its body left in body to read on.
   An answer other than 200 is left in http_status, unsaid. */
bool http_open(const char *url, struct source *body);
extern unsigned http_status;
void http_close(struct source *body);

