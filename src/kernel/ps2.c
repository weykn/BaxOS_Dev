#include "ps2.h"

#include <stddef.h>

#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "io.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT 0x01      /* a byte is waiting */
#define STATUS_INPUT  0x02      /* the controller is still taking the last one */
#define STATUS_AUX    0x20      /* and it came from the second port, not the keyboard */

#define CMD_READ_CONFIG  0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_ENABLE_KBD   0xAE

#define CONFIG_IRQS      0x03   /* interrupts, which nothing here takes */
#define CONFIG_CLOCKS    0x30   /* set, they switch a port off */
#define CONFIG_TRANSLATE 0x40   /* hand over scancode set 1 */

#define DEV_ENABLE   0xF4

/* ACPI's name for a PS/2 keyboard, compressed the EISA way: PNP03xx. */
#define EISA_PNP      0x41D0
#define PNP_KEYBOARD  0x03

static bool present;

/* ---- stopping the firmware's drivers ------------------------------------ */

/* Whether a handle's device path ends at a PS/2 keyboard. */
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

            if ((hid & 0xFFFF) == EISA_PNP && kind == PNP_KEYBOARD) {
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
    struct efi_boot_services *bs = efi_boot()->system->boot;
    struct efi_guid guid = EFI_SIMPLE_TEXT_INPUT_GUID;
    efi_handle *handles;
    efi_uintn count = 0;
    bool found = false;

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

bool ps2_init(void) {
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
    config = (config & ~(CONFIG_IRQS | CONFIG_CLOCKS)) | CONFIG_TRANSLATE;
    command(CMD_WRITE_CONFIG);
    data((uint8_t)config);
    command(CMD_ENABLE_KBD);

    data(DEV_ENABLE);               /* the firmware may have left it quiet */
    (void)read_byte(false);

    dbg("ps2: config %x\n", (uint64_t)config);
    return true;
}

/* ---- the keyboard ------------------------------------------------------- */

#define SC_RELEASE  0x80
#define SC_EXTENDED 0xE0
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D        /* the right one is the same, behind E0 */
#define SC_CAPS     0x3A

/* Scancode set 1, up to the space bar. Anything at 0 has no character. */
static const char unshifted[0x3A] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
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
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
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
static bool    shift, ctrl, caps, extended;

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

static void key_byte(uint8_t code) {
    uint8_t key = code & ~SC_RELEASE;
    bool was_extended = extended;
    char c = 0;

    extended = code == SC_EXTENDED;
    if (extended) {
        return;
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
    if (code & SC_RELEASE) {
        return;
    }
    if (code == SC_CAPS) {
        caps = !caps;
        return;
    }
    if (was_extended) {
        const char *text = grey_key(code);

        if (text != NULL) {
            while (*text != '\0') {
                key_push(*text++);
            }
            return;
        }
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
    key_push(c);
}

void ps2_poll(void) {
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
            key_byte(byte);         /* anything from the other port is nobody's */
        }
    }
}

char ps2_key(void) {
    ps2_poll();
    if (key_head == key_tail) {
        return 0;
    }
    return keys[key_head++ % KEYS];
}
