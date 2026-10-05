/* input/ps2: the PS/2 keyboard, and the mouse on the second port.
 *
 * The firmware's own keyboard driver, which empties the port on a timer, is
 * stopped, and every byte is read here instead - the way an operating system
 * normally runs it, and what lets a key with no character of its own be
 * turned into the escape sequence a terminal sends for it. A keyboard on USB
 * is left to the firmware and is not affected.
 *
 * The kernel takes it over as it lets the firmware go (ps2_init), and not
 * before: until then the firmware's driver is the one reading. */

#include <stdbool.h>
#include <stddef.h>

#include "debug.h"
#include "driver.h"
#include "efi.h"
#include "efi_kernel.h"
#include "input.h"
#include "io.h"
#include "mem.h"
#include "module.h"

#define EBUSY 16

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT 0x01      /* a byte is waiting */
#define STATUS_INPUT  0x02      /* the controller is still taking the last one */
#define STATUS_AUX    0x20      /* and it came from the second port, not the keyboard */

#define CMD_READ_CONFIG  0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_ENABLE_KBD   0xAE
#define CMD_ENABLE_AUX   0xA8
#define CMD_WRITE_AUX    0xD4   /* the next data byte goes to the mouse */

#define CONFIG_IRQS      0x03   /* interrupts, which nothing here takes */
#define CONFIG_CLOCKS    0x30   /* set, they switch a port off */
#define CONFIG_AUX_OFF   0x20   /* the second port's */
#define CONFIG_TRANSLATE 0x40   /* hand over scancode set 1 */

#define DEV_ENABLE   0xF4

/* ACPI's name for a PS/2 keyboard, compressed the EISA way: PNP03xx. */
#define EISA_PNP      0x41D0
#define PNP_KEYBOARD  0x03
#define PNP_MOUSE     0x0F      /* PNP0Fxx */

static bool present;
static bool mouse;              /* one answered on the second port */

/* ---- stopping the firmware's drivers ------------------------------------ */

/* Whether a handle's device path ends at a PS/2 keyboard or mouse. */
static bool is_ps2(efi_handle handle) {
    struct efi_guid guid = EFI_DEVICE_PATH_GUID;
    struct efi_device_path *node;

    if (EFI_ERROR(efi_boot()->system->boot->handle_protocol(handle, &guid,
                                                            (void **)&node))) {
        return false;
    }
    while (node->type != 0x7F) {
        unsigned length = node->length[0] | node->length[1] << 8;

        if (node->type == 2 && node->subtype == 1 && length >= 8) {
            uint32_t hid = *(uint32_t *)((uint8_t *)node + 4);
            unsigned kind = hid >> 24;      /* PNP0303 is 0x030341D0 */

            if ((hid & 0xFFFF) == EISA_PNP && (kind == PNP_KEYBOARD || kind == PNP_MOUSE)) {
                return true;
            }
        }
        if (length < 4) {
            break;                  /* a broken path: stop rather than loop */
        }
        node = (struct efi_device_path *)((uint8_t *)node + length);
    }
    return false;
}

/* Stops every firmware driver sitting on a PS/2 device. Returns whether there
   was one, which is the only trustworthy sign the controller exists at all:
   on a machine without one the ports read back as all ones. */
static bool stop_firmware(void) {
    struct efi_boot_services *bs;
    struct efi_guid guid = EFI_DEVICE_PATH_GUID;
    efi_handle *handles;
    efi_uintn count = 0;
    bool found = false;

    /* A BIOS drives it from interrupts the kernel never takes, so there is
       nothing to stop: the controller is there if its port answers. */
    if (efi_boot()->system == NULL) {
        return inb(PS2_STATUS) != 0xFF;
    }
    bs = efi_boot()->system->boot;
    if (EFI_ERROR(bs->locate_handle_buffer(2 /* by protocol */, &guid, NULL,
                                           &count, &handles))) {
        return false;
    }
    for (efi_uintn i = 0; i < count; i++) {
        if (is_ps2(handles[i])) {
            efi_status st = bs->disconnect_controller(handles[i], NULL, NULL);

            dbg("ps2: disconnected firmware driver: %x\n", st);
            found = true;
        }
    }
    bs->free_pool(handles);
    return found;
}

/* ---- talking to the controller ------------------------------------------ */

static void wait_input(void) {
    for (unsigned i = 0; i < 100000 && (inb(PS2_STATUS) & STATUS_INPUT); i++) {
        __asm__ volatile("pause");
    }
}

static void command(uint8_t value) {
    wait_input();
    outb(PS2_COMMAND, value);
}

static void data(uint8_t value) {
    wait_input();
    outb(PS2_DATA, value);
}

/* Waits for a byte from the controller itself, or from the given port. */
static int read_byte(bool aux) {
    for (unsigned i = 0; i < 200000; i++) {
        uint8_t status = inb(PS2_STATUS);

        if (status & STATUS_OUTPUT) {
            uint8_t byte = inb(PS2_DATA);

            if (((status & STATUS_AUX) != 0) == aux) {
                return byte;
            }
            continue;               /* the other device's: not now */
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static void drain(void) {
    for (unsigned i = 0; i < 64 && (inb(PS2_STATUS) & STATUS_OUTPUT); i++) {
        (void)inb(PS2_DATA);
    }
}

static bool ps2_init(void) {
    if (!stop_firmware()) {
        dbg("ps2: no controller\n");
        return false;
    }
    present = true;
    drain();

    /* The keyboard port on, set 1 scancodes, no interrupts: it is polled. */
    command(CMD_READ_CONFIG);
    int config = read_byte(false);
    if (config < 0) {
        config = CONFIG_TRANSLATE;
    }
    config = (config & ~(CONFIG_IRQS | CONFIG_CLOCKS)) | CONFIG_TRANSLATE | CONFIG_AUX_OFF;
    command(CMD_WRITE_CONFIG);
    data((uint8_t)config);
    command(CMD_ENABLE_KBD);

    data(DEV_ENABLE);               /* the firmware may have left it quiet */
    (void)read_byte(false);

    /* A controller with a second port turns its clock on when told to; one
       without leaves the bit as it was. Then the mouse is asked to talk. */
    command(CMD_ENABLE_AUX);
    command(CMD_READ_CONFIG);
    int aux = read_byte(false);

    if (aux >= 0 && (aux & CONFIG_AUX_OFF) == 0) {
        command(CMD_WRITE_AUX);
        data(DEV_ENABLE);
        mouse = read_byte(true) == 0xFA;
    }

    dbg("ps2: config %x mouse %u\n", (uint64_t)config, (uint64_t)mouse);
    return true;
}

/* ---- the keyboard ------------------------------------------------------- */

#define SC_RELEASE  0x80
#define SC_EXTENDED 0xE0
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D        /* the right one is the same, behind E0 */
#define SC_CAPS     0x3A
#define SC_ALT      0x38

/* Scancode set 1, up to the space bar. Anything at 0 has no character. */
static const char unshifted[0x3A] = {
    [0x01] = '\033', [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x37] = '*', [0x39] = ' ',
};

static const char shifted[0x3A] = {
    [0x01] = '\033', [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x37] = '*', [0x39] = ' ',
};

#define KEYS 16                     /* typed ahead and not yet read */

static char    keys[KEYS];
static uint8_t key_head, key_tail;
static bool    shift, ctrl, alt, caps, extended;

static void key_push(char c) {
    if (c != 0 && (uint8_t)(key_tail - key_head) < KEYS) {
        keys[key_tail++ % KEYS] = c;
    }
}

/* What a key with no character of its own sends: the escape sequence every
   terminal has sent for it since the VT100, which is what a program doing
   its own line editing is watching for. */
static const char *grey_key(uint8_t code) {
    switch (code) {
    case 0x48: return "\033[A";     /* up */
    case 0x50: return "\033[B";     /* down */
    case 0x4D: return "\033[C";     /* right */
    case 0x4B: return "\033[D";     /* left */
    case 0x47: return "\033[H";     /* home */
    case 0x4F: return "\033[F";     /* end */
    case 0x52: return "\033[2~";    /* insert */
    case 0x53: return "\033[3~";    /* delete */
    case 0x49: return "\033[5~";    /* page up */
    case 0x51: return "\033[6~";    /* page down */
    default:   return NULL;
    }
}

/* F1 to F12, as the Linux console sends them - what TERM=linux says. */
static const char *function_key(uint8_t code) {
    static const char *const keys[] = {
        "\033[[A", "\033[[B", "\033[[C", "\033[[D", "\033[[E",
        "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~",
    };

    return code >= 0x3B && code <= 0x44 ? keys[code - 0x3B]
         : code == 0x57 ? "\033[23~" : code == 0x58 ? "\033[24~" : NULL;
}

/* Linux's number for a key behind E0; set 1 numbers the rest the same as
   Linux does, up to F12. */
static unsigned extended_key(uint8_t key) {
    switch (key) {
    case 0x1C: return 96;           /* KEY_KPENTER */
    case 0x1D: return 97;           /* KEY_RIGHTCTRL */
    case 0x35: return 98;           /* KEY_KPSLASH */
    case 0x37: return 99;           /* KEY_SYSRQ */
    case 0x38: return 100;          /* KEY_RIGHTALT */
    case 0x47: return 102;          /* KEY_HOME */
    case 0x48: return 103;          /* KEY_UP */
    case 0x49: return 104;          /* KEY_PAGEUP */
    case 0x4B: return 105;          /* KEY_LEFT */
    case 0x4D: return 106;          /* KEY_RIGHT */
    case 0x4F: return 107;          /* KEY_END */
    case 0x50: return 108;          /* KEY_DOWN */
    case 0x51: return 109;          /* KEY_PAGEDOWN */
    case 0x52: return 110;          /* KEY_INSERT */
    case 0x53: return 111;          /* KEY_DELETE */
    case 0x5B: return 125;          /* KEY_LEFTMETA */
    case 0x5C: return 126;          /* KEY_RIGHTMETA */
    case 0x5D: return 127;          /* KEY_COMPOSE */
    default:   return 0;
    }
}

static void key_byte(uint8_t code) {
    uint8_t key = code & ~SC_RELEASE;
    bool was_extended = extended;
    char c = 0;

    extended = code == SC_EXTENDED;
    if (extended) {
        return;
    }
    unsigned keycode = was_extended ? extended_key(key) : key <= 0x58 ? key : 0;

    if (keycode != 0) {
        input_report(INPUT_KEYBOARD, EV_KEY, keycode, (code & SC_RELEASE) == 0);
        input_report(INPUT_KEYBOARD, EV_SYN, 0, 0);
    }
    if (key == SC_LSHIFT || key == SC_RSHIFT) {
        if (!was_extended) {        /* E0 2A is a fake shift some keys send */
            shift = (code & SC_RELEASE) == 0;
        }
        return;
    }
    if (key == SC_CTRL) {
        ctrl = (code & SC_RELEASE) == 0;
        return;
    }
    if (key == SC_ALT) {            /* left Alt, or right with E0 before it */
        alt = (code & SC_RELEASE) == 0;
        return;
    }
    if (code & SC_RELEASE) {
        return;
    }
    if (code == SC_CAPS) {
        caps = !caps;
        return;
    }
    const char *text = was_extended ? grey_key(code) : function_key(code);

    if (text != NULL) {
        while (*text != '\0') {
            key_push(*text++);
        }
        return;
    }
    if (was_extended) {
        /* Of the rest, only the keypad's Enter and slash type anything. */
        c = code == 0x1C ? '\n' : code == 0x35 ? '/' : 0;
    } else if (code < sizeof unshifted) {
        c = shift ? shifted[code] : unshifted[code];
        if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c ^= 0x20;
        }
    }
    if (ctrl && c >= '@' && c < 0x7F) {
        /* Held down, a key types the control character it stands for: Ctrl-D
           is 4, which ends a program's input, and a shell of its own knows
           what to do with the rest. Letters come out the same either case. */
        c = (char)(c >= 'a' ? c - 'a' + 1 : c - '@');
    }
    if (alt && c != 0) {
        key_push('\033');           /* Alt is Escape first, as Linux sends it */
    }
    key_push(c);
}

/* ---- the mouse ---------------------------------------------------------- */

static uint8_t  packet[3];
static unsigned packet_n;
static uint8_t  buttons;

/* Three bytes a movement: buttons and signs, then x, then y - y counting
   up, where the screen counts down. The first always has bit 3 set, which
   is how a byte lost along the way is noticed. */
static void mouse_byte(uint8_t byte) {
    static const unsigned button[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };

    if (packet_n == 0 && (byte & 0x08) == 0) {
        return;
    }
    packet[packet_n++] = byte;
    if (packet_n < 3) {
        return;
    }
    packet_n = 0;
    int dx = packet[1] - (packet[0] << 4 & 0x100);
    int dy = packet[2] - (packet[0] << 3 & 0x100);

    if ((packet[0] & 0xC0) == 0) {  /* not overflowed */
        if (dx != 0) {
            input_report(INPUT_MOUSE, EV_REL, REL_X, dx);
        }
        if (dy != 0) {
            input_report(INPUT_MOUSE, EV_REL, REL_Y, -dy);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        if (((packet[0] ^ buttons) >> i & 1) != 0) {
            input_report(INPUT_MOUSE, EV_KEY, button[i], packet[0] >> i & 1);
        }
    }
    buttons = packet[0] & 7;
    input_report(INPUT_MOUSE, EV_SYN, 0, 0);
}

static void ps2_poll(void) {
    if (!present) {
        return;
    }
    for (;;) {
        uint8_t status = inb(PS2_STATUS);

        if ((status & STATUS_OUTPUT) == 0) {
            return;
        }
        uint8_t byte = inb(PS2_DATA);

        if (!(status & STATUS_AUX)) {
            key_byte(byte);
        } else if (mouse) {
            mouse_byte(byte);
        }
    }
}

static char ps2_key(void) {
    ps2_poll();
    if (key_head == key_tail) {
        return 0;
    }
    return keys[key_head++ % KEYS];
}

/* ---- the module ------------------------------------------------------- */

static const struct keyboard_driver driver = { ps2_init, ps2_key };

MODULE_EXPORT int module_init(void) {
    keyboard_register(&driver, true);
    return 0;
}

/* Once it has the keyboard, nothing else is reading it. */
MODULE_EXPORT int module_exit(void) {
    if (present && efi_gone()) {
        return -EBUSY;
    }
    keyboard_register(&driver, false);
    return 0;
}
