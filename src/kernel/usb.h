#pragma once

#include <stdbool.h>
#include <stdint.h>

/* USB, as modules share it. A host controller driver (usb/xhci) brings its
 * controllers up when first asked, finds what is plugged in - behind hubs
 * too - and sets each device up for the one interface of it something here
 * can drive: a boot keyboard, or bulk-only mass storage. The class drivers
 * (input/usbkbd, storage/usb) find theirs among the devices and talk to it
 * through the host, never to the controller. */

#define USB_IN 0x80                 /* an endpoint address's direction bit */

/* Interface classes the host sets devices up for. */
#define USB_CLASS_HID     0x03      /* subclass 1, protocol 1: boot keyboard */
#define USB_CLASS_STORAGE 0x08      /* protocol 0x50: bulk-only */

struct usb_device {
    uint8_t  class, subclass, protocol;
    uint8_t  interface;             /* its number, for class requests */
    uint8_t  in, out;               /* its endpoints' addresses; 0, none */
    uint16_t in_size;               /* the in endpoint's packet size */
};

struct usb_host {
    /* Takes the controllers from the firmware and finds what is on them,
       the first time; false if there were none to take. */
    bool (*start)(void);

    /* Device i, or NULL past the last. */
    const struct usb_device *(*device)(unsigned i);

    /* A request on endpoint 0, as USB writes them; length bytes to or
       from data, at most 256, by the direction in type's top bit. 0 or
       -1. */
    int (*control)(const struct usb_device *d, uint8_t type, uint8_t request,
                   uint16_t value, uint16_t index, void *data, uint16_t length);

    /* A bulk transfer on the endpoint at address, direction in its top
       bit. data is read or written by the controller itself, so it must
       be the kernel's own memory, below 4 GiB. Bytes moved, or -1; a
       stalled endpoint is cleared before it returns. */
    int (*bulk)(const struct usb_device *d, uint8_t address, void *data, uint32_t length);

    /* The in endpoint is an interrupt one: copies the report that came
       since the last call into data, up to length, and answers its size;
       0 if none has, -1 if the device is gone. */
    int (*report)(const struct usb_device *d, void *data, unsigned length);
};

extern const struct usb_host *usb_host;
void usb_host_register(const struct usb_host *h);
