# Advanced Operations

## Firmware Takeover

`modman takeover` makes Tuxlet OS stop using the firmware (UEFI boot services or BIOS interrupts) and hand the firmware's memory to programs. After takeover, timer-driven scheduling (`sched/timer`) becomes available.

Before invoking takeover mode, ensure:

1. A **storage module** (`storage/ide`, `storage/ahci`, `storage/nvme` or `storage/usb`) is active.
2. An **input module** (`input/ps2` or `input/usbkbd`) is loaded.
3. Your display **`mode`** is configured (video resolution cannot be altered post-takeover).

```sh
modman enable storage/ide
modman enable input/ps2
modman takeover
```

## Boot Configuration

Startup is two `tsh` scripts:

1. **`/etc/tuxlet/boot`**: Runs first, from its own folder. Use it to set the environment, load driver modules, set the display mode and start services. `/etc/tuxlet/boot-example` is a complete one that ends by starting X.
2. **`/etc/tuxlet/shell`**: Runs after `boot`, from `$HOME`, and runs again every time it ends. Without it, you get an interactive `tsh` prompt.
