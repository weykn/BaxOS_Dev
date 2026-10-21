# Shell (`tsh`) Basics

Tuxlet boots into `tsh` (`/usr/bin/tsh`), a minimal shell that the kernel starts as its login shell. It runs `/etc/tuxlet/boot`, then `/etc/tuxlet/shell` if it exists, and otherwise gives you a prompt.

* **Built-in Commands:** `cd`, `export`, `exit`, `help`.
* **tuxlet-coreutils:** `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir [-p] [-m mode]` and `put` are programs in `/usr/bin`, replaced by the GNU ones when a package installs them.
* **Writing Files:** Use `put <file> [text]` to write a line of text.
* **Running Scripts:** `tsh <script>` runs a script's lines one by one, printing each as it runs.
* **Syntax:** A line is split into words on blanks, and `#` starts a comment. There are no pipes, redirections, variables or quoting; install `bash` for those.
* **TEF64:** `tsh` and the coreutils are TEF64 files built on TCR, Tuxlet's own executable format and C runtime. `tsh` is under 3 KB and takes one page of memory; each coreutil is under 1 KB. See [TEF64](tef64.md) and [TCR](tcr.md).

## Process Controls

| Key | Effect |
| --- | --- |
| `Ctrl + C` | Sends SIGINT to the foreground program. |
| `Ctrl + C` (3x repeatedly) | Kills a program that has stopped reading its keys. |
| `Ctrl + \` | Sends SIGQUIT. |

## Environment

A fresh image has no `PATH`, so only full paths work until you set one. The boot script normally does this:

```sh
export PATH=/ctl:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin
export MODPATH=/usr/lib/modules
```
