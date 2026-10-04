/* input/usbkbd: a USB keyboard, through the USB host (usb/xhci). Every
 * keyboard there has a boot interface - eight-byte reports, the same on all
 * of them - which is what is asked for, so no report descriptor is parsed.
 * A report says which keys are down; a key that was not down in the last
 * one has just been pressed, and one held repeats from here, since the
 * keyboard only reports changes.
 *
 * After SeaBIOS's usb-hid.c, typed the way input/ps2 types: the same
 * characters, escape sequences and modifiers. */

#include "driver.h"
#include "efi_kernel.h"
#include "module.h"
#include "string.h"
#include "usb.h"

#define ENODEV 19
#define EBUSY  16

#define KBDS        2
#define DELAY_MS    500             /* before a held key repeats */
#define REPEAT_MS   33              /* and between repeats */

/* HID usages 4 to 0x38, letters to slash, with and without shift. */
static const char plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\033\b\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\033\b\t _+{}|~:\"~<>?";

static const struct usb_device *kbds[KBDS];
static unsigned kbd_count;
static uint8_t  last[KBDS][8];
static bool     caps;

static uint8_t  held;               /* the key repeating, if any */
static uint64_t held_next;

#define KEYS 16
static char    keys[KEYS];
static uint8_t key_head, key_tail;

static void push(char c) {
    if (c != 0 && (uint8_t)(key_tail - key_head) < KEYS) {
        keys[key_tail++ % KEYS] = c;
    }
}

static void push_text(const char *text) {
    while (*text != '\0') {
        push(*text++);
    }
}

/* What a key with no character sends, by usage: the same sequences as
   input/ps2. */
static const char *grey(uint8_t usage) {
    static const char *const function[] = {
        "\033[[A", "\033[[B", "\033[[C", "\033[[D", "\033[[E", "\033[17~",
        "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~",
    };

    switch (usage) {
    case 0x52: return "\033[A";
    case 0x51: return "\033[B";
    case 0x4F: return "\033[C";
    case 0x50: return "\033[D";
    case 0x4A: return "\033[H";
    case 0x4D: return "\033[F";
    case 0x49: return "\033[2~";
    case 0x4C: return "\033[3~";
    case 0x4B: return "\033[5~";
    case 0x4E: return "\033[6~";
    default:
        return usage >= 0x3A && usage <= 0x45 ? function[usage - 0x3A] : NULL;
    }
}

/* The keypad, unlocked or not: digits and operators. */
static char keypad(uint8_t usage) {
    static const char pad[] = "/*-+\n1234567890.";

    return usage >= 0x54 && usage <= 0x63 ? pad[usage - 0x54] : 0;
}

static void type(uint8_t usage, uint8_t mods) {
    bool shift = mods & 0x22, ctrl = mods & 0x11, alt = mods & 0x44;
    const char *text = grey(usage);
    char c = 0;

    if (text != NULL) {
        push_text(text);
        return;
    }
    if (usage >= 4 && usage <= 0x38) {
        c = shift ? shifted[usage - 4] : plain[usage - 4];
        if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c ^= 0x20;
        }
    } else {
        c = keypad(usage);
    }
    if (ctrl && c >= '@' && c < 0x7F) {
        c = (char)(c >= 'a' ? c - 'a' + 1 : c - '@');
    }
    if (alt && c != 0) {
        push('\033');
    }
    push(c);
}

/* A report from keyboard k: each key down now that was not before. */
static void report(unsigned k, const uint8_t *now) {
    for (unsigned i = 2; i < 8; i++) {
        uint8_t usage = now[i];
        bool before = false;

        if (usage < 4) {
            continue;                   /* none, or too many keys at once */
        }
        for (unsigned j = 2; j < 8; j++) {
            before |= last[k][j] == usage;
        }
        if (before) {
            continue;
        }
        if (usage == 0x39) {
            caps = !caps;
            continue;
        }
        type(usage, now[0]);
        held = usage;
        held_next = efi_uptime_ms() + DELAY_MS;
    }
    /* Repeating stops when its key comes up. */
    bool down = false;

    for (unsigned j = 2; j < 8; j++) {
        down |= now[j] == held;
    }
    if (!down) {
        held = 0;
    }
    memcpy(last[k], now, 8);
}

static char kbd_key(void) {
    uint8_t now[8];

    for (unsigned k = 0; k < kbd_count; k++) {
        if (usb_host->report(kbds[k], now, sizeof now) == 8) {
            report(k, now);
        }
    }
    if (held != 0 && key_head == key_tail && efi_uptime_ms() >= held_next) {
        type(held, last[0][0] | (kbd_count > 1 ? last[1][0] : 0));
        held_next = efi_uptime_ms() + REPEAT_MS;
    }
    return key_head != key_tail ? keys[key_head++ % KEYS] : 0;
}

static bool kbd_start(void) {
    return true;                        /* it has been the module's since it loaded */
}

static const struct keyboard_driver driver = { kbd_start, kbd_key };

MODULE_EXPORT int module_init(void) {
    const struct usb_device *d;

    if (usb_host == NULL || !usb_host->start()) {
        return -ENODEV;
    }
    for (unsigned i = 0; (d = usb_host->device(i)) != NULL && kbd_count < KBDS; i++) {
        if (d->class != USB_CLASS_HID || d->subclass != 1 || d->protocol != 1 || d->in == 0) {
            continue;
        }
        /* The boot protocol, and reports only when something changes. */
        usb_host->control(d, 0x21, 0x0B /* SET_PROTOCOL */, 0, d->interface, NULL, 0);
        usb_host->control(d, 0x21, 0x0A /* SET_IDLE */, 0, d->interface, NULL, 0);
        usb_host->report(d, NULL, 0);
        kbds[kbd_count++] = d;
    }
    if (kbd_count == 0) {
        return -ENODEV;
    }
    keyboard_register(&driver, true);
    return 0;
}

/* Its keyboards are nobody else's once the host has them. */
MODULE_EXPORT int module_exit(void) {
    if (efi_gone()) {
        return -EBUSY;
    }
    keyboard_register(&driver, false);
    return 0;
}
