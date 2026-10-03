/* Sources, and the two decompressors a .deb and a package list need: gzip
 * (deflate, decoded as zlib's puff does - small rather than fast) and xz
 * (LZMA2, after the LZMA specification's reference decoder). Output goes
 * through one window, the deflate's 32 KiB or the xz dictionary - no bigger
 * than what it unpacks to - handed on each time it fills. */

#include "tuxpac.h"

/* ---- sources ------------------------------------------------------------- */

uint8_t src_byte(struct source *s) {
    if (s->pos == s->len && (s->bad || s->fill == NULL || s->fill(s) == 0)) {
        s->bad = true;
        return 0;
    }
    s->count++;
    return s->buf[s->pos++];
}

bool src_read(struct source *s, void *out, uint32_t n) {
    uint8_t *o = out;

    while (n > 0) {
        if (s->pos == s->len && (s->bad || s->fill == NULL || s->fill(s) == 0)) {
            s->bad = true;
            return false;
        }
        uint32_t k = s->len - s->pos < n ? s->len - s->pos : n;

        if (o != NULL) {
            memcpy(o, s->buf + s->pos, k);
            o += k;
        }
        s->pos += k;
        s->count += k;
        n -= k;
    }
    return true;
}

static uint32_t file_fill(struct source *s) {
    uint32_t want = s->end - s->at < s->cap ? s->end - s->at : s->cap;
    long got = s->at < s->end ? sys_pread(s->fd, s->buf, want, s->at) : 0;

    if (got <= 0) {
        return 0;
    }
    s->pos = 0;
    s->len = (uint32_t)got;
    s->at += (uint32_t)got;
    return (uint32_t)got;
}

void src_file(struct source *s, int fd, uint32_t at, uint32_t end) {
    s->fd = fd;
    s->at = at;
    s->end = end;
    s->pos = s->len = 0;
    s->count = 0;
    s->bad = false;
    s->fill = file_fill;
}

/* ---- the window ---------------------------------------------------------- */

static struct source *in;
static uint8_t  *win;
static uint32_t  wsize, wpos;
static uint64_t  total;             /* bytes out since the dictionary was reset */
static sink_fn   wsink;
static bool      wstop;

static bool window_open(uint32_t size, sink_fn out) {
    wsize = size;
    wpos = 0;
    total = 0;
    wsink = out;
    wstop = false;
    return (win = xalloc(size)) != NULL;
}

static bool window_close(bool ok) {
    if (win != NULL && ok && !wstop && wpos > 0 && !wsink(win, wpos)) {
        wstop = true;
    }
    xfree(win);
    win = NULL;
    return ok && !wstop && !in->bad;
}

static void put(uint8_t b) {
    win[wpos++] = b;
    total++;
    if (wpos == wsize) {
        wstop |= !wsink(win, wsize);
        wpos = 0;
    }
}

/* The byte d back; d >= 1. */
static uint8_t back(uint32_t d) {
    return win[wpos >= d ? wpos - d : wpos + wsize - d];
}

static bool copy(uint32_t d, uint32_t len) {
    if (d == 0 || d > total || d > wsize) {
        return false;
    }
    while (len-- > 0) {
        put(back(d));
    }
    return true;
}

/* ---- gzip ---------------------------------------------------------------- */

struct huff {
    uint16_t count[16];
    uint16_t symbol[288];
};

static uint32_t bitbuf, bitcnt;
static struct huff lencode, distcode;
static uint16_t lengths[320];

static unsigned bits(unsigned need) {
    uint32_t v = bitbuf;

    while (bitcnt < need) {
        v |= (uint32_t)src_byte(in) << bitcnt;
        bitcnt += 8;
    }
    bitbuf = v >> need;
    bitcnt -= need;
    return v & ((1u << need) - 1);
}

static int decode(const struct huff *h) {
    int code = 0, first = 0, index = 0;

    for (int len = 1; len < 16; len++) {
        code |= (int)bits(1);
        int count = h->count[len];

        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int construct(struct huff *h, const uint16_t *length, int n) {
    uint16_t offs[16];
    int left = 1;

    memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < n; s++) {
        h->count[length[s]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    for (int len = 1; len < 16; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0) {
            return left;
        }
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    }
    for (int s = 0; s < n; s++) {
        if (length[s] != 0) {
            h->symbol[offs[length[s]]++] = (uint16_t)s;
        }
    }
    return left;
}

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const uint8_t  dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool codes(void) {
    for (;;) {
        int sym = decode(&lencode);

        if (sym < 0 || in->bad || wstop) {
            return false;
        }
        if (sym < 256) {
            put((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            if ((sym -= 257) >= 29) {
                return false;
            }
            uint32_t len = lbase[sym] + bits(lext[sym]);
            int d = decode(&distcode);

            if (d < 0 || d >= 30 || !copy(dbase[d] + bits(dext[d]), len)) {
                return false;
            }
        }
    }
}

static bool stored(void) {
    bitbuf = bitcnt = 0;
    unsigned len = src_byte(in), nlen;

    len |= (unsigned)src_byte(in) << 8;
    nlen = src_byte(in);
    nlen |= (unsigned)src_byte(in) << 8;
    if (len != (~nlen & 0xFFFF)) {
        return false;
    }
    while (len-- > 0 && !in->bad) {
        put(src_byte(in));
    }
    return !in->bad;
}

static bool fixed(void) {
    int s = 0;

    for (; s < 144; s++) lengths[s] = 8;
    for (; s < 256; s++) lengths[s] = 9;
    for (; s < 280; s++) lengths[s] = 7;
    for (; s < 288; s++) lengths[s] = 8;
    construct(&lencode, lengths, 288);
    for (s = 0; s < 30; s++) lengths[s] = 5;
    construct(&distcode, lengths, 30);
    return codes();
}

static bool dynamic(void) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)bits(5) + 257, ndist = (int)bits(5) + 1, ncode = (int)bits(4) + 4, index = 0, err;

    if (nlen > 286 || ndist > 30) {
        return false;
    }
    for (int i = 0; i < 19; i++) {
        lengths[order[i]] = i < ncode ? (uint16_t)bits(3) : 0;
    }
    if (construct(&lencode, lengths, 19) != 0) {
        return false;
    }
    while (index < nlen + ndist) {
        int sym = decode(&lencode), len = 0;

        if (sym < 0 || in->bad) {
            return false;
        }
        if (sym < 16) {
            lengths[index++] = (uint16_t)sym;
            continue;
        }
        if (sym == 16) {
            if (index == 0) {
                return false;
            }
            len = lengths[index - 1];
            sym = 3 + (int)bits(2);
        } else {
            sym = sym == 17 ? 3 + (int)bits(3) : 11 + (int)bits(7);
        }
        if (index + sym > nlen + ndist) {
            return false;
        }
        while (sym-- > 0) {
            lengths[index++] = (uint16_t)len;
        }
    }
    if (lengths[256] == 0) {
        return false;
    }
    err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
        return false;
    }
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
        return false;
    }
    return codes();
}

bool gunzip(struct source *src, sink_fn out) {
    uint8_t h[10];
    bool ok;
    unsigned last;

    in = src;
    if (!src_read(in, h, 10) || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) {
        return false;
    }
    if (h[3] & 4) {
        unsigned n = src_byte(in);

        src_read(in, NULL, n | (unsigned)src_byte(in) << 8);
    }
    for (unsigned flag = 8; flag <= 16; flag <<= 1) {     /* name, comment */
        while ((h[3] & flag) && src_byte(in) != 0 && !in->bad) {
        }
    }
    if (h[3] & 2) {
        src_read(in, NULL, 2);
    }
    if (!window_open(32768, out)) {
        return false;
    }
    bitbuf = bitcnt = 0;
    do {
        last = bits(1);
        switch (bits(2)) {
        case 0:  ok = stored();  break;
        case 1:  ok = fixed();   break;
        case 2:  ok = dynamic(); break;
        default: ok = false;
        }
    } while (ok && !last);
    return window_close(ok);
}

/* ---- xz ------------------------------------------------------------------ */

struct len_coder {
    uint16_t choice, choice2, low[16][8], mid[16][8], high[256];
};

struct lzma {
    uint16_t is_match[192], is_rep[12], g0[12], g1[12], g2[12], rep0_long[192];
    uint16_t slot[4][64], pos[115], align[16];
    struct len_coder len, rep_len;
    uint16_t lit[0x300 << 4];       /* lc + lp is at most 4 in LZMA2 */
};

static uint32_t range, code, rep[4];
static unsigned lc, lp, pb, state;

static void rc_init(void) {
    src_byte(in);
    code = 0;
    for (int i = 0; i < 4; i++) {
        code = code << 8 | src_byte(in);
    }
    range = 0xFFFFFFFF;
}

static unsigned bit(uint16_t *p) {
    uint32_t bound = (range >> 11) * *p;
    unsigned b;

    if (code < bound) {
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        range = bound;
        b = 0;
    } else {
        *p = (uint16_t)(*p - (*p >> 5));
        code -= bound;
        range -= bound;
        b = 1;
    }
    if (range < 1u << 24) {
        range <<= 8;
        code = code << 8 | src_byte(in);
    }
    return b;
}

static unsigned tree(uint16_t *p, unsigned n) {
    unsigned m = 1;

    for (unsigned i = 0; i < n; i++) {
        m = (m << 1) + bit(&p[m]);
    }
    return m - (1u << n);
}

static unsigned rtree(uint16_t *p, unsigned n) {
    unsigned m = 1, s = 0;

    for (unsigned i = 0; i < n; i++) {
        unsigned b = bit(&p[m]);

        m = (m << 1) + b;
        s |= b << i;
    }
    return s;
}

static uint32_t direct(unsigned n) {
    uint32_t r = 0;

    while (n-- > 0) {
        range >>= 1;
        uint32_t b = code >= range;

        if (b) {
            code -= range;
        }
        r = r << 1 | b;
        if (range < 1u << 24) {
            range <<= 8;
            code = code << 8 | src_byte(in);
        }
    }
    return r;
}

static unsigned len_decode(struct len_coder *l, unsigned ps) {
    if (!bit(&l->choice)) {
        return tree(l->low[ps], 3);
    }
    if (!bit(&l->choice2)) {
        return 8 + tree(l->mid[ps], 3);
    }
    return 16 + tree(l->high, 8);
}

static void lzma_reset(struct lzma *p) {
    uint16_t *all = (uint16_t *)p;

    for (size_t i = 0; i < sizeof *p / 2; i++) {
        all[i] = 1024;
    }
    state = 0;
    rep[0] = rep[1] = rep[2] = rep[3] = 0;
}

/* One LZMA chunk: left bytes out. */
static bool lzma_chunk(struct lzma *p, uint32_t left) {
    while (left > 0) {
        unsigned ps = (unsigned)total & ((1u << pb) - 1), len;

        if (in->bad || wstop) {
            return false;
        }
        if (!bit(&p->is_match[state << 4 | ps])) {
            unsigned prev = total > 0 ? back(1) : 0;
            uint16_t *lit = &p->lit[0x300 * ((((unsigned)total & ((1u << lp) - 1)) << lc) +
                                             (prev >> (8 - lc)))];
            unsigned s = 1;

            if (state >= 7) {
                unsigned match = back(rep[0] + 1);

                do {
                    unsigned m = (match >> 7) & 1, b;

                    match <<= 1;
                    b = bit(&lit[((1 + m) << 8) + s]);
                    s = s << 1 | b;
                    if (m != b) {
                        break;
                    }
                } while (s < 0x100);
            }
            while (s < 0x100) {
                s = s << 1 | bit(&lit[s]);
            }
            put((uint8_t)s);
            left--;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        if (!bit(&p->is_rep[state])) {
            rep[3] = rep[2];
            rep[2] = rep[1];
            rep[1] = rep[0];
            len = len_decode(&p->len, ps);
            state = state < 7 ? 7 : 10;

            unsigned slot = tree(p->slot[len < 3 ? len : 3], 6);
            uint32_t d = slot;

            if (slot >= 4) {
                unsigned nb = (slot >> 1) - 1;

                d = (2 | (slot & 1)) << nb;
                if (slot < 14) {
                    d += rtree(&p->pos[d - slot], nb);
                } else {
                    d += direct(nb - 4) << 4;
                    d += rtree(p->align, 4);
                }
            }
            rep[0] = d;
        } else {
            if (!bit(&p->g0[state])) {
                if (!bit(&p->rep0_long[state << 4 | ps])) {
                    state = state < 7 ? 9 : 11;
                    if (!copy(rep[0] + 1, 1)) {
                        return false;
                    }
                    left--;
                    continue;
                }
            } else {
                uint32_t d;

                if (!bit(&p->g1[state])) {
                    d = rep[1];
                } else {
                    if (!bit(&p->g2[state])) {
                        d = rep[2];
                    } else {
                        d = rep[3];
                        rep[3] = rep[2];
                    }
                    rep[2] = rep[1];
                }
                rep[1] = rep[0];
                rep[0] = d;
            }
            len = len_decode(&p->rep_len, ps);
            state = state < 7 ? 8 : 11;
        }
        len += 2;
        if (len > left || !copy(rep[0] + 1, len)) {
            return false;
        }
        left -= len;
    }
    return true;
}

static bool lzma2(struct lzma *p) {
    bool props = false;

    for (;;) {
        unsigned c = src_byte(in);

        if (in->bad || wstop) {
            return false;
        }
        if (c == 0) {
            return true;
        }
        if (c == 1 || c == 2) {                 /* stored, with or without a reset */
            uint32_t n = (uint32_t)src_byte(in) << 8;

            n = (n | src_byte(in)) + 1;
            if (c == 1) {
                total = 0;
            }
            while (n-- > 0 && !in->bad) {
                put(src_byte(in));
            }
            continue;
        }
        if (c < 0x80) {
            return false;
        }
        uint32_t un = (c & 0x1F) << 16;

        un |= (uint32_t)src_byte(in) << 8;
        un = (un | src_byte(in)) + 1;
        uint32_t pk = (uint32_t)src_byte(in) << 8;

        pk = (pk | src_byte(in)) + 1;
        unsigned reset = (c >> 5) & 3;

        if (reset == 3) {
            total = 0;
        }
        if (reset >= 2) {
            unsigned b = src_byte(in);

            if (b > 224) {
                return false;
            }
            lc = b % 9;
            lp = b / 9 % 5;
            pb = b / 45;
            if (lc + lp > 4) {
                return false;
            }
            props = true;
        }
        if (!props) {
            return false;
        }
        if (reset >= 1) {
            lzma_reset(p);
        }
        uint64_t at = in->count;

        rc_init();
        if (!lzma_chunk(p, un)) {
            return false;
        }
        while (in->count < at + pk && !in->bad) {
            src_byte(in);
        }
    }
}

static uint64_t vli(const uint8_t *h, unsigned *at, unsigned end) {
    uint64_t v = 0;

    for (unsigned i = 0; i < 9 && *at < end; i++) {
        uint8_t b = h[(*at)++];

        v |= (uint64_t)(b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) {
            break;
        }
    }
    return v;
}

bool unxz(struct source *src, uint64_t size, sink_fn out) {
    static uint8_t h[1024];
    struct lzma *p = NULL;
    bool ok = true;

    in = src;
    win = NULL;
    if (!src_read(in, h, 12) || h[0] != 0xFD || h[1] != '7' || h[2] != 'z' ||
        h[3] != 'X' || h[4] != 'Z' || h[5] != 0) {
        return false;
    }
    unsigned check = h[7] & 0x0F;
    unsigned check_size = check == 0 ? 0 : 4u << ((check - 1) / 3);

    for (;;) {
        uint64_t start = in->count;
        unsigned hs = src_byte(in);

        if (in->bad || hs == 0) {               /* 0: the index, after the last block */
            ok = !in->bad;
            break;
        }
        hs = (hs + 1) * 4;
        if (!src_read(in, h + 1, hs - 1) || (h[1] & 3) != 0) {
            ok = false;                         /* more than one filter: BCJ and such */
            break;
        }
        unsigned at = 2;
        uint64_t block = 0;             /* what the block unpacks to, if it says */

        if (h[1] & 0x40) {
            vli(h, &at, hs);
        }
        if (h[1] & 0x80) {
            block = vli(h, &at, hs);
        }
        uint64_t id = vli(h, &at, hs), props = vli(h, &at, hs);

        if (id != 0x21 || props != 1 || h[at] > 40) {
            ok = false;
            break;
        }
        if (p == NULL) {
            uint64_t dict = h[at] == 40 ? 0xFFFFFFFFu : (uint64_t)(2 | (h[at] & 1)) << (h[at] / 2 + 11);

            if (size == 0) {
                size = block;           /* the first block is the biggest */
            }
            if (size != 0 && size < dict) {
                dict = size;
            }
            if (dict < 4096) {
                dict = 4096;
            }
            if ((p = xalloc(sizeof *p)) == NULL || !window_open((uint32_t)dict, out)) {
                ok = false;
                break;
            }
        }
        if (!lzma2(p)) {
            ok = false;
            break;
        }
        while ((in->count - start) % 4 != 0 && !in->bad) {
            src_byte(in);
        }
        src_read(in, NULL, check_size);
    }
    xfree(p);
    if (win == NULL) {
        return ok;
    }
    return window_close(ok);
}
