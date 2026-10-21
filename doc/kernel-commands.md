# Kernel Commands

The kernel's own commands live in `/ctl`. They are not files on the disk: running one runs it inside the kernel, and its output can still be piped like any program's.

| Command | Usage | Description |
| --- | --- | --- |
| `mem` | `mem usage\|all\|top\|peak <program>` | `usage` shows the OS's own memory use. `all` adds the firmware's share; `top` lists every process, biggest first; `peak` runs a program and, when it ends, prints the most memory it (with its children) and the whole system had in use. |
| `mode` / `scale` / `font` | `mode [size]`, `scale [size\|off]`, `font [size]` | Sets the screen resolution, console scaling and font size. Alone, each lists its choices. `mode` only works before `modman takeover`. |
| `clear` / `echo` | `echo [text]` | Clears the screen, or prints text with `{bold}`, `{n}` *(newline)* and `{uptime}` markup. |
| `greet` | `greet [text]` | Sets the welcome line, with the same markup as `echo`; alone, prints it. |
| `modman` | `modman [enable\|disable <module>\|auto <category>\|takeover]` | Loads and unloads modules. Alone, it lists them. See [Kernel Modules](modules.md). |
| `reboot` / `poweroff` | | Writes out pending disk data, then restarts or powers off. |

Modules can add commands of their own: `cache` (`storage/cache`) and `net` (`network/stack`). Make sure `/ctl` is on your `PATH` (see [Shell](shell.md#environment)).
