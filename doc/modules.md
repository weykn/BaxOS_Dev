# Kernel Modules

Drivers and optional kernel features are modules (`.kmod`), loaded with `modman`:

```sh
modman enable <module>   # Load a module
modman disable <module>  # Unload a module
modman auto <category>   # Load whichever drivers in a category find their hardware
modman                   # List modules and what is loaded
```

`modman` looks for modules in the folder named by `MODPATH`, normally `/usr/lib/modules`. Without `MODPATH`, nothing can load, including `compat/linux`, which most Linux programs need.

## Available Driver Matrix

| Category | Module | Provided Feature / Service |
| --- | --- | --- |
| **`storage`** | `storage/ide` | Low-level IDE disk controller support |
|  | `storage/ahci` | SATA disks on an AHCI controller |
|  | `storage/nvme` | NVMe SSDs |
|  | `storage/usb` | USB sticks and disks (bulk-only mass storage) |
|  | `storage/cache` | In-memory block cache (exposes `cache` command) |
| **`usb`** | `usb/xhci` | USB 3 (xHCI) host controller, used by the USB class drivers |
| **`input`** | `input/ps2` | PS/2 keyboard and mouse support |
|  | `input/usbkbd` | USB keyboard support (needs `usb/xhci`) |
| **`network`** | `network/stack` | TCP/IP network protocol stack (exposes `net` command) |
|  | `network/e1000` | Intel e1000 Gigabit NIC driver |
|  | `network/rtl8139` | Realtek RTL8139 Fast Ethernet driver |
|  | `network/rtl8169` | Realtek RTL8168/8111 Gigabit and RTL8125 2.5G Ethernet driver |
|  | `network/virtio` | Para-virtualized VirtIO network driver |
| **`ipc`** | `ipc/unix` | `AF_UNIX` stream sockets and device-event netlink |
|  | `ipc/pty` | Pseudo-terminals (`/dev/ptmx`, `/dev/pts/N`) for terminal emulators |
| **`sched`** | `sched/timer` | Timer interrupt for programs (after `modman takeover`): Ctrl-C stops busy loops, programs share the CPU |
| **`compat`** | `compat/linux` | Extra Linux system calls; loaded automatically on first use |
| **`debug`** | `debug/trace` | Low-level kernel tracing tool |
