/* compat/ia32: the calls of a 32-bit program.
 *
 * A 32-bit program calls with int 0x80: i386's number in EAX, its arguments
 * in EBX, ECX, EDX, ESI, EDI and EBP, and i386's structures behind its
 * pointers. Most calls are the 64-bit call under another number - its
 * pointers are addresses below 4 GiB, which mean the same to the kernel -
 * and go straight through. The rest have a structure that is laid out
 * differently, a 64-bit value split over two registers, or a 32-bit time:
 * those are converted, through room on the program's own stack below what
 * it uses, into the 64-bit call and back. Signals reach a 32-bit handler
 * through i386's frame, and come back through sigreturn.
 *
 * The kernel loads it with the first 32-bit program and drops it when the
 * last one is gone (ia32.h). */

#include "ia32.h"
#include "linux.h"
#include "module.h"
#include "string.h"
#include "syscall.h"
#include "thread.h"

typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;


/* Widened the way a C int is: -1 stays -1, AT_FDCWD stays AT_FDCWD. */
#define S(x) ((u64)(int64_t)(s32)(x))

static u64 run(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    return syscall_run(n, a, b, c, d, e, f);
}

/* ---- straight through ---------------------------------------------------
 *
 * i386's number, and the 64-bit one plus one: 0 is none. Every argument
 * is widened as an int; a pointer is below 2 GiB, which that leaves as it
 * is. */

#define NUMBERS 452

static const uint16_t straight[NUMBERS] = {
    [1] = 60 + 1,    [2] = 57 + 1,    [3] = 0 + 1,     [4] = 1 + 1,     [5] = 2 + 1,
    [6] = 3 + 1,     [8] = 85 + 1,    [9] = 86 + 1,    [10] = 87 + 1,   [12] = 80 + 1,
    [14] = 133 + 1,  [15] = 90 + 1,   [16] = 94 + 1,   [19] = 8 + 1,    [20] = 39 + 1,
    [21] = 165 + 1,  [23] = 105 + 1,  [24] = 102 + 1,  [27] = 37 + 1,   [29] = 34 + 1,
    [33] = 21 + 1,   [36] = 162 + 1,  [37] = 62 + 1,   [38] = 82 + 1,   [39] = 83 + 1,
    [40] = 84 + 1,   [41] = 32 + 1,   [42] = 22 + 1,   [45] = 12 + 1,   [46] = 106 + 1,
    [47] = 104 + 1,  [49] = 107 + 1,  [50] = 108 + 1,  [57] = 109 + 1,
    [60] = 95 + 1,   [61] = 161 + 1,  [63] = 33 + 1,   [64] = 110 + 1,  [65] = 111 + 1,
    [66] = 112 + 1,  [70] = 113 + 1,  [71] = 114 + 1,  [74] = 170 + 1,  [83] = 88 + 1,
    [85] = 89 + 1,   [88] = 169 + 1,  [91] = 11 + 1,   [92] = 76 + 1,   [93] = 77 + 1,
    [94] = 91 + 1,   [95] = 93 + 1,   [96] = 140 + 1,  [97] = 141 + 1,  [103] = 103 + 1,
    [118] = 74 + 1,  [122] = 63 + 1,  [125] = 10 + 1,  [132] = 121 + 1, [133] = 81 + 1,
    [136] = 135 + 1, [143] = 73 + 1,  [144] = 26 + 1,  [147] = 124 + 1, [148] = 75 + 1,
    [150] = 149 + 1, [151] = 150 + 1, [152] = 151 + 1, [153] = 152 + 1, [154] = 142 + 1,
    [155] = 143 + 1, [156] = 144 + 1, [157] = 145 + 1, [158] = 24 + 1,  [159] = 146 + 1,
    [160] = 147 + 1, [163] = 25 + 1,  [168] = 7 + 1,   [172] = 157 + 1, [175] = 14 + 1,
    [176] = 127 + 1, [178] = 129 + 1, [179] = 130 + 1, [182] = 92 + 1,  [183] = 79 + 1,
    [184] = 125 + 1, [185] = 126 + 1, [190] = 58 + 1,  [198] = 94 + 1,  [199] = 102 + 1,
    [200] = 104 + 1, [201] = 107 + 1, [202] = 108 + 1, [203] = 113 + 1, [204] = 114 + 1,
    [205] = 115 + 1, [206] = 116 + 1, [207] = 93 + 1,  [208] = 117 + 1, [209] = 118 + 1,
    [210] = 119 + 1, [211] = 120 + 1, [212] = 92 + 1,  [213] = 105 + 1, [214] = 106 + 1,
    [215] = 122 + 1, [216] = 123 + 1, [217] = 155 + 1, [218] = 27 + 1,  [219] = 28 + 1,
    [220] = 217 + 1, [224] = 186 + 1, [226] = 188 + 1, [227] = 189 + 1,
    [228] = 190 + 1, [229] = 191 + 1, [230] = 192 + 1, [231] = 193 + 1, [232] = 194 + 1,
    [233] = 195 + 1, [234] = 196 + 1, [235] = 197 + 1, [236] = 198 + 1, [237] = 199 + 1,
    [238] = 200 + 1, [239] = 40 + 1,  [241] = 203 + 1, [242] = 204 + 1, [252] = 231 + 1,
    [254] = 213 + 1, [255] = 233 + 1, [256] = 232 + 1, [258] = 218 + 1, [270] = 234 + 1,
    [289] = 251 + 1, [290] = 252 + 1, [291] = 253 + 1, [292] = 254 + 1, [293] = 255 + 1,
    [295] = 257 + 1, [296] = 258 + 1, [297] = 259 + 1, [298] = 260 + 1, [301] = 263 + 1,
    [302] = 264 + 1, [303] = 265 + 1, [304] = 266 + 1, [305] = 267 + 1, [306] = 268 + 1,
    [307] = 269 + 1, [310] = 272 + 1, [311] = 273 + 1, [313] = 275 + 1, [315] = 276 + 1,
    [318] = 309 + 1, [319] = 281 + 1, [321] = 282 + 1, [322] = 283 + 1, [323] = 284 + 1,
    [327] = 289 + 1, [328] = 290 + 1, [329] = 291 + 1, [330] = 292 + 1, [331] = 293 + 1,
    [332] = 294 + 1, [340] = 302 + 1, [344] = 306 + 1, [353] = 316 + 1, [355] = 318 + 1,
    [356] = 319 + 1, [359] = 41 + 1,  [360] = 53 + 1,  [361] = 49 + 1,  [362] = 42 + 1,
    [363] = 50 + 1,  [364] = 288 + 1, [365] = 55 + 1,  [366] = 54 + 1,  [367] = 51 + 1,
    [368] = 52 + 1,  [369] = 44 + 1,  [371] = 45 + 1,  [373] = 48 + 1,  [375] = 324 + 1,
    [376] = 325 + 1, [377] = 326 + 1, [383] = 332 + 1, [386] = 334 + 1, [403] = 228 + 1,
    [404] = 227 + 1, [406] = 229 + 1, [407] = 230 + 1, [408] = 224 + 1, [409] = 223 + 1,
    [410] = 287 + 1, [411] = 286 + 1, [412] = 280 + 1, [414] = 271 + 1, [421] = 128 + 1,
    [423] = 148 + 1, [424] = 424 + 1, [434] = 434 + 1, [436] = 436 + 1, [437] = 437 + 1,
    [439] = 439 + 1, [441] = 441 + 1,
};

/* ---- room on the program's stack --------------------------------------- */

struct call {
    struct user_regs *f;
    u64 below;                      /* what is taken of the stack so far */
};

/* n bytes under the program's stack, or 0 if they are not its. */
static u64 room(struct call *c, u64 n) {
    u64 at = (c->below - n) & ~15ull;

    if (n > 0x4000 || at < IA32_LOW || !user_range(at, n)) {
        return 0;
    }
    c->below = at;
    return at;
}

#define P32(x) ((u32 *)(u64)(x))
#define P64(x) ((u64 *)(u64)(x))

static bool ok32(u64 at, u64 n) {
    return at != 0 && user_range(at, n);
}

/* struct timespec or timeval, 32-bit, at from: into a 64-bit one at to. */
static void time_in(u64 from, u64 to) {
    P64(to)[0] = S(P32(from)[0]);
    P64(to)[1] = S(P32(from)[1]);
}

static void time_out(u64 from, u64 to) {
    P32(to)[0] = (u32)P64(from)[0];
    P32(to)[1] = (u32)P64(from)[1];
}

/* A 32-bit time at t, made 64-bit in room taken from c - or 0 for none,
   or ~0 if it could not be. */
static u64 time_arg(struct call *c, u32 t, u32 count) {
    u64 at;

    if (t == 0) {
        return 0;
    }
    if (!ok32(t, 8 * count) || (at = room(c, 16 * count)) == 0) {
        return ~0ull;
    }
    for (u32 i = 0; i < count; i++) {
        time_in(t + 8 * i, at + 16 * i);
    }
    return at;
}

/* iovec[count], 32-bit at v, into 64-bit room. */
#define IOVS 64

static u64 iov_arg(struct call *c, u32 v, u32 count) {
    u64 at;

    if (count > IOVS || !ok32(v, 8 * (u64)count) || (at = room(c, 16 * (u64)count + 16)) == 0) {
        return 0;
    }
    for (u32 i = 0; i < count; i++) {
        P64(at)[2 * i] = P32(v)[2 * i];
        P64(at)[2 * i + 1] = P32(v)[2 * i + 1];
    }
    return at;
}

/* ---- stat64 ------------------------------------------------------------- */

struct stat64_32 {
    u64 dev;
    u32 pad0, ino32, mode, nlink, uid, gid;
    u64 rdev;
    u32 pad3;
    u64 size;
    u32 blksize;
    u64 blocks;
    u32 atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    u64 ino;
} __attribute__((packed));

static void stat_out(u64 from, u32 to) {
    const u64 *s = P64(from);
    struct stat64_32 *d = (struct stat64_32 *)(u64)to;

    *d = (struct stat64_32){
        .dev = s[0], .ino32 = (u32)s[1], .ino = s[1], .nlink = (u32)s[2],
        .mode = ((u32 *)s)[6], .uid = ((u32 *)s)[7], .gid = ((u32 *)s)[8],
        .rdev = s[5], .size = s[6], .blksize = (u32)s[7], .blocks = s[8],
        .atime = (u32)s[9], .atime_ns = (u32)s[10], .mtime = (u32)s[11],
        .mtime_ns = (u32)s[12], .ctime = (u32)s[13], .ctime_ns = (u32)s[14],
    };
}

/* Call n, which fills a 64-bit struct stat at its argument which, made
   into stat64 at the program's pointer out. */
static u64 stat_call(struct call *c, u64 n, u64 a0, u64 a1, u64 a3, unsigned which, u32 out) {
    u64 at = room(c, 144), r;
    u64 args[4] = { a0, a1, 0, a3 };

    if (at == 0 || !ok32(out, sizeof(struct stat64_32))) {
        return ERR(EFAULT);
    }
    args[which] = at;
    if ((int64_t)(r = run(n, args[0], args[1], args[2], args[3], 0, 0)) == 0) {
        stat_out(at, out);
    }
    return r;
}

/* statfs, 64-bit at from, as i386's statfs (wide 0) or statfs64 at to. */
static void statfs_out(u64 from, u32 to, bool wide) {
    const u64 *s = P64(from);
    u32 *d = P32(to);

    if (!wide) {
        for (unsigned i = 0; i < 7; i++) {
            d[i] = (u32)s[i];
        }
        d[7] = ((u32 *)s)[14];      /* fsid */
        d[8] = ((u32 *)s)[15];
        for (unsigned i = 0; i < 7; i++) {
            d[9 + i] = (u32)s[8 + i];       /* namelen, frsize, flags, spare */
        }
        return;
    }
    d[0] = (u32)s[0];
    d[1] = (u32)s[1];
    memcpy(d + 2, s + 2, 5 * 8);    /* blocks .. ffree */
    d[12] = ((u32 *)s)[14];
    d[13] = ((u32 *)s)[15];
    for (unsigned i = 0; i < 7; i++) {
        d[14 + i] = (u32)s[8 + i];
    }
}

/* rusage: two times then fourteen counts, all longs. */
static void rusage_out(u64 from, u32 to) {
    for (unsigned i = 0; i < 18; i++) {
        P32(to)[i] = (u32)P64(from)[i];
    }
}

/* ---- messages ------------------------------------------------------------ */

#define CMSG32(n) (((n) + 3) & ~3u)
#define CMSG64(n) (((n) + 7) & ~7ull)

/* msghdr, 32-bit at m, into 64-bit room - its iovecs and control messages
   too, those with room for the answer when in is false. */
static u64 msg_arg(struct call *c, u32 m, bool send) {
    u32 *h = P32(m);
    u64 at, iov, ctl = 0, ctl_len = 0;

    if (!ok32(m, 28) || (at = room(c, 56)) == 0 || (iov = iov_arg(c, h[2], h[3])) == 0) {
        return 0;
    }
    if (h[4] != 0 && h[5] != 0) {
        if (!ok32(h[4], h[5]) || (ctl = room(c, 2 * (u64)h[5] + 64)) == 0) {
            return 0;
        }
        if (send) {
            for (u32 off = 0; off + 12 <= h[5];) {
                u32 *cm = P32(h[4] + off);
                u32 len = cm[0], data = len >= 12 ? len - 12 : 0;

                if (len < 12 || off + len > h[5]) {
                    break;
                }
                P64(ctl + ctl_len)[0] = 16 + data;
                P32(ctl + ctl_len)[2] = cm[1];
                P32(ctl + ctl_len)[3] = cm[2];
                memcpy((void *)(ctl + ctl_len + 16), cm + 3, data);
                ctl_len += CMSG64(16 + data);
                off += CMSG32(len);
            }
        } else {
            ctl_len = 2 * (u64)h[5] + 64;
        }
    }
    P64(at)[0] = h[0];
    P64(at)[1] = h[1];
    P64(at)[2] = iov;
    P64(at)[3] = h[3];
    P64(at)[4] = ctl;
    P64(at)[5] = ctl_len;
    P64(at)[6] = h[6];
    return at;
}

/* What recvmsg says back: the name's length, the flags, and the control
   messages, made 32-bit. */
static void msg_out(u64 at, u32 m) {
    u32 *h = P32(m);
    u64 ctl = P64(at)[4], len = P64(at)[5];
    u32 out = 0;

    h[1] = (u32)P64(at)[1];
    h[6] = (u32)P64(at)[6];
    for (u64 off = 0; ctl != 0 && off + 16 <= len;) {
        u64 cl = P64(ctl + off)[0], data = cl >= 16 ? cl - 16 : 0;

        if (cl < 16 || off + cl > len || out + CMSG32(12 + data) > h[5]) {
            break;
        }
        P32(h[4] + out)[0] = (u32)(12 + data);
        P32(h[4] + out)[1] = P32(ctl + off)[2];
        P32(h[4] + out)[2] = P32(ctl + off)[3];
        memcpy((void *)(u64)(h[4] + out + 12), (void *)(ctl + off + 16), data);
        out += CMSG32(12 + data);
        off += CMSG64(cl);
    }
    h[5] = out;
}

/* ---- the calls that are not straight through ------------------------------ */

#define CLONE_SETTLS 0x00080000
#define SA_SIGINFO   4

static u64 call32(struct call *c, u32 n, const u32 *a);

static u64 sigreturn(struct user_regs *f, bool rt);

/* execve's argv or envp: the 32-bit pointers, as 64-bit ones in room. */
static u64 strings_arg(struct call *c, u32 v) {
    u32 count = 0;
    u64 at;

    if (v == 0) {
        return 0;
    }
    while (count < 128 && ok32(v + 4 * count, 4) && P32(v)[count] != 0) {
        count++;
    }
    if ((at = room(c, 8 * (u64)count + 8)) == 0) {
        return ~0ull;
    }
    for (u32 i = 0; i < count; i++) {
        P64(at)[i] = P32(v)[i];
    }
    P64(at)[count] = 0;
    return at;
}

/* set_thread_area and clone's TLS: i386's struct user_desc, of which the
   base is what matters - the GDT entry it names is the one there is. */
static u64 thread_area(u32 desc) {
    u32 *d = P32(desc);

    if (!ok32(desc, 16)) {
        return ERR(EFAULT);
    }
    unsigned entry = tls_descriptor(d[1]);

    if (d[0] != (u32)-1 && d[0] != entry) {
        return ERR(EINVAL);
    }
    d[0] = entry;
    tls_gs_set(d[1]);
    return 0;
}

/* socketcall's numbers, in i386's own numbering of the direct calls. */
static const uint16_t socketcalls[21] = {
    0, 359, 361, 362, 363, 364, 367, 368, 360, 369, 371, 369, 371, 373, 366, 365, 370, 372, 364,
    337, 345,
};

static u64 special(struct call *c, u32 n, const u32 *a) {
    struct user_regs *f = c->f;
    u64 at = 0, at2, r;

    switch (n) {
    case 7:                         /* waitpid */
        return run(61, S(a[0]), a[1], S(a[2]), 0, 0, 0);
    case 55:                        /* fcntl */
    case 221:                       /* fcntl64: i386's F_GETLK64, F_SETLK64 and
                                       F_SETLKW64 are x86-64's F_GETLK.. - the
                                       lock's type first in either */
        return run(72, S(a[0]), a[1] >= 12 && a[1] <= 14 ? a[1] - 7 : a[1], a[2], 0, 0, 0);
    case 11: {                      /* execve */
        u64 argv = strings_arg(c, a[1]), envp = strings_arg(c, a[2]);

        if (argv == ~0ull || envp == ~0ull) {
            return ERR(EFAULT);
        }
        return run(59, a[0], argv, envp, 0, 0, 0);
    }
    case 13:                        /* time */
        r = run(201, 0, 0, 0, 0, 0, 0);
        if (a[0] != 0 && ok32(a[0], 4)) {
            *P32(a[0]) = (u32)r;
        }
        return r;
    case 43:                        /* times */
        if ((at = room(c, 32)) == 0) {
            return ERR(EFAULT);
        }
        r = run(100, a[0] != 0 ? at : 0, 0, 0, 0, 0, 0);
        if (a[0] != 0 && ok32(a[0], 16)) {
            for (unsigned i = 0; i < 4; i++) {
                P32(a[0])[i] = (u32)P64(at)[i];
            }
        }
        return r;
    case 54:                        /* ioctl: the request a number, not an int */
        return run(16, S(a[0]), a[1], a[2], 0, 0, 0);
    case 75:                        /* setrlimit */
        if ((at = room(c, 16)) == 0 || !ok32(a[1], 8)) {
            return ERR(EFAULT);
        }
        for (unsigned i = 0; i < 2; i++) {
            P64(at)[i] = P32(a[1])[i] == (u32)-1 ? ~0ull : P32(a[1])[i];
        }
        return run(160, a[0], at, 0, 0, 0, 0);
    case 76:                        /* getrlimit */
    case 191:                       /* ugetrlimit */
        if ((at = room(c, 16)) == 0 || !ok32(a[1], 8)) {
            return ERR(EFAULT);
        }
        if ((r = run(97, a[0], at, 0, 0, 0, 0)) == 0) {
            for (unsigned i = 0; i < 2; i++) {
                P32(a[1])[i] = P64(at)[i] > 0xFFFFFFFFull ? (u32)-1 : (u32)P64(at)[i];
            }
        }
        return r;
    case 77:                        /* getrusage */
        if ((at = room(c, 144)) == 0 || !ok32(a[1], 72)) {
            return ERR(EFAULT);
        }
        if ((r = run(98, S(a[0]), at, 0, 0, 0, 0)) == 0) {
            rusage_out(at, a[1]);
        }
        return r;
    case 78:                        /* gettimeofday */
        if ((at = room(c, 16)) == 0) {
            return ERR(EFAULT);
        }
        r = run(96, a[0] != 0 ? at : 0, a[1], 0, 0, 0, 0);
        if (r == 0 && a[0] != 0 && ok32(a[0], 8)) {
            time_out(at, a[0]);
        }
        return r;
    case 90: {                      /* mmap, its arguments in memory */
        const u32 *m = P32(a[0]);

        if (!ok32(a[0], 24)) {
            return ERR(EFAULT);
        }
        return run(9, m[0], m[1], m[2], m[3], S(m[4]), m[5]);
    }
    case 99:                        /* statfs */
    case 100:                       /* fstatfs */
    case 268:                       /* statfs64 */
    case 269: {                     /* fstatfs64 */
        bool wide = n >= 268;
        u32 out = wide ? a[2] : a[1];

        if ((at = room(c, 120)) == 0 || !ok32(out, wide ? 84 : 64)) {
            return ERR(EFAULT);
        }
        r = run(n == 99 || n == 268 ? 137 : 138, n == 99 || n == 268 ? a[0] : S(a[0]), at,
                0, 0, 0, 0);
        if (r == 0) {
            statfs_out(at, out, wide);
        }
        return r;
    }
    case 102: {                     /* socketcall */
        u32 args[6] = { 0 };

        if (a[0] == 0 || a[0] > 20 || !ok32(a[1], 6 * 4)) {
            return ERR(EINVAL);
        }
        memcpy(args, P32(a[1]), a[0] == 5 || a[0] == 9 || a[0] == 10 ? 16 : 24);
        if (a[0] == 5) {
            args[3] = 0;            /* accept: accept4 without flags */
        } else if (a[0] == 9 || a[0] == 10) {
            args[4] = args[5] = 0;  /* send, recv: sendto, recvfrom to nobody */
        }
        return call32(c, socketcalls[a[0]], args);
    }
    case 104:                       /* setitimer */
        if ((at = time_arg(c, a[1], 2)) == ~0ull || (at2 = room(c, 32)) == 0) {
            return ERR(EFAULT);
        }
        r = run(38, S(a[0]), at, a[2] != 0 ? at2 : 0, 0, 0, 0);
        if (r == 0 && a[2] != 0 && ok32(a[2], 16)) {
            time_out(at2, a[2]);
            time_out(at2 + 16, a[2] + 8);
        }
        return r;
    case 105:                       /* getitimer */
        if ((at = room(c, 32)) == 0 || !ok32(a[1], 16)) {
            return ERR(EFAULT);
        }
        if ((r = run(36, S(a[0]), at, 0, 0, 0, 0)) == 0) {
            time_out(at, a[1]);
            time_out(at + 16, a[1] + 8);
        }
        return r;
    case 114:                       /* wait4 */
        if ((at = room(c, 144)) == 0) {
            return ERR(EFAULT);
        }
        r = run(61, S(a[0]), a[1], S(a[2]), a[3] != 0 ? at : 0, 0, 0);
        if ((int64_t)r >= 0 && a[3] != 0 && ok32(a[3], 72)) {
            rusage_out(at, a[3]);
        }
        return r;
    case 116: {                     /* sysinfo */
        u32 *d = P32(a[0]);
        u64 unit;

        if ((at = room(c, 112)) == 0 || !ok32(a[0], 64)) {
            return ERR(EFAULT);
        }
        if ((r = run(99, at, 0, 0, 0, 0, 0)) != 0) {
            return r;
        }
        const u64 *s = P64(at);
        u64 mem = ((u32 *)s)[26] != 0 ? ((u32 *)s)[26] : 1;

        unit = s[4] * mem > 0xFFFFFFFFull ? 4096 : 1;   /* totalram, in what fits */
        memset(d, 0, 64);
        for (unsigned i = 0; i < 4; i++) {
            d[i] = (u32)s[i];       /* uptime, loads */
        }
        for (unsigned i = 4; i < 10; i++) {
            d[i] = (u32)(s[i] * mem / unit);
        }
        d[10] = ((u32 *)s)[20];     /* procs */
        d[13] = (u32)unit;
        return 0;
    }
    case 119:                       /* sigreturn */
        return sigreturn(f, false);
    case 173:                       /* rt_sigreturn */
        return sigreturn(f, true);
    case 120: {                     /* clone: flags, stack, parent_tid, tls, child_tid */
        u64 had = 0;
        bool tls = (a[0] & CLONE_SETTLS) != 0;

        if (tls) {
            u32 *d = P32(a[3]);

            if (!ok32(a[3], 16)) {
                return ERR(EFAULT);
            }
            /* The new thread takes its GS base from here as it is made. */
            had = tls_gs();
            tls_gs_set(d[1]);
        }
        r = run(56, a[0] & ~CLONE_SETTLS, a[1], a[2], a[4], 0, 0);
        if (tls) {
            tls_gs_set(had);
        }
        return r;
    }
    case 140:                       /* _llseek: fd, offset high, low, result, whence */
        if (!ok32(a[3], 8)) {
            return ERR(EFAULT);
        }
        r = run(8, S(a[0]), (u64)a[1] << 32 | a[2], a[4], 0, 0, 0);
        if ((int64_t)r < 0) {
            return r;
        }
        *P64(a[3]) = r;
        return 0;
    case 142:                       /* select */
        if ((at = time_arg(c, a[4], 1)) == ~0ull) {
            return ERR(EFAULT);
        }
        return run(23, S(a[0]), a[1], a[2], a[3], at, 0);
    case 145:                       /* readv */
    case 146:                       /* writev */
        if ((at = iov_arg(c, a[1], a[2])) == 0) {
            return ERR(a[2] > IOVS ? EINVAL : EFAULT);
        }
        return run(n == 145 ? 19 : 20, S(a[0]), at, a[2], 0, 0, 0);
    case 162:                       /* nanosleep */
    case 267: {                     /* clock_nanosleep */
        bool clk = n == 267;
        u32 req = a[clk ? 2 : 0], rem = a[clk ? 3 : 1];

        if ((at = time_arg(c, req, 1)) == ~0ull || (at2 = room(c, 16)) == 0) {
            return ERR(EFAULT);
        }
        r = clk ? run(230, S(a[0]), S(a[1]), at, rem != 0 ? at2 : 0, 0, 0)
                : run(35, at, rem != 0 ? at2 : 0, 0, 0, 0, 0);
        if (rem != 0 && ok32(rem, 8)) {
            time_out(at2, rem);
        }
        return r;
    }
    case 174: {                     /* rt_sigaction: handler, flags, restorer, mask */
        if ((at = room(c, 32)) == 0 || (at2 = room(c, 32)) == 0) {
            return ERR(EFAULT);
        }
        if (a[1] != 0) {
            if (!ok32(a[1], 20)) {
                return ERR(EFAULT);
            }
            P64(at)[0] = P32(a[1])[0];
            P64(at)[1] = P32(a[1])[1];
            P64(at)[2] = P32(a[1])[2];
            P64(at)[3] = *(u64 *)(u64)(a[1] + 12);
        }
        r = run(13, S(a[0]), a[1] != 0 ? at : 0, a[2] != 0 ? at2 : 0, a[3], 0, 0);
        if (r == 0 && a[2] != 0 && ok32(a[2], 20)) {
            P32(a[2])[0] = (u32)P64(at2)[0];
            P32(a[2])[1] = (u32)P64(at2)[1];
            P32(a[2])[2] = (u32)P64(at2)[2];
            *(u64 *)(u64)(a[2] + 12) = P64(at2)[3];
        }
        return r;
    }
    case 177:                       /* rt_sigtimedwait */
        if ((at = time_arg(c, a[2], 1)) == ~0ull) {
            return ERR(EFAULT);
        }
        return run(128, a[0], a[1], at, a[3], 0, 0);
    case 180:                       /* pread64 */
    case 181:                       /* pwrite64 */
        return run(n == 180 ? 17 : 18, S(a[0]), a[1], a[2], (u64)a[4] << 32 | a[3], 0, 0);
    case 186: {                     /* sigaltstack */
        if ((at = room(c, 24)) == 0 || (at2 = room(c, 24)) == 0) {
            return ERR(EFAULT);
        }
        if (a[0] != 0) {
            if (!ok32(a[0], 12)) {
                return ERR(EFAULT);
            }
            P64(at)[0] = P32(a[0])[0];
            P64(at)[1] = P32(a[0])[1];
            P64(at)[2] = P32(a[0])[2];
        }
        r = run(131, a[0] != 0 ? at : 0, a[1] != 0 ? at2 : 0, 0, 0, 0, 0);
        if (r == 0 && a[1] != 0 && ok32(a[1], 12)) {
            P32(a[1])[0] = (u32)P64(at2)[0];
            P32(a[1])[1] = (u32)P64(at2)[1];
            P32(a[1])[2] = (u32)P64(at2)[2];
        }
        return r;
    }
    case 187:                       /* sendfile, its offset 32-bit */
        if ((at = room(c, 8)) == 0) {
            return ERR(EFAULT);
        }
        if (a[2] != 0) {
            if (!ok32(a[2], 4)) {
                return ERR(EFAULT);
            }
            *P64(at) = S(*P32(a[2]));
        }
        r = run(40, S(a[0]), S(a[1]), a[2] != 0 ? at : 0, a[3], 0, 0);
        if (a[2] != 0) {
            *P32(a[2]) = (u32)*P64(at);
        }
        return r;
    case 192:                       /* mmap2: the offset in pages */
        return run(9, a[0], a[1], a[2], a[3], S(a[4]), (u64)a[5] << 12);
    case 193:                       /* truncate64 */
    case 194:                       /* ftruncate64 */
        return run(n == 193 ? 76 : 77, n == 193 ? a[0] : S(a[0]), (u64)a[2] << 32 | a[1],
                   0, 0, 0, 0);
    case 195:                       /* stat64 */
        return stat_call(c, 4, a[0], 0, 0, 1, a[1]);
    case 196:                       /* lstat64 */
        return stat_call(c, 6, a[0], 0, 0, 1, a[1]);
    case 197:                       /* fstat64 */
        return stat_call(c, 5, S(a[0]), 0, 0, 1, a[1]);
    case 300:                       /* fstatat64 */
        return stat_call(c, 262, S(a[0]), a[1], S(a[3]), 2, a[2]);
    case 243:                       /* set_thread_area */
        return thread_area(a[0]);
    case 244: {                     /* get_thread_area */
        u32 lo = (u32)tls_gs();

        if (!ok32(a[0], 16)) {
            return ERR(EFAULT);
        }
        P32(a[0])[0] = tls_descriptor(lo);
        P32(a[0])[1] = lo;
        P32(a[0])[2] = 0xFFFFF;
        P32(a[0])[3] = 0x51;        /* 32-bit, limit in pages, usable */
        return 0;
    }
    case 240: {                     /* futex, its timeout a 32-bit time */
        unsigned op = a[1] & 127;
        bool timed = op == 0 || op == 6 || op == 9 || op == 11;     /* WAIT, LOCK_PI,
                                                                       WAIT_BITSET, WAIT_REQUEUE_PI */

        if (timed && (at = time_arg(c, a[3], 1)) == ~0ull) {
            return ERR(EFAULT);
        }
        return run(202, a[0], a[1], a[2], timed ? at : a[3], a[4], a[5]);
    }
    case 250:                       /* fadvise64 */
    case 272:                       /* fadvise64_64 */
    case 314:                       /* sync_file_range */
        return 0;
    case 265:                       /* clock_gettime */
    case 266:                       /* clock_getres */
        if ((at = room(c, 16)) == 0) {
            return ERR(EFAULT);
        }
        r = run(n == 265 ? 228 : 229, S(a[0]), a[1] != 0 ? at : 0, 0, 0, 0, 0);
        if (r == 0 && a[1] != 0 && ok32(a[1], 8)) {
            time_out(at, a[1]);
        }
        return r;
    case 271:                       /* utimes */
        if ((at = time_arg(c, a[1], 2)) == ~0ull) {
            return ERR(EFAULT);
        }
        return run(235, a[0], at, 0, 0, 0, 0);
    case 320:                       /* utimensat */
        if ((at = time_arg(c, a[2], 2)) == ~0ull) {
            return ERR(EFAULT);
        }
        return run(280, S(a[0]), a[1], at, S(a[3]), 0, 0);
    case 308:                       /* pselect6 */
    case 413:                       /* pselect6_time64 */
        at = n == 308 ? time_arg(c, a[4], 1) : a[4];
        if (at == ~0ull) {
            return ERR(EFAULT);
        }
        at2 = 0;
        if (a[5] != 0) {            /* { sigset, its size }, the pair 32-bit */
            if (!ok32(a[5], 8) || (at2 = room(c, 16)) == 0) {
                return ERR(EFAULT);
            }
            P64(at2)[0] = P32(a[5])[0];
            P64(at2)[1] = P32(a[5])[1];
        }
        return run(270, S(a[0]), a[1], a[2], a[3], at, at2);
    case 309:                       /* ppoll */
        if ((at = time_arg(c, a[2], 1)) == ~0ull) {
            return ERR(EFAULT);
        }
        return run(271, a[0], a[1], at, a[3], a[4], 0);
    case 324:                       /* fallocate */
        return run(285, S(a[0]), S(a[1]), (u64)a[3] << 32 | a[2], (u64)a[5] << 32 | a[4], 0, 0);
    case 325:                       /* timerfd_settime */
        if ((at = time_arg(c, a[2], 2)) == ~0ull || (at2 = room(c, 32)) == 0) {
            return ERR(EFAULT);
        }
        r = run(286, S(a[0]), S(a[1]), at, a[3] != 0 ? at2 : 0, 0, 0);
        if (r == 0 && a[3] != 0 && ok32(a[3], 16)) {
            time_out(at2, a[3]);
            time_out(at2 + 16, a[3] + 8);
        }
        return r;
    case 326:                       /* timerfd_gettime */
        if ((at = room(c, 32)) == 0 || !ok32(a[1], 16)) {
            return ERR(EFAULT);
        }
        if ((r = run(287, S(a[0]), at, 0, 0, 0, 0)) == 0) {
            time_out(at, a[1]);
            time_out(at + 16, a[1] + 8);
        }
        return r;
    case 333:                       /* preadv */
    case 334:                       /* pwritev */
        if ((at = iov_arg(c, a[1], a[2])) == 0) {
            return ERR(a[2] > IOVS ? EINVAL : EFAULT);
        }
        return run(n == 333 ? 295 : 296, S(a[0]), at, a[2], (u64)a[4] << 32 | a[3], 0, 0);
    case 370:                       /* sendmsg */
    case 372:                       /* recvmsg */
        if ((at = msg_arg(c, a[1], n == 370)) == 0) {
            return ERR(EFAULT);
        }
        r = run(n == 370 ? 46 : 47, S(a[0]), at, a[2], 0, 0, 0);
        if (n == 372 && (int64_t)r >= 0) {
            msg_out(at, a[1]);
        }
        return r;
    case 422:                       /* futex_time64: the values numbers, not ints */
        return run(202, a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    return ERR(ENOSYS);
}

static u64 call32(struct call *c, u32 n, const u32 *a) {
    if (n < NUMBERS && straight[n] != 0) {
        return run(straight[n] - 1u, S(a[0]), S(a[1]), S(a[2]), S(a[3]), S(a[4]), S(a[5]));
    }
    return special(c, n, a);
}

static u64 ia32_syscall(struct user_regs *f) {
    u32 a[6] = { (u32)f->rbx, (u32)f->pad, (u32)f->rdx, (u32)f->rsi, (u32)f->rdi, (u32)f->rbp };
    struct call c = { f, (u32)f->rsp - 128 };

    return call32(&c, (u32)f->rax, a);
}

/* ---- signals ---------------------------------------------------------------
 *
 * i386's frames, as a libc and a debugger read them: sigcontext, then for
 * an SA_SIGINFO handler siginfo and a ucontext around the sigcontext. The
 * handler is called with the signal - and the two pointers - on the stack,
 * returning into the restorer, which is sigreturn or rt_sigreturn. */

struct sigcontext32 {
    uint16_t gs, gsh, fs, fsh, es, esh, ds, dsh;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax, trapno, err, eip;
    uint16_t cs, csh;
    u32 eflags, esp_at_signal;
    uint16_t ss, ssh;
    u32 fpstate, oldmask, cr2;
};

struct sigframe32 {
    u32 pretcode;
    s32 sig;
    struct sigcontext32 sc;
    uint8_t fpstate[624];
    u32 extramask;
    char retcode[8];
};

struct rt_sigframe32 {
    u32 pretcode;
    s32 sig;
    u32 pinfo, puc;
    s32 info[32];                   /* signo, errno, code, then the union */
    struct {
        u32 flags, link, ss_sp;
        s32 ss_flags;
        u32 ss_size;
        struct sigcontext32 sc;
        u32 mask[2];                /* i386 puts a u64 on a 4-byte boundary */
    } uc;
    char retcode[8];
};

static void context_out(struct sigcontext32 *sc, const struct ia32_signal *s) {
    *sc = (struct sigcontext32){
        .edi = s->edi, .esi = s->esi, .ebp = s->ebp, .esp = s->esp, .ebx = s->ebx,
        .edx = s->edx, .ecx = s->ecx, .eax = s->eax, .trapno = s->trapno, .err = s->err,
        .eip = s->eip, .eflags = s->eflags, .esp_at_signal = s->esp, .oldmask = (u32)s->mask,
        .cr2 = s->addr,
    };
}

static bool ia32_signal(struct ia32_signal *s) {
    bool rt = (s->flags & SA_SIGINFO) != 0;
    u64 size = rt ? sizeof(struct rt_sigframe32) : sizeof(struct sigframe32);
    u64 at = ((s->esp - size) & ~15ull) - 4;    /* as a call leaves it */

    if (at < IA32_LOW || !user_range(at, size)) {
        return false;
    }
    if (rt) {
        struct rt_sigframe32 *fr = (struct rt_sigframe32 *)at;

        memset(fr, 0, sizeof *fr);
        fr->pinfo = (u32)(u64)(uintptr_t)fr->info;
        fr->puc = (u32)(u64)(uintptr_t)&fr->uc;
        fr->info[0] = (s32)s->sig;
        fr->info[2] = (s32)s->code;
        if (s->pid != 0) {
            fr->info[3] = (s32)s->pid;
            fr->info[5] = (s32)s->status;
        } else {
            fr->info[3] = (s32)s->addr;
        }
        context_out(&fr->uc.sc, s);
        fr->uc.mask[0] = (u32)s->mask;
        fr->uc.mask[1] = (u32)(s->mask >> 32);
        s->edx = fr->pinfo;
        s->ecx = fr->puc;
    } else {
        struct sigframe32 *fr = (struct sigframe32 *)at;

        memset(fr, 0, sizeof *fr);
        context_out(&fr->sc, s);
        fr->extramask = (u32)(s->mask >> 32);
        s->edx = s->ecx = 0;
    }
    P32(at)[0] = s->restorer;
    P32(at)[1] = s->sig;
    s->eax = s->sig;
    s->esp = (u32)at;
    s->eip = s->handler;
    return true;
}

/* The handler has returned through its restorer: the registers and the
   mask the frame holds are the program's again. */
static u64 sigreturn(struct user_regs *f, bool rt) {
    u64 at = rt ? (u32)f->rsp - 4 : (u32)f->rsp - 8;   /* past what the handler's
                                       ret took, and old sigreturn's pop */
    const struct sigcontext32 *sc;
    u64 mask;

    if (!user_range(at, rt ? sizeof(struct rt_sigframe32) : sizeof(struct sigframe32))) {
        return ERR(EFAULT);
    }
    if (rt) {
        const struct rt_sigframe32 *fr = (const struct rt_sigframe32 *)at;

        sc = &fr->uc.sc;
        mask = fr->uc.mask[0] | (u64)fr->uc.mask[1] << 32;
    } else {
        const struct sigframe32 *fr = (const struct sigframe32 *)at;

        sc = &fr->sc;
        mask = sc->oldmask | (u64)fr->extramask << 32;
    }
    f->rbx = sc->ebx;
    f->pad = sc->ecx;
    f->rdx = sc->edx;
    f->rsi = sc->esi;
    f->rdi = sc->edi;
    f->rbp = sc->ebp;
    f->rsp = sc->esp;
    f->rip = sc->eip;
    f->rflags = (f->rflags & ~0x40DD5ull) | (sc->eflags & 0x40DD5);
    *thread_sigmask() = mask & ~(1ull << (9 - 1) | 1ull << (19 - 1));
    return sc->eax;
}

static const struct ia32_ops ops = { ia32_syscall, ia32_signal };

MODULE_EXPORT int module_init(void) {
    ia32_register(&ops);
    return 0;
}

MODULE_EXPORT int module_exit(void) {
    ia32_register(NULL);
    return 0;
}
