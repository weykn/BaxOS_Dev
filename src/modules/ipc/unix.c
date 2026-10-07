/* ipc/unix: AF_UNIX stream sockets - how programs on one machine find each
 * other by name. An X server listens on /tmp/.X11-unix/X0 and every window
 * connects to it there; D-Bus, and plenty else, work the same way.
 *
 * A connection is a socketpair: the kernel's own two pipes, read, written,
 * polled and closed like any. What is here is only the meeting: a socket
 * made and bound to a name, listening; connect() makes a pair, hands one end
 * to the caller in place of its socket and queues the other for accept().
 * A name beginning with a NUL is Linux's abstract kind - no file - and any
 * other is a file, made by bind as Linux makes one.
 *
 * Nothing is kept but the sockets not yet connected, a few dozen bytes each,
 * and nothing at all while there are none.
 *
 * It also answers for the kernel's device events (NETLINK_KOBJECT_UEVENT),
 * which a udev library listens on to hear of devices coming and going.
 * Here none ever do, so it is a socket that is never ready: enough for a
 * program built for udev - an X server - to go on to find what is there. */

#include <stdbool.h>
#include <stddef.h>

#include "fs.h"
#include "linux.h"
#include "mem.h"
#include "module.h"
#include "net.h"
#include "string.h"
#include "syscall.h"
#include "thread.h"

#define EBUSY       16
#define EISCONN     106

#define AF_UNIX     1
#define AF_NETLINK  16
#define NETLINK_KOBJECT_UEVENT 15
#define SOCK_STREAM 1
#define SOCK_TYPE   0xF

#define SOCKETS 8                   /* not yet connected, at once */
#define BACKLOG 8                   /* connections waiting for accept */
#define PATH    108                 /* sockaddr_un's sun_path */

#define SOL_SOCKET  1
#define SO_TYPE     3
#define SO_ERROR    4
#define SO_SNDBUF   7
#define SO_RCVBUF   8
#define SO_PEERCRED 17

static struct sock {
    unsigned refs;                  /* descriptors on it; 0 when the slot is free */
    bool     listening;
    bool     uevent;                /* the kernel's device events: never anything */
    char     name[PATH + 1];        /* where it is bound, "" if nowhere; an
                                       abstract name keeps its NUL as '@' */
    struct handle queue[BACKLOG];   /* the ends accept() hands out */
    unsigned queued;
} *socks[SOCKETS];

struct sockaddr_un {
    uint16_t family;
    char     path[PATH];
};

struct msghdr {
    uint64_t name;
    uint32_t namelen, pad;
    uint64_t iov, iovlen;
    uint64_t control, controllen;
    int32_t  flags;
};

struct iovec {
    uint64_t base, len;
};

static struct sock *sock_of(struct handle *h) {
    unsigned n = h != NULL && h->start == SOCK_MARK && (h->offset & SOCK_UNIX) != 0 ? h->folder : 0;

    return n > 0 && n <= SOCKETS ? socks[n - 1] : NULL;
}

static bool is_pair(const struct handle *h) {
    return h != NULL && h->start == PIPE_MARK && h->size == PIPE_PAIR;
}

/* A sockaddr_un's name, as kept: NUL-terminated, '@' for the abstract
   kind's leading NUL. Empty if there is none. */
static bool name_of(uint64_t addr, uint64_t len, char *out) {
    const struct sockaddr_un *a = (const struct sockaddr_un *)addr;
    size_t n;

    if (len < 3 || len > sizeof *a || !user_range(addr, len) || a->family != AF_UNIX) {
        return false;
    }
    n = len - 2;
    memcpy(out, a->path, n);
    out[n] = '\0';
    if (out[0] == '\0') {
        out[0] = '@';
    }
    return true;
}

static struct sock *bound(const char *name) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i] != NULL && socks[i]->name[0] != '\0' && strcmp(socks[i]->name, name) == 0) {
            return socks[i];
        }
    }
    return NULL;
}

/* ---- what the kernel asks of a socket not yet connected ---------------- */

static void unix_hold(int s) {
    if (s > 0 && s <= SOCKETS && socks[s - 1] != NULL) {
        socks[s - 1]->refs++;
    }
}

static void unix_drop(int s) {
    struct sock *k = s > 0 && s <= SOCKETS ? socks[s - 1] : NULL;

    if (k == NULL || --k->refs > 0) {
        return;
    }
    for (unsigned i = 0; i < k->queued; i++) {
        handle_close(&k->queue[i]);     /* connected, and never answered */
    }
    mem_free(k);
    socks[s - 1] = NULL;
}

static unsigned unix_ready(int s) {
    struct sock *k = s > 0 && s <= SOCKETS ? socks[s - 1] : NULL;

    return k == NULL ? NET_ERR : k->queued > 0 ? NET_IN : 0;
}

/* ---- the calls ---------------------------------------------------------- */

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    unsigned i = 0;
    struct sock *k;
    uint64_t fd;

    (void)protocol;
    if (domain == AF_UNIX && (type & SOCK_TYPE) != SOCK_STREAM) {
        return ERR(EPROTONOSUPPORT);
    }
    while (i < SOCKETS && socks[i] != NULL) {
        i++;
    }
    if (i == SOCKETS || (k = mem_alloc(sizeof *k)) == NULL) {
        return ERR(ENFILE);
    }
    memset(k, 0, sizeof *k);
    k->refs = 1;
    k->uevent = domain == AF_NETLINK;
    socks[i] = k;
    fd = give_handle((struct handle){ .start = SOCK_MARK, .folder = i + 1,
                                      .offset = SOCK_UNIX | SOCK_STREAM << 16 |
                                                ((uint32_t)type & O_NONBLOCK) });
    if ((int64_t)fd < 0) {
        unix_drop((int)i + 1);
    } else if ((type & O_CLOEXEC) != 0) {
        fd_cloexec(fd, true);
    }
    return fd;
}

static uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
    struct sock *k = sock_of(handle_of(fd));
    char name[PATH + 1];
    struct fs_file file;

    if (k == NULL) {
        return ERR(EINVAL);
    }
    if (k->uevent) {
        return 0;                   /* to its groups: there is nothing to join */
    }
    if (!name_of(addr, len, name)) {
        return ERR(EINVAL);
    }
    if (bound(name) != NULL || (name[0] != '@' && fs_stat(name, &file) == 0)) {
        return ERR(EADDRINUSE);
    }
    if (name[0] != '@' && fs_write(name, NULL, 0) < 0) {
        return ERR(ENOENT);         /* the folder it is to go in is not there */
    }
    strcpy(k->name, name);
    return 0;
}

static uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t c) {
    struct sock *k = sock_of(handle_of(fd));

    (void)backlog;
    (void)c;
    if (k == NULL || k->name[0] == '\0') {
        return ERR(EINVAL);
    }
    k->listening = true;
    return 0;
}

/* The caller's socket becomes one end of a new pair, and the other end
   waits at the listener for accept. */
static uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len) {
    struct handle *h = handle_of(fd);
    struct sock *k = sock_of(h), *to;
    char name[PATH + 1];
    struct fs_file file;
    struct handle pair[2];

    if (k == NULL) {
        return is_pair(h) ? ERR(EISCONN) : ERR(ENOTSOCK);
    }
    if (!name_of(addr, len, name)) {
        return ERR(EINVAL);
    }
    if ((to = bound(name)) == NULL || !to->listening) {
        return name[0] != '@' && fs_stat(name, &file) != 0 ? ERR(ENOENT) : ERR(ECONNREFUSED);
    }
    if (to->queued == BACKLOG) {
        return ERR(EAGAIN);
    }
    if (!pair_new(pair, h->offset & O_NONBLOCK)) {
        return ERR(ENFILE);
    }
    to->queue[to->queued++] = pair[1];
    unix_drop((int)h->folder);      /* the socket it was is gone: it is an end now */
    *h = pair[0];
    return 0;
}

/* The oldest connection waiting, as a descriptor - waiting for one unless
   the socket says not to. */
static uint64_t accept_with(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t flags) {
    struct handle *h = handle_of(fd);
    struct sock *k = sock_of(h);
    struct handle end;
    uint64_t got;

    if (k == NULL || !k->listening) {
        return ERR(EINVAL);
    }
    while (k->queued == 0) {
        if ((h->offset & O_NONBLOCK) != 0) {
            return ERR(EAGAIN);
        }
        if (interrupt_check()) {
            return ERR(EINTR);
        }
        thread_yield();
    }
    end = k->queue[0];
    end.offset = (end.offset & ~(uint32_t)O_NONBLOCK) | ((uint32_t)flags & O_NONBLOCK);
    got = give_handle(end);
    if ((int64_t)got < 0) {
        return got;                 /* it stays queued */
    }
    k->queued--;
    memmove(k->queue, k->queue + 1, k->queued * sizeof k->queue[0]);
    if ((flags & O_CLOEXEC) != 0) {
        fd_cloexec(got, true);
    }
    if (addr != 0 && lenp != 0 && user_range(lenp, 4) && *(uint32_t *)lenp >= 2 &&
        user_range(addr, 2)) {
        *(uint16_t *)addr = AF_UNIX;    /* the other end has no name */
        *(uint32_t *)lenp = 2;
    }
    return got;
}

static uint64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t lenp) {
    return accept_with(fd, addr, lenp, 0);
}

static uint64_t sys_accept4(uint64_t fd, uint64_t addr, uint64_t lenp) {
    return accept_with(fd, addr, lenp, syscall_args()[3]);
}

/* A socket's own name, or - peer - the other end's. A connected end knows
   neither, and says so as Linux does for an unnamed one: the family alone. */
static uint64_t name_call(uint64_t fd, uint64_t addr, uint64_t lenp, bool peer) {
    struct handle *h = handle_of(fd);
    struct sock *k = sock_of(h);
    struct sockaddr_un a = { .family = AF_UNIX };
    uint32_t len = 2;

    if (!user_range(lenp, 4) || !user_range(addr, *(uint32_t *)lenp)) {
        return ERR(EFAULT);
    }
    if (k != NULL && k->uevent) {
        /* sockaddr_nl: the family, and the caller's pid as its port. */
        uint32_t nl[3] = { AF_NETLINK, peer ? 0 : (uint32_t)thread_id(), 0 };

        memcpy((void *)addr, nl, *(uint32_t *)lenp < 12 ? *(uint32_t *)lenp : 12);
        *(uint32_t *)lenp = 12;
        return 0;
    }
    if (peer && k != NULL) {
        return ERR(ENOTCONN);
    }
    if (k != NULL && k->name[0] != '\0') {
        len = (uint32_t)strlen(k->name);
        memcpy(a.path, k->name, len);
        if (a.path[0] == '@') {
            a.path[0] = '\0';
        } else {
            len++;                  /* a path's NUL is counted */
        }
        len += 2;
    }
    memcpy((void *)addr, &a, len < *(uint32_t *)lenp ? len : *(uint32_t *)lenp);
    *(uint32_t *)lenp = len;
    return 0;
}

static uint64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t lenp) {
    return name_call(fd, addr, lenp, false);
}

static uint64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t lenp) {
    return name_call(fd, addr, lenp, true);
}

#define SO_PASSCRED 16
#define SCM_CREDENTIALS 2
#define SOCK_SEQPACKET 5

static uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t name) {
    uint64_t value = syscall_args()[3];

    /* Who sends, with each message: Chromium's zygote learns its children's
       pids this way. Everything else here has nothing to tune. */
    if (level == SOL_SOCKET && name == SO_PASSCRED && user_range(value, 4)) {
        pair_flags(fd, *(const int32_t *)value != 0);
    }
    return 0;
}

static uint64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t name) {
    uint64_t out = syscall_args()[3], lenp = syscall_args()[4];
    uint32_t value[3] = { 0 };
    uint32_t size = 4;

    (void)fd;
    if (!user_range(lenp, 4) || !user_range(out, *(uint32_t *)lenp)) {
        return ERR(EFAULT);
    }
    if (level != SOL_SOCKET) {
        return ERR(EOPNOTSUPP);
    }
    switch (name) {
    case SO_TYPE:
        value[0] = (pair_flags(fd, -1) & 2) != 0 ? SOCK_SEQPACKET : SOCK_STREAM;
        break;
    case SO_SNDBUF:
    case SO_RCVBUF:
        value[0] = 65536;
        break;
    case SO_PEERCRED:               /* pid, uid, gid: everyone is root */
        value[0] = (uint32_t)thread_id();
        size = 12;
        break;
    default:                        /* SO_ERROR and the rest: nothing to say */
        break;
    }
    if (size > *(uint32_t *)lenp) {
        size = *(uint32_t *)lenp;
    }
    memcpy((void *)out, value, size);
    *(uint32_t *)lenp = size;
    return 0;
}

static uint64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t c) {
    (void)how;
    (void)c;
    return handle_of(fd) != NULL ? 0 : ERR(EBADF);
}

/* sendmsg and recvmsg: the iovecs, written or read through the pair, and
   the descriptors in the control part (SCM_RIGHTS) passed with them - the
   kernel holds them until they are read. Credentials are not passed. */
#define SCM_RIGHTS       1
#define MSG_CTRUNC       0x8
#define MSG_TRUNC        0x20
#define MSG_DONTWAIT     0x40
#define MSG_CMSG_CLOEXEC 0x40000000
#define PASS_FDS         16

struct cmsghdr {
    uint64_t len;
    int32_t  level, type;
};

static uint64_t sys_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags) {
    const struct msghdr *m = (const struct msghdr *)msg;
    struct handle fds[PASS_FDS];
    uint32_t nfd = 0;
    uint64_t result;

    if (!user_range(msg, sizeof *m) || !user_range(m->iov, m->iovlen * sizeof(struct iovec)) ||
        (m->controllen != 0 && !user_range(m->control, m->controllen))) {
        return ERR(EFAULT);
    }
    for (uint64_t at = 0; m->control != 0 && at + sizeof(struct cmsghdr) <= m->controllen;) {
        const struct cmsghdr *c = (const struct cmsghdr *)(m->control + at);

        if (c->len < sizeof *c || at + c->len > m->controllen) {
            break;
        }
        if (c->level == SOL_SOCKET && c->type == SCM_RIGHTS) {
            const int32_t *given = (const int32_t *)(c + 1);

            for (uint64_t i = 0; i < (c->len - sizeof *c) / 4; i++) {
                if (nfd == PASS_FDS || !handle_share((uint64_t)(uint32_t)given[i], &fds[nfd])) {
                    while (nfd > 0) {
                        handle_close(&fds[--nfd]);
                    }
                    return ERR(nfd == PASS_FDS ? EINVAL : EBADF);
                }
                nfd++;
            }
        }
        at += (c->len + 7) & ~7ull;
    }
    result = pair_send(fd, m->iov, m->iovlen, fds, nfd, (flags & MSG_DONTWAIT) != 0);
    if ((int64_t)result < 0) {
        while (nfd > 0) {
            handle_close(&fds[--nfd]);
        }
    }
    return result;
}

static uint64_t sys_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags) {
    struct msghdr *m = (struct msghdr *)msg;
    struct handle *h = handle_of(fd);
    uint64_t done = 0;

    if (sock_of(h) != NULL && sock_of(h)->uevent) {
        return ERR(EAGAIN);         /* no device has come or gone */
    }
    struct handle fds[PASS_FDS];
    uint32_t nfd = 0;
    bool cut = (flags & MSG_DONTWAIT) != 0;

    if (!user_range(msg, sizeof *m)) {
        return ERR(EFAULT);
    }
    done = pair_recv(fd, m->iov, m->iovlen, fds, &nfd, &cut);
    if ((int64_t)done < 0) {
        return done;
    }
    m->flags = cut ? MSG_TRUNC : 0;
    m->namelen = 0;
    uint64_t room = m->control != 0 && user_range(m->control, m->controllen) ? m->controllen : 0;
    uint64_t need = sizeof(struct cmsghdr) + 4 * (uint64_t)nfd;

    /* Who sent it, where the reader asked (SO_PASSCRED): after any
       descriptors, as its own message. */
    uint64_t creds = (pair_flags(fd, -1) & 1) != 0 ? sizeof(struct cmsghdr) + 12 : 0;
    uint64_t rights = nfd != 0 ? (need + 7) & ~7ull : 0;

    if (creds != 0 && room >= rights + creds) {
        struct cmsghdr *c = (struct cmsghdr *)(m->control + rights);
        uint32_t *who = (uint32_t *)(c + 1);

        c->len = creds;
        c->level = SOL_SOCKET;
        c->type = SCM_CREDENTIALS;
        who[0] = (uint32_t)pair_sender;
        who[1] = who[2] = 0;        /* root */
    } else {
        creds = 0;
    }
    if (nfd == 0) {
        m->controllen = creds;
    } else if (room < need) {
        while (nfd > 0) {
            handle_close(&fds[--nfd]);      /* no room for them: they are lost */
        }
        m->flags |= MSG_CTRUNC;
        m->controllen = 0;
    } else {
        struct cmsghdr *c = (struct cmsghdr *)m->control;
        int32_t *got = (int32_t *)(c + 1);

        c->len = need;
        c->level = SOL_SOCKET;
        c->type = SCM_RIGHTS;
        for (uint32_t i = 0; i < nfd; i++) {
            uint64_t made = give_handle(fds[i]);

            if ((int64_t)made < 0) {
                handle_close(&fds[i]);
                got[i] = -1;
                continue;
            }
            if ((flags & MSG_CMSG_CLOEXEC) != 0) {
                fd_cloexec(made, true);
            }
            got[i] = (int32_t)made;
        }
        m->controllen = rights + ((creds + 7) & ~7ull);
    }
    return done;
}

static syscall_fn unix_syscall(uint64_t number, uint64_t first) {
    struct handle *h;

    if (number == SYS_SOCKET) {
        return first == AF_UNIX ||
               (first == AF_NETLINK && syscall_args()[2] == NETLINK_KOBJECT_UEVENT) ? sys_socket : NULL;
    }
    h = handle_of(first);
    if (sock_of(h) == NULL && !is_pair(h)) {
        return NULL;                /* not an AF_UNIX socket: someone else's call */
    }
    switch (number) {
    case SYS_BIND:        return sys_bind;
    case SYS_LISTEN:      return sys_listen;
    case SYS_CONNECT:     return sys_connect;
    case SYS_ACCEPT:      return sys_accept;
    case SYS_ACCEPT4:     return sys_accept4;
    case SYS_GETSOCKNAME: return sys_getsockname;
    case SYS_GETPEERNAME: return sys_getpeername;
    case SYS_SETSOCKOPT:  return sys_setsockopt;
    case SYS_GETSOCKOPT:  return sys_getsockopt;
    case SYS_SHUTDOWN:    return sys_shutdown;
    case SYS_SENDMSG:     return sys_sendmsg;
    case SYS_RECVMSG:     return sys_recvmsg;
    default:              return NULL;
    }
}

static const struct unix_ops ops = { unix_syscall, unix_hold, unix_drop, unix_ready };

MODULE_EXPORT int module_init(void) {
    unix_register(&ops);
    return 0;
}

/* A socket still made, bound or listening is a program's: it stays. */
MODULE_EXPORT int module_exit(void) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i] != NULL) {
            return -EBUSY;
        }
    }
    unix_register(NULL);
    return 0;
}
