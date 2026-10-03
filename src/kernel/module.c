#include "module.h"

#include "debug.h"
#include "driver.h"
#include "efi_kernel.h"
#include "fs.h"
#include "mem.h"
#include "net.h"
#include "pci.h"
#include "proc.h"
#include "shell.h"
#include "string.h"
#include "syscall.h"
#include "thread.h"
#include "vga.h"

#define MODULES  8
#define NAME_LEN 32
#define EXT      ".kmod"
#define ENODEV   19

/* ---- what the kernel offers ---------------------------------------------- */

const struct net_ops         *net;
const struct keyboard_driver *keyboard_driver;
const struct tracer          *tracer;

const struct net_card        *net_card;

void net_register(const struct net_ops *ops) {
    net = ops;
}

int net_card_register(const struct net_card *card) {
    if (card != NULL && net_card != NULL) {
        return -16;                 /* EBUSY: one card at a time */
    }
    net_card = card;
    if (net != NULL) {
        net->card_changed();
    }
    return 0;
}

void keyboard_register(const struct keyboard_driver *k) {
    keyboard_driver = k;
}

const struct disk_cache *disk_cache;

/* What it held of the disk is still the disk's, so it simply stops being
   asked. */
void disk_cache_register(const struct disk_cache *c) {
    disk_cache = c;
}

void tracer_register(const struct tracer *t) {
    tracer = t;
}

#define X(f) { #f, (const void *)&f }

static const struct {
    const char *name;
    const void *at;
} exports[] = {
    X(memcpy), X(memset), X(memmove), X(strlen), X(strcmp),
    X(str_word),
    X(kprintf), X(ksprintf), X(vga_putc), X(vga_puts), X(vga_set_color),
    X(efi_boot), X(efi_seconds), X(efi_uptime_ms), X(efi_uptime_us),
    X(mem_pages), X(mem_pages_below), X(mem_pages_free), X(mem_alloc), X(mem_free),
    X(mem_ours),
    X(fs_stat), X(fs_sector), X(fs_read_many), X(fs_write), X(fs_error),
    X(pci_read), X(pci_write), X(pci_find), X(pci_find_class),
    X(net_register), X(net_card_register), X(disk_register), X(keyboard_register),
    X(tracer_register), X(disk_cache_register), X(fs_runs),
    X(mem_free_kib), X(usage_bar), X(net_card), X(disk_driver),
    X(fs_mkdir), X(fs_write_at), X(strcpy),
    X(proc_add), X(proc_remove),
    X(handle_of), X(give_handle), X(user_range), X(syscall_args), X(interrupt_check),
    X(wait_began), X(wait_ended), X(thread_yield),
#ifdef DEBUG
    X(dbg),
#endif
};

/* ---- ELF, as much of it as a shared object needs ----------------------- */

struct elf_header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_program {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct elf_sym {
    uint32_t name;
    uint8_t  info, other;
    uint16_t shndx;
    uint64_t value, size;
};

struct elf_rela {
    uint64_t offset, info;
    int64_t  addend;
};

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define DT_NULL     0
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_JMPREL   23
#define R_NONE      0
#define R_64        1
#define R_GLOB_DAT  6
#define R_JUMP_SLOT 7
#define R_RELATIVE  8

static struct module {
    char     name[NAME_LEN];        /* empty in a free slot */
    uint64_t base;
    uint32_t span;                  /* bytes of its image from base */
    const struct elf_sym *syms;
    const char *strs;
    uint32_t nsyms;
    uint32_t needs;                 /* the slots it took symbols from */
} mods[MODULES];

static char why[80];                /* what the last failure was */

static int slot_of(const char *name) {
    for (int i = 0; i < MODULES; i++) {
        if (mods[i].name[0] != '\0' && strcmp(mods[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* A symbol m defines, or 0. Only exported ones are in the table at all. */
static uint64_t defined(const struct module *m, const char *name) {
    for (uint32_t i = 1; i < m->nsyms; i++) {
        const struct elf_sym *s = &m->syms[i];

        if (s->shndx != 0 && strcmp(m->strs + s->name, name) == 0) {
            return m->base + s->value;
        }
    }
    return 0;
}

static uint64_t resolve(struct module *m, const struct elf_sym *s) {
    const char *name = m->strs + s->name;
    uint64_t at;

    if (s->shndx != 0) {
        return m->base + s->value;
    }
    for (unsigned i = 0; i < sizeof exports / sizeof exports[0]; i++) {
        if (strcmp(exports[i].name, name) == 0) {
            return (uint64_t)exports[i].at;
        }
    }
    for (int i = 0; i < MODULES; i++) {
        if (&mods[i] != m && mods[i].name[0] != '\0' && (at = defined(&mods[i], name)) != 0) {
            m->needs |= 1u << i;
            return at;
        }
    }
    if ((s->info >> 4) != 2) {      /* a weak one may be missing */
        ksprintf(why, "unknown symbol %s", name);
    }
    return 0;
}

static bool relocate(struct module *m, const struct elf_rela *r, uint64_t bytes) {
    for (; bytes >= sizeof *r; r++, bytes -= sizeof *r) {
        uint64_t *at = (uint64_t *)(m->base + r->offset);
        uint32_t type = (uint32_t)r->info;
        const struct elf_sym *s = &m->syms[r->info >> 32];
        uint64_t value;

        if (r->offset + 8 > m->span) {
            return false;
        }
        switch (type) {
        case R_NONE:
            break;
        case R_RELATIVE:
            *at = m->base + (uint64_t)r->addend;
            break;
        case R_64:
        case R_GLOB_DAT:
        case R_JUMP_SLOT:
            value = resolve(m, s);
            if (value == 0 && why[0] != '\0') {
                return false;
            }
            *at = value + (type == R_64 ? (uint64_t)r->addend : 0);
            break;
        default:
            ksprintf(why, "relocation type %u", type);
            return false;
        }
    }
    return true;
}

static void *read_file(const char *path, uint32_t *size) {
    struct fs_file file;
    unsigned sectors;
    void *data;

    if (fs_stat(path, &file) < 0 || file.size == 0) {
        return NULL;
    }
    sectors = (file.size + FS_SECTOR - 1) / FS_SECTOR;
    if ((data = mem_alloc(sectors * FS_SECTOR)) == NULL) {
        return NULL;
    }
    if (fs_read_many(file.start, 0, sectors, data) < 0) {
        mem_free(data);
        return NULL;
    }
    *size = file.size;
    return data;
}

/* ---- where images go ---------------------------------------------------------
 *
 * Memory comes in pages, and most modules are a few kilobytes: given pages
 * of its own, each would leave most of its last one empty. So a module's
 * last page, where it only partly fills it, is offered to the next small
 * one, and a page goes back only once every module in it has. */

#define TAILS 8

static struct tail {
    uint64_t page;                  /* 0 in a free slot */
    uint16_t used;                  /* bytes from the start of it taken */
    uint16_t users;                 /* modules in it */
} tails[TAILS];

static uint32_t held;               /* pages all the images take */

static struct tail *tail_of(uint64_t page) {
    for (unsigned i = 0; i < TAILS; i++) {
        if (tails[i].page == page && page != 0) {
            return &tails[i];
        }
    }
    return NULL;
}

static uint64_t image_alloc(uint32_t span) {
    uint32_t need = (span + 15) & ~15u, pages = (span + 4095) / 4096;
    uint64_t base;

    for (unsigned i = 0; i < TAILS; i++) {
        if (tails[i].page != 0 && tails[i].used + need <= 4096) {
            tails[i].users++;
            tails[i].used = (uint16_t)(tails[i].used + need);
            return tails[i].page + tails[i].used - need;
        }
    }
    if ((base = mem_pages(pages)) == 0) {
        return 0;
    }
    held += pages;
    if (need % 4096 != 0) {
        struct tail *t = NULL;

        for (unsigned i = 0; i < TAILS && t == NULL; i++) {
            t = tails[i].page == 0 ? &tails[i] : NULL;
        }
        if (t != NULL) {
            *t = (struct tail){ base + (pages - 1) * 4096, (uint16_t)(need % 4096), 1 };
        }
    }
    return base;
}

static void image_free(uint64_t base, uint32_t span) {
    for (uint64_t page = base & ~4095ull; page < base + span; page += 4096) {
        struct tail *t = tail_of(page);

        if (t != NULL && --t->users > 0) {
            continue;               /* still someone else's too */
        }
        if (t != NULL) {
            *t = (struct tail){ 0 };
        }
        mem_pages_free(page, 1);
        held--;
    }
}

/* Puts the image in place and links it; false with why set if it cannot. */
static bool place(struct module *m, const uint8_t *file, uint32_t size) {
    const struct elf_header *h = (const struct elf_header *)file;
    const struct elf_program *p = (const struct elf_program *)(file + h->phoff);
    const uint64_t *dyn = NULL;
    uint64_t span = 0, tag[24] = { 0 };

    if (size < sizeof *h || h->ident[0] != 0x7F || h->ident[1] != 'E' || h->type != 3 ||
        h->machine != 0x3E || h->phoff + (uint64_t)h->phnum * sizeof *p > size) {
        strcpy(why, "not a module");
        return false;
    }
    for (unsigned i = 0; i < h->phnum; i++) {
        if (p[i].type == PT_LOAD) {
            if (p[i].offset + p[i].filesz > size || p[i].filesz > p[i].memsz) {
                strcpy(why, "not a module");
                return false;
            }
            if (p[i].vaddr + p[i].memsz > span) {
                span = p[i].vaddr + p[i].memsz;
            }
        }
    }
    m->span = (uint32_t)span;
    if ((m->base = image_alloc(m->span)) == 0) {
        strcpy(why, "out of memory");
        return false;
    }
    memset((void *)m->base, 0, span);
    for (unsigned i = 0; i < h->phnum; i++) {
        if (p[i].type == PT_LOAD) {
            memcpy((void *)(m->base + p[i].vaddr), file + p[i].offset, p[i].filesz);
        } else if (p[i].type == PT_DYNAMIC) {
            dyn = (const uint64_t *)(m->base + p[i].vaddr);
        }
    }
    for (; dyn != NULL && dyn[0] != DT_NULL; dyn += 2) {
        if (dyn[0] < 24) {
            tag[dyn[0]] = dyn[1];
        }
    }
    if (tag[DT_SYMTAB] == 0 || tag[DT_STRTAB] == 0 || tag[DT_HASH] == 0) {
        strcpy(why, "not a module");
        return false;
    }
    m->syms = (const struct elf_sym *)(m->base + tag[DT_SYMTAB]);
    m->strs = (const char *)(m->base + tag[DT_STRTAB]);
    m->nsyms = ((const uint32_t *)(m->base + tag[DT_HASH]))[1];
    why[0] = '\0';
    if (!relocate(m, (const struct elf_rela *)(m->base + tag[DT_RELA]), tag[DT_RELASZ]) ||
        !relocate(m, (const struct elf_rela *)(m->base + tag[DT_JMPREL]), tag[DT_PLTRELSZ])) {
        if (why[0] == '\0') {
            strcpy(why, "not a module");
        }
        return false;
    }
    return true;
}

static void release(struct module *m) {
    if (m->base != 0) {
        image_free(m->base, m->span);
    }
    *m = (struct module){ 0 };
}

/* Where module name is: <dir>/<name>.kmod for the first folder of MODPATH
   that has it, as /etc/tuxlet/env exports it. */
static bool module_file(const char *name, char *out, size_t max) {
    const char *dirs = shell_env("MODPATH");
    struct fs_file file;

    while (dirs != NULL && *dirs != '\0') {
        const char *end = strchr(dirs, ':');
        size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);

        if (n > 0 && n + strlen(name) + sizeof "/" EXT < max) {
            memcpy(out, dirs, n);
            ksprintf(out + n, "/%s" EXT, name);
            if (fs_stat(out, &file) == 0) {
                return true;
            }
        }
        dirs = end != NULL ? end + 1 : dirs + n;
    }
    return false;
}

/* Loads the one module; its init's answer, or -1 with why set. */
static int load_one(const char *name) {
    char path[FS_NAME_LEN];
    struct module *m = NULL;
    uint32_t size;
    uint8_t *file;

    if (strlen(name) >= NAME_LEN) {
        strcpy(why, "name too long");
        return -1;
    }
    for (int i = 0; i < MODULES && m == NULL; i++) {
        m = mods[i].name[0] == '\0' ? &mods[i] : NULL;
    }
    if (m == NULL) {
        strcpy(why, "too many modules");
        return -1;
    }
    if (!module_file(name, path, sizeof path) || (file = read_file(path, &size)) == NULL) {
        strcpy(why, "no such module");
        return -1;
    }
    strcpy(m->name, name);
    bool placed = place(m, file, size);

    mem_free(file);
    uint64_t init = placed ? defined(m, "module_init") : 0;

    if (placed && init == 0) {
        strcpy(why, "no module_init");
    }
    if (init == 0) {
        release(m);
        return -1;
    }
    int err = ((int (*)(void))init)();

    if (err < 0) {
        strcpy(why, err == -ENODEV ? "no device" : "failed to start");
        release(m);
        return err;
    }
    dbg("module: %s at %x, %u bytes\n", name, m->base, m->span);
    return 0;
}

static bool load(const char *name) {
    return slot_of(name) >= 0 || load_one(name) == 0;
}

/* Unloads a module and, first, everything that uses it. A module still in
   use - the disk driver once the firmware is gone - says so from its
   module_exit, and stays, and so does whatever it uses. */
static bool unload(int slot) {
    struct module *m = &mods[slot];

    for (int i = 0; i < MODULES; i++) {
        if (mods[i].name[0] != '\0' && (mods[i].needs & 1u << slot) != 0 && !unload(i)) {
            return false;
        }
    }
    uint64_t exit = defined(m, "module_exit");

    if (exit != 0 && ((int (*)(void))exit)() < 0) {
        strcpy(why, "in use");
        return false;
    }
    release(m);
    return true;
}

uint32_t module_memory(void) {
    return held * 4096;
}

/* ---- modman ------------------------------------------------------------------ */

/* The module a table entry is, into out, if it is one: its path below the
   folder whose table name is dir characters long. */
static bool module_named(const char *entry, size_t dir, char *out) {
    size_t len = strlen(entry), ext = strlen(EXT);

    if (len < dir + ext + 3 || len - dir - ext >= NAME_LEN ||
        strcmp(entry + len - ext, EXT) != 0) {
        return false;
    }
    memcpy(out, entry + dir, len - dir - ext);
    out[len - dir - ext] = '\0';
    return strchr(out, '/') != NULL;
}

/* The modules in the folder whose table name is top: its category folders,
   and the .kmod files in each. */
static void list_in(const char *top) {
    char cat[FS_NAME_LEN], name[NAME_LEN];
    struct fs_file folder, file;
    size_t outer = 0, inner, index;

    while (fs_list(top, &outer, &folder, &index) == 0) {
        size_t len = strlen(folder.name);

        if (len == 0 || folder.name[len - 1] != '/') {
            continue;
        }
        strcpy(cat, folder.name);
        inner = 0;
        while (fs_list(cat, &inner, &file, &index) == 0) {
            if (!module_named(file.name, strlen(top), name)) {
                continue;
            }
            int slot = slot_of(name);
            size_t pad = strlen(name);

            vga_puts(name);
            do {
                vga_puts(" ");
            } while (++pad < 20);
            if (slot >= 0) {
                kprintf("loaded %uK\n", (mods[slot].span + 1023) / 1024);
            } else {
                vga_puts("-\n");
            }
        }
    }
}

/* Every module there is, in each folder of MODPATH. */
static void list(void) {
    char dir[FS_NAME_LEN], top[FS_NAME_LEN];
    const char *dirs = shell_env("MODPATH");

    while (dirs != NULL && *dirs != '\0') {
        const char *end = strchr(dirs, ':');
        size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);

        if (n > 0 && n < sizeof dir) {
            memcpy(dir, dirs, n);
            dir[n] = '\0';
            if (fs_folder(dir, top, sizeof top) == 0) {
                list_in(top);
            }
        }
        dirs = end != NULL ? end + 1 : dirs + n;
    }
}

/* modman auto <category>: every module of the category that starts on
   this machine is enabled - a driver whose hardware is not here answers
   -ENODEV and is let go again. Each one enabled is named. */
static void auto_enable(const char *cat) {
    char dir[FS_NAME_LEN], top[FS_NAME_LEN], folder[FS_NAME_LEN], name[NAME_LEN];
    const char *dirs = shell_env("MODPATH");
    struct fs_file file;
    size_t cursor, index;
    unsigned enabled = 0;

    while (dirs != NULL && *dirs != '\0') {
        const char *end = strchr(dirs, ':');
        size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);

        if (n > 0 && n + strlen(cat) + 2 < sizeof dir) {
            memcpy(dir, dirs, n);
            dir[n] = '\0';
            if (fs_folder(dir, top, sizeof top) == 0) {
                ksprintf(dir + n, "/%s", cat);
                cursor = 0;
                while (fs_folder(dir, folder, sizeof folder) == 0 &&
                       fs_list(folder, &cursor, &file, &index) == 0) {
                    if (!module_named(file.name, strlen(top), name)) {
                        continue;
                    }
                    if (slot_of(name) < 0 && load_one(name) < 0) {
                        if (strcmp(why, "no device") != 0) {
                            kprintf("modman: %s: %s\n", name, why);
                        }
                        continue;
                    }
                    kprintf("enabled %s\n", name);
                    enabled++;
                }
            }
        }
        dirs = end != NULL ? end + 1 : dirs + n;
    }
    if (enabled == 0) {
        kprintf("modman: nothing in %s for this machine\n", cat);
    }
}

void module_command(char *args) {
    const char *verb = str_word(&args);
    const char *name = str_word(&args);

    if (*verb == '\0') {
        list();
        return;
    }
    bool on = strcmp(verb, "enable") == 0, off = strcmp(verb, "disable") == 0;

    if (strcmp(verb, "takeover") == 0 && *name == '\0') {
        const char *not = efi_leave();

        if (not != NULL) {
            kprintf("modman: takeover: %s\n", not);
        }
        return;
    }
    if (strcmp(verb, "auto") == 0 && *name != '\0' && strchr(name, '/') == NULL) {
        auto_enable(name);
        return;
    }
    if ((!on && !off) || *name == '\0') {
        vga_puts("usage: modman [enable|disable <category/name>|auto <category>|takeover]\n");
        return;
    }
    if (on && !load(name)) {
        kprintf("modman: %s: %s\n", name, why);
        return;
    }
    if (off) {
        char path[FS_NAME_LEN];
        int slot = slot_of(name);

        if (slot < 0) {
            kprintf("modman: %s: %s\n", name,
                    module_file(name, path, sizeof path) ? "not loaded" : "no such module");
        } else if (!unload(slot)) {
            kprintf("modman: %s: %s\n", name, why);
        }
    }
}
