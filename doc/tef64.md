# TEF64: Tuxlet Executable Format, 64-bit

TEF64 is the format of the preinstalled programs, `tsh` and tuxlet-coreutils; everything else, such as Debian's packages, is ordinary ELF64. It is small and simple:

- 64-bit only.
- A 12-byte header: a marker and how much memory the program needs; the program starts right after it.
- Static only: no dynamic linking, no relocations, no interpreter.
- No sections, segments, symbol tables or anything else that is not needed to run it, which keeps the files small. `tsh` is under 3 KB and each coreutil is under 1 KB.
- A tiny loader.
