# TCR: Tuxlet C Runtime

TCR is a small C runtime that the core programs (`tsh` and tuxlet-coreutils) use in place of glibc or musl; everything else runs on glibc. Each program links it statically, and the linker drops any part the program does not use.

- **Start-up:** `_start` passes `argc`/`argv`/`envp` to `main` and exits with its result.
- **System calls:** `sys()` plus wrappers for the calls the core tools make (`read`, `write`, `open`, `stat`, `execve`, `wait4`, `getdents64`, `brk`, ...), `vfork()`, and `tcr_restorer` for signal handlers. Calls return what the kernel returns: a negated errno on failure.
- **Memory:** `malloc`, `free` and `realloc` over `brk`, using a first-fit free list.
- **Strings:** `strlen`, `strcmp`, `strchr`, `memcpy`, `memset`.
- **Output:** `out(fd, ...)` writes a NULL-terminated list of strings, `puts` writes a line, and `utoa` formats a number.
- **Paths:** `base_of`, `is_folder`, `dest_of`.

It is deliberately minimal: no locale, no threads, no `FILE *` and no `printf`.
