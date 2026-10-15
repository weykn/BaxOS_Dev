#include "share.h"

#include "mem.h"
#include "string.h"

/* Every page is an entry, found two ways: by what it is - the file and the
   page in it - when a process maps it, and by where it is when a process
   lets go of it, which is all a page table entry says. Both are chains
   through the entries, from heads hashed into a table as long as there is
   room for entries. Neither needs anything but the entries to build, so
   growing is building both again. */

#define PAGE        4096
#define SECTORS     (PAGE / 512)
#define FIRST_ROOM  256

struct entry {
    uint32_t start;         /* the file's first sector; 0 once written over */
    uint32_t index;         /* the page in the file */
    uint32_t frame;         /* where the page is, in pages */
    uint32_t refs;          /* 0: the entry is free */
    uint32_t next_key;      /* the chains, one-based, 0 at the end */
    uint32_t next_frame;
};

static struct entry *entries;
static uint32_t *by_key, *by_frame;     /* heads, one per entry there is room for */
static uint32_t room, used, free_head;

static uint32_t key_hash(uint32_t start, uint32_t index) {
    return (start * 2654435761u ^ index * 40503u) & (room - 1);
}

static uint32_t frame_hash(uint32_t frame) {
    return (frame * 2654435761u) & (room - 1);
}

static void link_key(uint32_t i) {
    uint32_t *head = &by_key[key_hash(entries[i].start, entries[i].index)];

    entries[i].next_key = *head;
    *head = i + 1;
}

static void unlink_key(uint32_t i) {
    uint32_t *at = &by_key[key_hash(entries[i].start, entries[i].index)];

    while (*at != 0 && *at != i + 1) {
        at = &entries[*at - 1].next_key;
    }
    if (*at == i + 1) {
        *at = entries[i].next_key;
    }
    entries[i].start = 0;
}

static struct entry *of_frame(uint64_t page, uint32_t **link) {
    uint32_t frame = (uint32_t)(page / PAGE);
    uint32_t *at = &by_frame[frame_hash(frame)];

    while (*at != 0 && entries[*at - 1].frame != frame) {
        at = &entries[*at - 1].next_frame;
    }
    *link = at;
    return *at != 0 ? &entries[*at - 1] : NULL;
}

/* Room for bigger entries - more or fewer than now, but never fewer than
   are used - the used ones packed at the front and both chains built
   again; with none used and bigger 0, nothing at all. */
static bool resize(uint32_t bigger) {
    if (bigger == 0) {
        mem_free(entries);
        mem_free(by_key);
        mem_free(by_frame);
        entries = NULL;
        by_key = by_frame = NULL;
        room = free_head = 0;
        return true;
    }
    struct entry *e = mem_alloc((size_t)bigger * sizeof *e);
    uint32_t *k = mem_alloc((size_t)bigger * sizeof *k);
    uint32_t *f = mem_alloc((size_t)bigger * sizeof *f);

    if (e == NULL || k == NULL || f == NULL) {
        mem_free(e);
        mem_free(k);
        mem_free(f);
        return false;
    }
    memset(e, 0, (size_t)bigger * sizeof *e);
    memset(k, 0, (size_t)bigger * sizeof *k);
    memset(f, 0, (size_t)bigger * sizeof *f);
    for (uint32_t i = 0, n = 0; i < room; i++) {
        if (entries[i].refs != 0) {
            e[n++] = entries[i];
        }
    }
    mem_free(entries);
    mem_free(by_key);
    mem_free(by_frame);
    entries = e;
    by_key = k;
    by_frame = f;
    room = bigger;
    free_head = 0;
    for (uint32_t i = room; i-- > 0;) {
        if (entries[i].refs == 0) {
            entries[i].next_key = free_head;    /* the free ones, through next_key */
            free_head = i + 1;
            continue;
        }
        if (entries[i].start != 0) {
            link_key(i);
        }
        uint32_t *head = &by_frame[frame_hash(entries[i].frame)];

        entries[i].next_frame = *head;
        *head = i + 1;
    }
    return true;
}

static bool grow(void) {
    return resize(room == 0 ? FIRST_ROOM : room * 2);
}

/* After an entry is let go: the table shrinks as what is shared does, and
   goes once nothing is. */
static void shrink(void) {
    if (used == 0) {
        resize(0);
    } else if (room > FIRST_ROOM && used < room / 8) {
        resize(room / 2);
    }
}

uint64_t shared_find(uint32_t start, uint32_t index) {
    if (room == 0) {
        return 0;
    }
    for (uint32_t at = by_key[key_hash(start, index)]; at != 0; at = entries[at - 1].next_key) {
        struct entry *e = &entries[at - 1];

        if (e->start == start && e->index == index) {
            e->refs++;
            return (uint64_t)e->frame * PAGE;
        }
    }
    return 0;
}

bool shared_add(uint32_t start, uint32_t index, uint64_t page) {
    if (free_head == 0 && !grow()) {
        return false;
    }
    uint32_t i = free_head - 1;

    free_head = entries[i].next_key;
    entries[i] = (struct entry){ .start = start, .index = index,
                                 .frame = (uint32_t)(page / PAGE), .refs = 1 };
    link_key(i);
    uint32_t *head = &by_frame[frame_hash(entries[i].frame)];

    entries[i].next_frame = *head;
    *head = i + 1;
    used++;
    return true;
}

bool shared_adopt(uint64_t page) {
    if (free_head == 0 && !grow()) {
        return false;
    }
    uint32_t i = free_head - 1;

    free_head = entries[i].next_key;
    entries[i] = (struct entry){ .frame = (uint32_t)(page / PAGE), .refs = 1 };
    uint32_t *head = &by_frame[frame_hash(entries[i].frame)];

    entries[i].next_frame = *head;
    *head = i + 1;
    used++;
    return true;
}

bool shared_take(uint64_t page) {
    uint32_t *link;
    struct entry *e = of_frame(page, &link);

    if (e == NULL || e->refs != 1) {
        return false;
    }
    uint32_t i = (uint32_t)(e - entries);

    *link = e->next_frame;
    if (e->start != 0) {
        unlink_key(i);              /* changed from here on: not the file's any more */
    }
    e->refs = 0;
    e->next_key = free_head;
    free_head = i + 1;
    used--;
    shrink();
    return true;
}

void shared_hold(uint64_t page) {
    uint32_t *link;
    struct entry *e = of_frame(page, &link);

    if (e != NULL) {
        e->refs++;
    }
}

void shared_drop(uint64_t page) {
    uint32_t *link;
    struct entry *e = of_frame(page, &link);

    if (e == NULL || --e->refs > 0) {
        return;
    }
    uint32_t i = (uint32_t)(e - entries);

    *link = e->next_frame;
    if (e->start != 0) {
        unlink_key(i);
    }
    mem_pages_free(page, 1);
    e->refs = 0;
    e->next_key = free_head;
    free_head = i + 1;
    used--;
    shrink();
}

void shared_written(uint32_t lba, unsigned count) {
    for (uint32_t i = 0; i < room; i++) {
        struct entry *e = &entries[i];
        uint32_t first = e->start + e->index * SECTORS;

        if (e->refs != 0 && e->start != 0 && first < lba + count && lba < first + SECTORS) {
            unlink_key(i);          /* those who have it keep it; nobody new */
        }
    }
}

size_t shared_bytes(void) {
    return (size_t)used * PAGE +
           (room != 0 ? (size_t)room * (sizeof(struct entry) + 2 * sizeof(uint32_t)) : 0);
}
