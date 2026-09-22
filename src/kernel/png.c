#include "png.h"

#include <stdbool.h>

/* PNG is a chunked container around one zlib stream: the stream inflates to
 * the rows of the picture, each with a leading byte saying how it was
 * filtered - a guess at the pixel, from its neighbours, which the compressor
 * subtracted to leave less for it to code. Undoing the guess needs the row
 * above and the pixels to the left, and nothing else, which is why a decoder
 * can work a row at a time.
 *
 * The inflate here is the plain, slow, small one: codes are read a bit at a
 * time and looked up by counting through the canonical table, rather than
 * through a table of prefixes. A wallpaper is decoded once at startup, so the
 * kilobytes a faster decoder would cost buy nothing. */

#define WINDOW 32768            /* how far back a match may reach */
/* How much is asked of the file at a time. Reading through the firmware
   costs the same for a big read as a small one - and on a machine booting
   off a USB stick that cost is milliseconds - so it is asked rarely. */
#define IN_BUF 65536

#define MAX_WIDTH 8192          /* no wider picture is worth the memory */

/* ---- reading the file --------------------------------------------------- */

/* A canonical Huffman table as how many codes are of each length, and the
   symbols in the order the codes run. */
struct huffman {
    int16_t count[16];
    int16_t symbol[288];
};

/* Everything the decoder works in. It is allocated rather than made on the
   stack: the kernel's stack is a few kilobytes and the firmware's interrupt
   handlers share it, and this is far too much to put there. */
struct png {
    const struct png_io *io;

    struct huffman literal, distance, code;
    uint8_t  lengths[288 + 32];

    uint8_t *in;
    size_t   in_have, in_pos;
    bool     eof;

    uint32_t chunk_left;        /* bytes of the current IDAT still to come */
    bool     stream_end;        /* IEND, or a chunk that ends the picture */

    uint32_t bits;              /* bits read but not yet used, low first */
    unsigned bit_count;

    uint8_t *window;
    uint32_t window_at;

    uint8_t *cur, *prev;        /* the row being built, and the one above */
    uint32_t row_bytes, row_at;
    unsigned width, height, channels, y;
};

static int file_byte(struct png *z) {
    if (z->in_pos == z->in_have) {
        if (z->eof) {
            return -1;
        }
        z->in_have = z->io->read(z->io->ctx, z->in, IN_BUF);
        z->in_pos = 0;
        if (z->in_have == 0) {
            z->eof = true;
            return -1;
        }
    }
    return z->in[z->in_pos++];
}

/* Four bytes, most significant first, as every number in a PNG is. */
static int64_t file_u32(struct png *z) {
    uint32_t value = 0;

    for (unsigned i = 0; i < 4; i++) {
        int byte = file_byte(z);

        if (byte < 0) {
            return -1;
        }
        value = value << 8 | (uint32_t)byte;
    }
    return value;
}

static bool skip(struct png *z, uint32_t count) {
    while (count-- > 0) {
        if (file_byte(z) < 0) {
            return false;
        }
    }
    return true;
}

/* The next byte of the zlib stream, which lives in the IDAT chunks: they run
   one into the next, and anything else in between is not ours to read.
   Crossing into another chunk, or into another bufferful, is rare and left
   to this; the byte after byte after byte is the inline part below. */
static int next_chunk_byte(struct png *z) {
    while (z->chunk_left == 0) {
        int64_t length;
        uint8_t type[4];

        if (z->stream_end) {
            return -1;
        }
        length = file_u32(z);
        for (unsigned i = 0; i < 4; i++) {
            int byte = file_byte(z);

            if (length < 0 || byte < 0) {
                return -1;
            }
            type[i] = (uint8_t)byte;
        }
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            z->chunk_left = (uint32_t)length;
            if (z->chunk_left == 0 && !skip(z, 4)) {
                return -1;          /* an empty one: its checksum, then on */
            }
            continue;
        }
        if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            z->stream_end = true;
            return -1;
        }
        if (!skip(z, (uint32_t)length + 4)) {
            return -1;
        }
    }
    int byte = file_byte(z);

    if (byte < 0) {
        return -1;
    }
    if (--z->chunk_left == 0 && !skip(z, 4)) {
        return -1;                  /* the chunk's checksum, which is not read */
    }
    return byte;
}

static inline int data_byte(struct png *z) {
    /* Not the last byte of the chunk, which has a checksum behind it. */
    if (z->chunk_left > 1 && z->in_pos < z->in_have) {
        z->chunk_left--;
        return z->in[z->in_pos++];
    }
    return next_chunk_byte(z);
}

/* ---- the compressed stream ---------------------------------------------- */

/* count bits, least significant first, as deflate writes them. */
static int64_t take_bits(struct png *z, unsigned count) {
    while (z->bit_count < count) {
        int byte = data_byte(z);

        if (byte < 0) {
            return -1;
        }
        z->bits |= (uint32_t)byte << z->bit_count;
        z->bit_count += 8;
    }
    uint32_t value = z->bits & ((1u << count) - 1);

    z->bits >>= count;
    z->bit_count -= count;
    return value;
}

static int build(struct huffman *h, const uint8_t *lengths, unsigned n) {
    int16_t offset[16];

    for (unsigned i = 0; i < 16; i++) {
        h->count[i] = 0;
    }
    for (unsigned i = 0; i < n; i++) {
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;
    offset[1] = 0;
    for (unsigned len = 1; len < 15; len++) {
        offset[len + 1] = (int16_t)(offset[len] + h->count[len]);
    }
    for (unsigned i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            h->symbol[offset[lengths[i]]++] = (int16_t)i;
        }
    }
    return 0;
}

/* The next single bit, which is what decoding a symbol is made of and so
   the busiest thing here. */
static inline int one_bit(struct png *z) {
    if (z->bit_count == 0) {
        int byte = data_byte(z);

        if (byte < 0) {
            return -1;
        }
        z->bits = (uint32_t)byte;
        z->bit_count = 8;
    }
    int bit = (int)(z->bits & 1);

    z->bits >>= 1;
    z->bit_count--;
    return bit;
}

/* Reads one symbol: a code is decoded by walking the lengths, since the
   codes of each length run in order and each length's first code follows on
   from the last. */
static int decode(struct png *z, const struct huffman *h) {
    int code = 0, first = 0, index = 0;

    for (unsigned len = 1; len < 16; len++) {
        int bit = one_bit(z);

        if (bit < 0) {
            return -1;
        }
        code |= bit;
        int count = h->count[len];

        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* ---- the rows ----------------------------------------------------------- */

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) {
        return (uint8_t)a;
    }
    return pb <= pc ? (uint8_t)b : (uint8_t)c;
}

/* Undoes the row's filter in place and hands it over. */
static void finish_row(struct png *z) {
    unsigned bpp = z->channels;
    uint8_t *row = z->cur + 1;
    uint32_t n = z->row_bytes - 1;

    switch (z->cur[0]) {
    case 1:                         /* Sub: the pixel to the left */
        for (uint32_t i = bpp; i < n; i++) {
            row[i] = (uint8_t)(row[i] + row[i - bpp]);
        }
        break;
    case 2:                         /* Up: the pixel above */
        for (uint32_t i = 0; i < n; i++) {
            row[i] = (uint8_t)(row[i] + z->prev[1 + i]);
        }
        break;
    case 3:                         /* Average: the two of them */
        for (uint32_t i = 0; i < n; i++) {
            unsigned left = i >= bpp ? row[i - bpp] : 0;

            row[i] = (uint8_t)(row[i] + (left + z->prev[1 + i]) / 2);
        }
        break;
    case 4:                         /* Paeth: whichever of three is nearest */
        for (uint32_t i = 0; i < n; i++) {
            unsigned left = i >= bpp ? row[i - bpp] : 0;
            unsigned above_left = i >= bpp ? z->prev[1 + i - bpp] : 0;

            row[i] = (uint8_t)(row[i] + paeth((int)left, z->prev[1 + i],
                                              (int)above_left));
        }
        break;
    default:
        break;                      /* 0, or something odd taken as none */
    }
    if (z->y < z->height) {
        z->io->row(z->io->ctx, z->y, row, z->width, z->channels);
    }
    z->y++;

    uint8_t *was = z->prev;         /* this row is the next one's row above */

    z->prev = z->cur;
    z->cur = was;
    z->row_at = 0;
}

/* One byte of the picture: into the window, where a later match may reach
   back for it, and into the row being built. */
static void emit(struct png *z, uint8_t byte) {
    z->window[z->window_at++ & (WINDOW - 1)] = byte;
    if (z->row_at < z->row_bytes) {
        z->cur[z->row_at++] = byte;
    }
    if (z->row_at == z->row_bytes) {
        finish_row(z);
    }
}

/* ---- inflate ------------------------------------------------------------ */

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0,
};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13,
};

/* The literal and distance codes a block says to use, or the fixed pair every
   deflate stream may fall back on. */
static int codes(struct png *z, const struct huffman *literal,
                 const struct huffman *distance) {
    for (;;) {
        int symbol = decode(z, literal);

        if (symbol < 0) {
            return PNG_EDATA;
        }
        if (symbol < 256) {
            emit(z, (uint8_t)symbol);
            continue;
        }
        if (symbol == 256) {
            return 0;               /* the end of the block */
        }
        symbol -= 257;
        if (symbol >= 29) {
            return PNG_EDATA;
        }
        int64_t extra = take_bits(z, length_extra[symbol]);
        if (extra < 0) {
            return PNG_EDATA;
        }
        unsigned length = length_base[symbol] + (unsigned)extra;

        symbol = decode(z, distance);
        if (symbol < 0 || symbol >= 30) {
            return PNG_EDATA;
        }
        extra = take_bits(z, distance_extra[symbol]);
        if (extra < 0) {
            return PNG_EDATA;
        }
        unsigned back = distance_base[symbol] + (unsigned)extra;

        if (back > WINDOW) {
            return PNG_EDATA;
        }
        /* A match may overlap what it is writing - that is how a run of the
           same bytes is coded - so it is copied a byte at a time. */
        for (unsigned i = 0; i < length; i++) {
            emit(z, z->window[(z->window_at - back) & (WINDOW - 1)]);
        }
    }
}

static void fixed_tables(struct png *z) {
    struct huffman *literal = &z->literal, *distance = &z->distance;
    uint8_t *lengths = z->lengths;

    for (unsigned i = 0; i < 144; i++) {
        lengths[i] = 8;
    }
    for (unsigned i = 144; i < 256; i++) {
        lengths[i] = 9;
    }
    for (unsigned i = 256; i < 280; i++) {
        lengths[i] = 7;
    }
    for (unsigned i = 280; i < 288; i++) {
        lengths[i] = 8;
    }
    build(literal, lengths, 288);
    for (unsigned i = 0; i < 30; i++) {
        lengths[i] = 5;
    }
    build(distance, lengths, 30);
}

/* The tables a block carries with it, themselves Huffman coded. */
static int dynamic_tables(struct png *z) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    };
    uint8_t *lengths = z->lengths;
    int64_t literals = take_bits(z, 5);
    int64_t distances = take_bits(z, 5);
    int64_t code_count = take_bits(z, 4);

    if (literals < 0 || distances < 0 || code_count < 0) {
        return PNG_EDATA;
    }
    for (unsigned i = 0; i < sizeof z->lengths; i++) {
        lengths[i] = 0;
    }
    literals += 257;
    distances += 1;
    code_count += 4;
    if (literals > 286 || distances > 30) {
        return PNG_EDATA;
    }
    for (int64_t i = 0; i < code_count; i++) {
        int64_t length = take_bits(z, 3);

        if (length < 0) {
            return PNG_EDATA;
        }
        lengths[order[i]] = (uint8_t)length;
    }
    for (unsigned i = (unsigned)code_count; i < 19; i++) {
        lengths[order[i]] = 0;
    }
    build(&z->code, lengths, 19);

    /* The two tables' lengths, one list, with short runs coded as repeats. */
    for (int64_t i = 0; i < literals + distances;) {
        int symbol = decode(z, &z->code);
        int64_t extra;
        unsigned repeat;
        uint8_t value;

        if (symbol < 0) {
            return PNG_EDATA;
        }
        if (symbol < 16) {
            lengths[i++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 16) {
            if (i == 0) {
                return PNG_EDATA;
            }
            value = lengths[i - 1];
            extra = take_bits(z, 2);
            repeat = 3;
        } else if (symbol == 17) {
            value = 0;
            extra = take_bits(z, 3);
            repeat = 3;
        } else {
            value = 0;
            extra = take_bits(z, 7);
            repeat = 11;
        }
        if (extra < 0) {
            return PNG_EDATA;
        }
        repeat += (unsigned)extra;
        if (i + repeat > literals + distances) {
            return PNG_EDATA;
        }
        while (repeat-- > 0) {
            lengths[i++] = value;
        }
    }
    build(&z->literal, lengths, (unsigned)literals);
    build(&z->distance, lengths + literals, (unsigned)distances);
    return 0;
}

static int inflate(struct png *z) {
    /* The zlib header: a compression method and a window size, neither of
       which a PNG is allowed to vary. */
    if (data_byte(z) < 0 || data_byte(z) < 0) {
        return PNG_EDATA;
    }
    for (;;) {
        int64_t last = take_bits(z, 1);
        int64_t type = take_bits(z, 2);
        int err;

        if (last < 0 || type < 0) {
            return PNG_EDATA;
        }
        if (type == 0) {
            /* Stored: to the next byte boundary, then a length and its
               complement, then the bytes themselves. */
            int64_t length;

            z->bits = 0;
            z->bit_count = 0;
            length = data_byte(z);
            int high = data_byte(z);
            if (length < 0 || high < 0) {
                return PNG_EDATA;
            }
            length |= (int64_t)high << 8;
            if (data_byte(z) < 0 || data_byte(z) < 0) {
                return PNG_EDATA;   /* its complement, which is not checked */
            }
            while (length-- > 0) {
                int byte = data_byte(z);

                if (byte < 0) {
                    return PNG_EDATA;
                }
                emit(z, (uint8_t)byte);
            }
        } else if (type == 1 || type == 2) {
            if (type == 1) {
                fixed_tables(z);
            } else if ((err = dynamic_tables(z)) < 0) {
                return err;
            }
            if ((err = codes(z, &z->literal, &z->distance)) < 0) {
                return err;
            }
        } else {
            return PNG_EDATA;
        }
        if (last == 1 || z->y >= z->height) {
            return 0;
        }
    }
}

/* ---- the file ----------------------------------------------------------- */

int png_decode(const struct png_io *io, unsigned *width, unsigned *height) {
    static const uint8_t signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    struct png *z = io->alloc(io->ctx, sizeof *z);
    uint8_t type[4], header[13];
    int64_t length;
    int err = PNG_EINVAL;

    if (z == NULL) {
        return PNG_ENOSPC;
    }
    for (size_t i = 0; i < sizeof *z; i++) {
        ((uint8_t *)z)[i] = 0;
    }
    z->io = io;
    z->in = io->alloc(io->ctx, IN_BUF);
    if (z->in == NULL) {
        err = PNG_ENOSPC;
        goto done;
    }

    for (unsigned i = 0; i < 8; i++) {
        if (file_byte(z) != signature[i]) {
            goto done;
        }
    }

    /* The header chunk comes first, and says everything about the picture. */
    length = file_u32(z);
    for (unsigned i = 0; i < 4; i++) {
        int byte = file_byte(z);

        if (byte < 0) {
            goto done;
        }
        type[i] = (uint8_t)byte;
    }
    if (length != 13 || type[0] != 'I' || type[1] != 'H' || type[2] != 'D' ||
        type[3] != 'R') {
        goto done;
    }
    for (unsigned i = 0; i < 13; i++) {
        int byte = file_byte(z);

        if (byte < 0) {
            goto done;
        }
        header[i] = (uint8_t)byte;
    }
    if (!skip(z, 4)) {              /* the header's checksum */
        goto done;
    }
    z->width = (unsigned)header[0] << 24 | (unsigned)header[1] << 16 |
               (unsigned)header[2] << 8 | header[3];
    z->height = (unsigned)header[4] << 24 | (unsigned)header[5] << 16 |
                (unsigned)header[6] << 8 | header[7];
    z->channels = header[9] == 2 ? 3 : header[9] == 6 ? 4 : 0;

    /* Eight bits a channel, colour, and stored row after row. Anything else -
       a palette, sixteen bits, interlacing - is a picture this was not
       written for. */
    if (z->width == 0 || z->height == 0 || z->width > MAX_WIDTH ||
        header[8] != 8 || z->channels == 0 || header[10] != 0 ||
        header[11] != 0 || header[12] != 0) {
        goto done;
    }
    *width = z->width;
    *height = z->height;
    io->size(io->ctx, z->width, z->height);

    z->row_bytes = (uint32_t)z->width * z->channels + 1;
    z->window = io->alloc(io->ctx, WINDOW);
    z->cur = io->alloc(io->ctx, z->row_bytes);
    z->prev = io->alloc(io->ctx, z->row_bytes);
    if (z->window == NULL || z->cur == NULL || z->prev == NULL) {
        err = PNG_ENOSPC;
    } else {
        /* The row above the first one is taken to be black, as the format
           says, so the first row's filter has something to work from. */
        for (uint32_t i = 0; i < z->row_bytes; i++) {
            z->prev[i] = 0;
        }
        err = inflate(z);
        if (err == 0 && z->y == 0) {
            err = PNG_EDATA;
        }
    }
    io->free(io->ctx, z->window);
    io->free(io->ctx, z->cur);
    io->free(io->ctx, z->prev);
done:
    io->free(io->ctx, z->in);
    io->free(io->ctx, z);
    return err;
}
