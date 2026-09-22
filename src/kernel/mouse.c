#include "mouse.h"

#include <stddef.h>

#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "ps2.h"
#include "vga.h"

/* The pointer, from whichever devices the machine has - all of them at once,
 * the way Linux merges every mouse into one pointer:
 *
 *   vmmouse   the absolute pointer QEMU, VMware and VirtualBox offer through
 *             the VMware backdoor port. It reports where the host's cursor
 *             is, so the pointer simply follows it, with nothing to grab.
 *   PS/2      the mouse port of the keyboard controller: a laptop's touchpad,
 *             or a USB mouse the firmware passes off as PS/2.
 *   firmware  pointers the firmware has its own driver for, typically a USB
 *             mouse or a touchscreen. Only real devices are opened: the
 *             console multiplexer's merged one would read them first. */

static unsigned pointer_x, pointer_y;
static unsigned limit_x, limit_y;
static bool     down, click, fresh, found;

static unsigned clamp(int64_t value, unsigned limit) {
    if (value < 0) {
        return 0;
    }
    return (uint64_t)value < limit ? (unsigned)value : limit - 1;
}

static void move_by(int dx, int dy) {
    pointer_x = clamp((int64_t)pointer_x + dx, limit_x);
    pointer_y = clamp((int64_t)pointer_y + dy, limit_y);
}

/* Records the left button. A press is caught here, report by report, rather
   than by comparing before and after a poll - a quick click can go down and
   up again within one. */
static void set_button(bool now) {
    if (now && !down) {
        click = true;
    }
    down = now;
}

/* Maps value, somewhere in min .. max, onto 0 .. limit. */
static unsigned scale(uint64_t value, uint64_t min, uint64_t max, unsigned limit) {
    if (max <= min) {
        return clamp((int64_t)value, limit);
    }
    if (value < min) {
        value = min;
    }
    return clamp((int64_t)((value - min) * limit / (max - min + 1)), limit);
}

/* Relative movement, sped up for quick moves like a desktop does: slow ones
   are kept as they are, for precision; fast ones go further, to cross the
   screen without lifting the mouse. */
static int accelerate(int d) {
    int size = d < 0 ? -d : d;

    return size <= 3 ? d : size <= 10 ? d * 2 : d * 3;
}

/* ---- vmmouse ------------------------------------------------------------ */

#define VM_MAGIC   0x564D5868u     /* "VMXh" */
#define VM_PORT    0x5658

#define VM_GETVERSION 10
#define VM_DATA       39
#define VM_STATUS     40
#define VM_COMMAND    41

#define VM_ENABLE       0x45414552u
#define VM_ABSOLUTE     0x53424152u
#define VM_VERSION_ID   0x3442554Au

#define VM_LEFT 0x20

static bool vmmouse;

struct vm_regs {
    uint32_t a, b, c, d;
};

/* One call through the backdoor: an IN from its port with the magic in EAX
   and the command in ECX. The hypervisor answers in all four registers. On a
   real machine nothing is there, and EBX comes back unchanged. */
static struct vm_regs backdoor(uint32_t cmd, uint32_t arg) {
    struct vm_regs r = { VM_MAGIC, arg, cmd, VM_PORT };

    __asm__ volatile("inl %%dx, %%eax"
                     : "+a"(r.a), "+b"(r.b), "+c"(r.c), "+d"(r.d)
                     : : "memory");
    return r;
}

static bool vmmouse_init(void) {
    if (backdoor(VM_GETVERSION, ~VM_MAGIC).b != VM_MAGIC) {
        return false;               /* not a hypervisor that speaks it */
    }
    backdoor(VM_COMMAND, VM_ENABLE);
    if ((backdoor(VM_STATUS, 0).a & 0xFFFF) == 0 ||
        backdoor(VM_DATA, 1).a != VM_VERSION_ID) {
        return false;
    }
    backdoor(VM_COMMAND, VM_ABSOLUTE);
    return true;
}

static bool poll_vmmouse(void) {
    bool news = false;

    for (;;) {
        uint32_t status = backdoor(VM_STATUS, 0).a;

        if (status == 0xFFFF0000u) {
            /* The queue broke: switch it off and on again, as Linux does. */
            vmmouse = vmmouse_init();
            return news;
        }
        if ((status & 0xFFFF) < 4) {
            return news;
        }
        struct vm_regs r = backdoor(VM_DATA, 4);

        /* Absolute, 0 .. 0xFFFF across the screen. */
        pointer_x = scale(r.b, 0, 0xFFFF, limit_x);
        pointer_y = scale(r.c, 0, 0xFFFF, limit_y);
        set_button((r.a & VM_LEFT) != 0);
        news = true;
    }
}

/* ---- PS/2 --------------------------------------------------------------- */

#define AUX_RESET       0xFF
#define AUX_SAMPLE_RATE 0xF3
#define AUX_RESOLUTION  0xE8
#define AUX_ENABLE      0xF4
#define AUX_ACK         0xFA
#define AUX_PASSED      0xAA

static bool    ps2;
static uint8_t packet[3];
static unsigned packet_len;

/* The same bring-up Linux's psmouse does: reset, wait for the self test,
   then 100 reports a second at 8 counts per millimetre, and go. */
static bool ps2_mouse_init(void) {
    if (!ps2_init()) {
        return false;
    }
    if (ps2_aux_send(AUX_RESET) != AUX_ACK) {
        return false;
    }
    int result = -1;
    for (unsigned i = 0; i < 5 && result < 0; i++) {
        result = ps2_aux_read();    /* the self test takes its time */
    }
    if (result != AUX_PASSED) {
        return false;
    }
    (void)ps2_aux_read();           /* its ID, 0 for a plain mouse */

    ps2_aux_send(AUX_SAMPLE_RATE);
    ps2_aux_send(100);
    ps2_aux_send(AUX_RESOLUTION);
    ps2_aux_send(3);
    packet_len = 0;
    return ps2_aux_send(AUX_ENABLE) == AUX_ACK;
}

/* Called by ps2.c for every byte from the mouse port. Three make a report:
   buttons and signs, then X, then Y - measured upwards. */
void mouse_ps2_byte(uint8_t byte) {
    /* Bit 3 of the first byte is always set; a report that started mid-way is
       dropped until one lines up again. */
    if (packet_len == 0 && (byte & 0x08) == 0) {
        return;
    }
    packet[packet_len++] = byte;
    if (packet_len < 3) {
        return;
    }
    packet_len = 0;

    /* The hypervisor's absolute pointer nudges this one to say it has news;
       with that in use, these reports are only noise. */
    if (vmmouse || (packet[0] & 0xC0) != 0) {
        return;                     /* ...or they overflowed */
    }
    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);

    move_by(accelerate(dx), -accelerate(dy));
    set_button((packet[0] & 1) != 0);
}

/* ---- firmware ----------------------------------------------------------- */

#define POINTERS 4

static struct efi_simple_pointer   *relative[POINTERS];
static struct efi_absolute_pointer *absolute[POINTERS];
static unsigned relative_count, absolute_count;

/* Opens the protocol on every handle that is a real device - one with a
   device path, which the console multiplexer's merged pointer lacks. */
static unsigned open_devices(const struct efi_guid *guid, void **out) {
    struct efi_boot_services *bs = efi_boot()->system->boot;
    struct efi_guid path_guid = EFI_DEVICE_PATH_GUID;
    efi_handle *handles;
    efi_uintn count = 0;
    unsigned opened = 0;

    if (EFI_ERROR(bs->locate_handle_buffer(2 /* by protocol */, guid, NULL,
                                           &count, &handles))) {
        return 0;
    }
    for (efi_uintn i = 0; i < count && opened < POINTERS; i++) {
        void *path, *protocol;

        if (!EFI_ERROR(bs->handle_protocol(handles[i], &path_guid, &path)) &&
            !EFI_ERROR(bs->handle_protocol(handles[i], guid, &protocol))) {
            out[opened++] = protocol;
        }
    }
    bs->free_pool(handles);
    return opened;
}

static bool poll_firmware(void) {
    bool news = false;

    for (unsigned i = 0; i < absolute_count; i++) {
        struct efi_absolute_pointer *device = absolute[i];
        struct efi_absolute_state state;

        if (!EFI_ERROR(device->get_state(device, &state))) {
            pointer_x = scale(state.x, device->mode->min_x, device->mode->max_x, limit_x);
            pointer_y = scale(state.y, device->mode->min_y, device->mode->max_y, limit_y);
            set_button((state.buttons & 1) != 0);
            news = true;
        }
    }
    for (unsigned i = 0; i < relative_count; i++) {
        struct efi_pointer_state state;

        if (!EFI_ERROR(relative[i]->get_state(relative[i], &state))) {
            move_by(accelerate(state.x), accelerate(state.y));
            set_button(state.left != 0);
            news = true;
        }
    }
    return news;
}

/* ---- all of them -------------------------------------------------------- */

bool mouse_init(void) {
    struct efi_guid simple_guid = EFI_SIMPLE_POINTER_GUID;
    struct efi_guid absolute_guid = EFI_ABSOLUTE_POINTER_GUID;

    /* PS/2 first: it stops the firmware's own PS/2 drivers, and a protocol
       of theirs opened before that would be left dangling. It is set up even
       under a hypervisor, since the keyboard shares its controller. */
    ps2 = ps2_mouse_init();
    vmmouse = vmmouse_init();
    absolute_count = open_devices(&absolute_guid, (void **)absolute);
    relative_count = open_devices(&simple_guid, (void **)relative);
    for (unsigned i = 0; i < absolute_count; i++) {
        absolute[i]->reset(absolute[i], 0);
    }
    for (unsigned i = 0; i < relative_count; i++) {
        relative[i]->reset(relative[i], 0);
    }

    dbg("mouse: vmmouse %u, ps2 %u, firmware %u absolute %u relative\n",
        (uint64_t)vmmouse, (uint64_t)ps2, absolute_count, relative_count);
    found = vmmouse || ps2 || absolute_count + relative_count > 0;
    fresh = true;
    return found;
}

bool mouse_poll(void) {
    unsigned was_x = pointer_x, was_y = pointer_y;
    bool was_down = down;

    /* The screen may have changed size since the last look. */
    limit_x = vga_pixel_width();
    limit_y = vga_pixel_height();
    if (fresh) {
        /* Centred on the screen as it is now, which /conf may have resized
           since mouse_init. */
        pointer_x = limit_x / 2;
        pointer_y = limit_y / 2;
    }
    pointer_x = clamp(pointer_x, limit_x);
    pointer_y = clamp(pointer_y, limit_y);

    ps2_poll();                     /* the keyboard's bytes, and the mouse's */
    if (vmmouse) {
        poll_vmmouse();
    }
    poll_firmware();

    if (fresh) {
        fresh = false;
        return true;
    }
    return pointer_x != was_x || pointer_y != was_y || down != was_down;
}

bool mouse_present(void) {
    return found;
}

unsigned mouse_x(void) {
    return pointer_x;
}

unsigned mouse_y(void) {
    return pointer_y;
}

bool mouse_clicked(void) {
    bool was = click;

    click = false;
    return was;
}
