#include "bg.h"

#include <stdbool.h>
#include <stddef.h>

#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "fs.h"
#include "png.h"
#include "string.h"
#include "vga.h"

/* The picture is never held at its own size: rows arrive from the decoder
 * one at a time, in order, and each is written straight into the screen-sized
 * buffer wherever it belongs. A 1672x941 photograph would be 4.7 MB unpacked;
 * this way the only thing kept is what the screen can actually show. */

static uint32_t *pixels;            /* one per screen pixel, or NULL */
static unsigned  pixel_w, pixel_h;
static uint32_t  pixel_bytes;

static char     kept_path[FS_NAME_LEN];
static unsigned kept_alpha;
static bool     loading;            /* so a reload cannot call itself */

/* Where the picture sits under the screen: it is scaled to cover, keeping
   its shape, and whatever hangs over the edges is cropped. */
struct fit {
    unsigned src_w, src_h;          /* the picture */
    unsigned used_w, used_h;        /* the part of it that is shown */
    unsigned off_x, off_y;          /* where that part starts */
    unsigned next_row;              /* the next screen row still to fill */
};

static struct fit fit;

/* Worked out once a picture, rather than once a pixel: which column of the
   picture each column of the screen comes from, and what each of the 256
   brightnesses dims to. A divide per pixel is a divide too many when there
   are two million of them. */
static unsigned *columns;
static uint32_t  column_count;
static uint8_t   dimmed[256];

static struct efi_boot_services *services(void) {
    return efi_boot()->system->boot;
}

/* ---- reading the file --------------------------------------------------- */

struct source {
    struct fs_file file;
    uint32_t at;                    /* bytes handed over so far */
};

static size_t source_read(void *ctx, void *buffer, size_t len) {
    struct source *s = ctx;
    uint8_t *out = buffer;
    size_t done = 0;

    while (done < len && s->at < s->file.size) {
        uint32_t left = s->file.size - s->at;
        size_t want = len - done;

        if (want > left) {
            want = left;
        }
        /* Whole sectors from where we stand go straight into the caller's
           buffer; anything else goes through the filesystem's own. */
        if (s->at % 512 == 0 && want >= 512) {
            unsigned count = (unsigned)(want / 512);

            if (fs_read_many(s->file.start, s->at / 512, count, out + done) < 0) {
                return done;
            }
            done += (size_t)count * 512;
            s->at += (uint32_t)count * 512;
            continue;
        }
        const char *sector = fs_sector(s->file.start, s->at / 512);
        size_t chunk = 512 - s->at % 512;

        if (sector == NULL) {
            return done;
        }
        if (chunk > want) {
            chunk = want;
        }
        memcpy(out + done, sector + s->at % 512, chunk);
        done += chunk;
        s->at += (uint32_t)chunk;
    }
    return done;
}

static void *source_alloc(void *ctx, size_t len) {
    void *memory;

    (void)ctx;
    if (EFI_ERROR(services()->allocate_pool(EFI_LOADER_DATA, len, &memory))) {
        return NULL;
    }
    return memory;
}

static void source_free(void *ctx, void *memory) {
    (void)ctx;
    if (memory != NULL) {
        services()->free_pool(memory);
    }
}

/* ---- laying a row onto the screen --------------------------------------- */

/* The row of the picture a screen row is taken from. */
static unsigned source_row(unsigned y) {
    return fit.off_y + (unsigned)((uint64_t)y * fit.used_h / pixel_h);
}

/* Works out which part of the picture the screen shows: it is scaled up
   until it covers, which leaves it too wide or too tall, and the overhang is
   split evenly between the two edges. */
static void put_size(void *ctx, unsigned width, unsigned height) {
    (void)ctx;
    fit.src_w = width;
    fit.src_h = height;
    fit.next_row = 0;

    if ((uint64_t)width * pixel_h > (uint64_t)height * pixel_w) {
        fit.used_h = height;                        /* too wide: crop the sides */
        fit.used_w = (unsigned)((uint64_t)height * pixel_w / pixel_h);
    } else {
        fit.used_w = width;                         /* too tall: crop top and bottom */
        fit.used_h = (unsigned)((uint64_t)width * pixel_h / pixel_w);
    }
    if (fit.used_w == 0 || fit.used_h == 0) {
        fit.used_w = width;
        fit.used_h = height;
    }
    fit.off_x = (width - fit.used_w) / 2;
    fit.off_y = (height - fit.used_h) / 2;

    for (unsigned x = 0; x < column_count; x++) {
        columns[x] = fit.off_x + (unsigned)((uint64_t)x * fit.used_w / pixel_w);
    }
}

static void put_row(void *ctx, unsigned y, const uint8_t *row, unsigned width,
                    unsigned channels) {
    (void)ctx;
    (void)width;

    /* Rows arrive in order, so every screen row that this one is the nearest
       to is filled in now - one source row may cover several, or none. */
    while (fit.next_row < pixel_h && source_row(fit.next_row) <= y) {
        uint32_t *out = pixels + (size_t)fit.next_row * pixel_w;

        for (unsigned x = 0; x < pixel_w; x++) {
            const uint8_t *px = row + (size_t)columns[x] * channels;

            /* Dimmed towards black, which is what makes the text on top of
               it readable - and is all "transparency" can mean when there is
               nothing behind the wallpaper but the screen itself. */
            out[x] = vga_rgb(dimmed[px[0]], dimmed[px[1]], dimmed[px[2]]);
        }
        fit.next_row++;
    }
}

/* ---- loading ------------------------------------------------------------ */

void bg_clear(void) {
    vga_background(NULL);
    if (pixels != NULL) {
        services()->free_pool(pixels);
        pixels = NULL;
        pixel_bytes = 0;
    }
    if (columns != NULL) {
        services()->free_pool(columns);
        columns = NULL;
        column_count = 0;
    }
    kept_path[0] = '\0';
}

int bg_set(const char *path, unsigned alpha) {
    struct source source = { 0 };
    struct png_io io = { source_read, source_alloc, source_free, put_size, put_row,
                         &source };
    unsigned width, height;
    int err;

    if (alpha > 100) {
        alpha = 100;
    }
    err = fs_stat(path, &source.file);
    if (err < 0) {
        return err;
    }
    /* The screen's own size, which a mode change moves. */
    unsigned w = vga_pixel_width(), h = vga_pixel_height();
    uint32_t bytes = (uint32_t)w * h * sizeof(uint32_t);

    if (pixels == NULL || pixel_bytes != bytes) {
        void *memory;

        if (EFI_ERROR(services()->allocate_pool(EFI_LOADER_DATA, bytes, &memory))) {
            return BG_EMEMORY;
        }
        vga_background(NULL);       /* the old pixels are about to go */
        if (pixels != NULL) {
            services()->free_pool(pixels);
        }
        pixels = memory;
        pixel_bytes = bytes;
    }
    if (columns == NULL || column_count != w) {
        void *memory;

        if (EFI_ERROR(services()->allocate_pool(EFI_LOADER_DATA,
                                                (size_t)w * sizeof *columns, &memory))) {
            return BG_EMEMORY;
        }
        if (columns != NULL) {
            services()->free_pool(columns);
        }
        columns = memory;
        column_count = w;
    }
    pixel_w = w;
    pixel_h = h;
    kept_alpha = alpha;
    for (unsigned i = 0; i < 256; i++) {
        dimmed[i] = (uint8_t)(i * alpha / 100);
    }

    /* Two passes over the header are not worth it, so the fit is worked out
       from the size the decoder reports before the first row. */
    unsigned began = efi_seconds();
    err = png_decode(&io, &width, &height);
    dbg("bg: %u bytes in %u s\n", source.file.size, efi_seconds() - began);
    if (err < 0) {
        return err == PNG_ENOSPC ? BG_EMEMORY
             : err == PNG_EDATA ? BG_EDATA : BG_EFORMAT;
    }
    (void)width;
    (void)height;
    vga_background(pixels);
    if (path != kept_path) {
        size_t n = strlen(path);

        if (n >= sizeof kept_path) {
            n = sizeof kept_path - 1;
        }
        memcpy(kept_path, path, n);
        kept_path[n] = '\0';
    }
    return 0;
}

void bg_refresh(void) {
    if (kept_path[0] == '\0' || loading) {
        return;
    }
    loading = true;
    if (bg_set(kept_path, kept_alpha) < 0) {
        bg_clear();
    }
    loading = false;
}

void bg_check(void) {
    if (kept_path[0] != '\0' && !vga_has_background()) {
        bg_refresh();
    }
}

uint32_t bg_memory(void) {
    return pixel_bytes;
}
