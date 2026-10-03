/* network/stack: the socket calls.
 *
 * socket, connect, sendmsg and the rest, and the ioctls ifconfig and route
 * use: everything a program does with a socket but read, write, poll and
 * close it, which the kernel does for every descriptor (net.h). A socket
 * descriptor holds the stack's number for it, and the two things Linux
 * keeps per descriptor rather than per connection: whether it blocks, and
 * how long a read waits. Nothing in the stack waits, so every wait is here,
 * on ready(), letting other threads run, and Ctrl-C ends it. */

#include <stddef.h>

#include "efi_kernel.h"
#include "net.h"
#include "string.h"
#include "syscall.h"
#include "thread.h"

extern const struct net_ops stack_ops;

#define ERR(e) ((uint64_t)-(int64_t)(e))
#define arg    (syscall_args())

#define EBADF           9
#define EAGAIN          11
#define EFAULT          14
#define EINVAL          22
#define ENOTTY          25
#define ENOTSOCK        88
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define FIONREAD        0x541B

struct iovec {
    uint64_t base;
    uint64_t length;
};

#define AF_UNSPEC     0
#define AF_INET       2
#define AF_NETLINK    16
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_NONBLOCK 0x800
#define KIND_NETLINK  0x100         /* with the type, in a socket's kind */
#define SOL_IP        0
#define IP_TTL        2
#define IP_RECVERR    11
#define SOL_SOCKET    1
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_RCVTIMEO   20
#define SO_RCVTIMEO_NEW 66
#define MSG_PEEK      0x02
#define MSG_CTRUNC    0x08
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_ERRQUEUE  0x2000
#define EINPROGRESS   115
#define ENOPROTOOPT   92

struct sockaddr_in {
    uint16_t family, port;
    uint32_t addr;
    uint8_t  zero[8];
};

struct msghdr {
    uint64_t name;
    uint32_t namelen, pad;
    uint64_t iov, iovlen;
    uint64_t control, controllen;
    uint32_t flags, pad2;
};

struct mmsghdr {
    struct msghdr hdr;
    uint32_t len, pad;
};

#define SOCK(h)    ((int)(h)->folder)
#define KIND(h)    ((h)->offset >> 16 & 0xF)
#define NETLINK(h) (((h)->offset >> 16 & KIND_NETLINK) != 0)

/* The socket behind fd, or NULL - which it also is once the module has
   gone, taking every socket with it. */
static struct handle *sock_handle(uint64_t fd) {
    struct handle *h = handle_of(fd);

    return h != NULL && h->start == SOCK_MARK ? h : NULL;
}

static bool blocking(const struct handle *h, uint64_t flags) {
    return (h->offset & O_NONBLOCK) == 0 && (flags & MSG_DONTWAIT) == 0;
}

/* Until the socket is ready for want, or has an error or has ended, or
   timeout_ms has gone by if it is not 0. */
static void sock_wait(const struct handle *h, unsigned want, uint64_t timeout_ms) {
    uint64_t until = efi_uptime_ms() + timeout_ms;
    uint64_t began = wait_began();

    while ((stack_ops.ready(SOCK(h)) & (want | NET_ERR | NET_HUP)) == 0 &&
           (timeout_ms == 0 || efi_uptime_ms() < until) && !interrupt_check()) {
        thread_yield();
    }
    wait_ended(began);
}

static uint64_t sock_give(int s, uint32_t kind, uint64_t flags) {
    uint64_t fd = give_handle((struct handle){
        .start = SOCK_MARK, .folder = (uint32_t)s,
        .offset = kind << 16 | ((flags & SOCK_NONBLOCK) != 0 ? O_NONBLOCK : 0) });

    if ((int64_t)fd < 0) {
        stack_ops.drop(s);
    }
    return fd;
}

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto) {
    uint32_t kind = type & 0xF;
    int p, s;

    if (domain != AF_INET && domain != AF_NETLINK) {
        return ERR(EAFNOSUPPORT);
    }
    if (domain == AF_NETLINK) {
        if (kind != SOCK_RAW && kind != SOCK_DGRAM) {
            return ERR(ESOCKTNOSUPPORT);
        }
        p = NET_NETLINK | (int)(proto & 0xFF);
        kind |= KIND_NETLINK;
    } else if (kind == SOCK_RAW) {
        if (proto == 0 || proto > 255) {
            return ERR(EPROTONOSUPPORT);
        }
        p = NET_RAW | (int)proto;
    } else if (kind == SOCK_STREAM && (proto == 0 || proto == NET_TCP)) {
        p = NET_TCP;
    } else if (kind == SOCK_DGRAM && (proto == 0 || proto == NET_UDP)) {
        p = NET_UDP;
    } else if (kind == SOCK_DGRAM && proto == NET_ICMP) {
        p = NET_ICMP;
    } else {
        return kind == SOCK_STREAM || kind == SOCK_DGRAM ? ERR(EPROTONOSUPPORT)
                                                         : ERR(ESOCKTNOSUPPORT);
    }
    if ((s = stack_ops.socket(p)) < 0) {
        return ERR(-s);
    }
    return sock_give(s, kind, type);
}

/* An address from the program, as ip and port. AF_UNSPEC is none. */
static uint64_t addr_in(uint64_t addr, uint64_t len, uint32_t *ip, uint16_t *port) {
    const struct sockaddr_in *a = (const struct sockaddr_in *)addr;

    *ip = 0;
    *port = 0;
    if (len < 8 || !user_range(addr, len)) {
        return ERR(EINVAL);
    }
    if (a->family == AF_UNSPEC) {
        return 0;
    }
    if (a->family != AF_INET) {
        return ERR(EAFNOSUPPORT);
    }
    *ip = a->addr;
    *port = a->port;
    return 0;
}

/* And one back, as much as the program left room for. A netlink socket's
   is a sockaddr_nl, the port id in ip. */
static void addr_out(const struct handle *h, uint64_t addr, uint64_t lenp, uint32_t ip,
                     uint16_t port) {
    struct sockaddr_in a = { AF_INET, port, ip, { 0 } };
    uint32_t nl[3] = { AF_NETLINK, ip, 0 };
    const void *from = &a;
    uint32_t room, size = sizeof a;

    if (addr == 0 || lenp == 0 || !user_range(lenp, 4)) {
        return;
    }
    if (NETLINK(h)) {
        from = nl;
        size = sizeof nl;
    }
    room = *(uint32_t *)lenp;
    if (room > size) {
        room = size;
    }
    if (user_range(addr, room)) {
        memcpy((void *)addr, from, room);
    }
    *(uint32_t *)lenp = size;
}

/* A datagram goes whole or not at all; a stream goes a piece at a time,
   waiting for room between, until all of it has. */
static uint64_t sock_send(struct handle *h, uint64_t buf, uint64_t len, uint64_t flags,
                          uint64_t addr, uint64_t alen) {
    uint32_t ip = 0;
    uint16_t port = 0;
    uint64_t err, done = 0;

    if (!user_range(buf, len)) {
        return ERR(EFAULT);
    }
    if (addr != 0 && KIND(h) != SOCK_STREAM && !NETLINK(h) &&
        (err = addr_in(addr, alen, &ip, &port)) != 0) {
        return err;
    }
    for (;;) {
        int64_t sent = stack_ops.send(SOCK(h), (const char *)buf + done, len - done, ip, port);

        if (sent >= 0) {
            done += (uint64_t)sent;
            if (done >= len || KIND(h) != SOCK_STREAM) {
                return done;
            }
            if (sent > 0) {
                continue;
            }
            sent = -EAGAIN;
        }
        if (sent != -EAGAIN || !blocking(h, flags)) {
            return done > 0 ? done : ERR(-sent);
        }
        sock_wait(h, NET_OUT, 0);
    }
}

/* A read of the error queue never waits: it is there or it is not. */
static uint64_t sock_recv(struct handle *h, uint64_t buf, uint64_t len, uint64_t flags,
                          uint64_t addr, uint64_t alenp, struct net_from *from,
                          uint32_t *msg_flags) {
    int64_t got;

    if (len > 0 && !user_range(buf, len)) {
        return ERR(EFAULT);
    }
    if (blocking(h, flags) && !(flags & MSG_ERRQUEUE)) {
        sock_wait(h, NET_IN, h->size);
    }
    got = stack_ops.recv(SOCK(h), (void *)buf, len, from,
                    ((flags & MSG_PEEK) ? NET_PEEK : 0) | ((flags & MSG_ERRQUEUE) ? NET_ERRQUEUE : 0));
    if (got < 0) {
        return ERR(-got);
    }
    addr_out(h, addr, alenp, from->ip, from->port);
    if (msg_flags != NULL) {
        *msg_flags = ((uint64_t)got > len ? MSG_TRUNC : 0) | (flags & MSG_ERRQUEUE);
    }
    return (flags & MSG_TRUNC) || (uint64_t)got < len ? (uint64_t)got : len;
}

static uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len) {
    struct handle *h = sock_handle(fd);
    uint32_t ip;
    uint16_t port;
    uint64_t err;
    int r;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    if (!NETLINK(h) && (err = addr_in(addr, len, &ip, &port)) != 0) {
        return err;
    }
    r = stack_ops.connect(SOCK(h), NETLINK(h) ? 0 : ip, NETLINK(h) ? 0 : port);
    if (r == -EINPROGRESS && blocking(h, 0)) {
        sock_wait(h, NET_OUT, 0);
        r = -stack_ops.error(SOCK(h));
    }
    return r < 0 ? ERR(-r) : 0;
}

static uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
    struct handle *h = sock_handle(fd);
    uint32_t ip;
    uint16_t port;
    uint64_t err;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    if (NETLINK(h)) {
        return 0;                   /* its port id is already its own */
    }
    if ((err = addr_in(addr, len, &ip, &port)) != 0) {
        return err;
    }
    int r = stack_ops.bind(SOCK(h), ip, port);

    return r < 0 ? ERR(-r) : 0;
}

static uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t c) {
    struct handle *h = sock_handle(fd);

    (void)c;
    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    int r = stack_ops.listen(SOCK(h), (int)backlog);

    return r < 0 ? ERR(-r) : 0;
}

static uint64_t sys_accept4(uint64_t fd, uint64_t addr, uint64_t lenp) {
    struct handle *h = sock_handle(fd);
    uint64_t flags = arg[3], made;
    uint32_t ip;
    uint16_t port;
    int s;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    while ((s = stack_ops.accept(SOCK(h))) == -EAGAIN && blocking(h, 0)) {
        sock_wait(h, NET_IN, 0);
    }
    if (s < 0) {
        return ERR(-s);
    }
    if ((int64_t)(made = sock_give(s, SOCK_STREAM, flags)) >= 0) {
        stack_ops.name(s, true, &ip, &port);
        addr_out(handle_of(made), addr, lenp, ip, port);
    }
    return made;
}

static uint64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t lenp) {
    arg[3] = 0;
    return sys_accept4(fd, addr, lenp);
}

static uint64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t c) {
    struct handle *h = sock_handle(fd);

    (void)c;
    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    int r = stack_ops.shutdown(SOCK(h), (int)how);

    return r < 0 ? ERR(-r) : 0;
}

static uint64_t sock_name(uint64_t fd, uint64_t addr, uint64_t lenp, bool peer) {
    struct handle *h = sock_handle(fd);
    uint32_t ip;
    uint16_t port;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    stack_ops.name(SOCK(h), peer, &ip, &port);
    addr_out(h, addr, lenp, ip, port);
    return 0;
}

static uint64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t lenp) {
    return sock_name(fd, addr, lenp, false);
}

static uint64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t lenp) {
    return sock_name(fd, addr, lenp, true);
}

static uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len) {
    struct handle *h = sock_handle(fd);

    return h == NULL ? ERR(ENOTSOCK) : sock_send(h, buf, len, arg[3], arg[4], arg[5]);
}

static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len) {
    struct handle *h = sock_handle(fd);
    struct net_from from;

    return h == NULL ? ERR(ENOTSOCK) : sock_recv(h, buf, len, arg[3], arg[4], arg[5], &from, NULL);
}

/* A message's data is its first buffer: what a datagram program hands
   over is one. */
static const struct iovec *first_iov(const struct msghdr *m) {
    static const struct iovec none;
    const struct iovec *v = (const struct iovec *)m->iov;

    if (m->iovlen == 0) {
        return &none;               /* nothing but the size is wanted */
    }
    return user_range(m->iov, sizeof *v) ? v : NULL;
}

static uint64_t msg_send(struct handle *h, struct msghdr *m, uint64_t flags) {
    const struct iovec *v = first_iov(m);

    return v == NULL ? ERR(EINVAL) : sock_send(h, v->base, v->length, flags, m->name, m->namelen);
}

static uint64_t sys_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags) {
    struct handle *h = sock_handle(fd);

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    return user_range(msg, sizeof(struct msghdr)) ? msg_send(h, (struct msghdr *)msg, flags)
                                                  : ERR(EFAULT);
}

static uint64_t sys_sendmmsg(uint64_t fd, uint64_t vec, uint64_t count) {
    struct mmsghdr *m = (struct mmsghdr *)vec;
    struct handle *h = sock_handle(fd);
    uint64_t flags = arg[3], i;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    if (!user_range(vec, count * sizeof *m)) {
        return ERR(EFAULT);
    }
    for (i = 0; i < count; i++) {
        uint64_t sent = msg_send(h, &m[i].hdr, flags);

        if ((int64_t)sent < 0) {
            return i > 0 ? i : sent;
        }
        m[i].len = (uint32_t)sent;
    }
    return i;
}

/* What comes with a datagram besides its data, as control messages: the
   TTL it arrived with, which is what ping shows, and for one off the error
   queue what went wrong and who said so. */
struct cmsg {
    uint64_t len;
    int32_t  level, type;
};

static bool cmsg_put(struct msghdr *m, uint64_t *used, int32_t type, const void *data,
                     uint32_t size) {
    uint64_t space = sizeof(struct cmsg) + ((size + 7) & ~7u);

    if (*used + space > m->controllen || !user_range(m->control + *used, space)) {
        m->flags |= MSG_CTRUNC;
        return false;
    }
    struct cmsg *c = (struct cmsg *)(m->control + *used);

    *c = (struct cmsg){ sizeof *c + size, SOL_IP, type };
    memcpy(c + 1, data, size);
    *used += space;
    return true;
}

static uint64_t sys_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags) {
    struct msghdr *m = (struct msghdr *)msg;
    struct handle *h = sock_handle(fd);
    const struct iovec *v;
    struct net_from from;
    uint64_t used = 0;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    if (!user_range(msg, sizeof *m) || (v = first_iov(m)) == NULL) {
        return ERR(EFAULT);
    }
    uint64_t got = sock_recv(h, v->base, v->length, flags, m->name,
                             m->name != 0 ? msg + offsetof(struct msghdr, namelen) : 0,
                             &from, &m->flags);

    if ((int64_t)got < 0) {
        return got;
    }
    if (from.err) {
        struct {
            uint32_t errnum;
            uint8_t  origin, type, code, pad;
            uint32_t info, data;
            struct sockaddr_in offender;
        } e = { from.errnum, from.origin, from.type, from.code, 0, from.info, 0,
                { AF_INET, 0, from.offender, { 0 } } };

        cmsg_put(m, &used, IP_RECVERR, &e, sizeof e);
    }
    if (KIND(h) != SOCK_STREAM && !NETLINK(h)) {
        int32_t ttl = from.ttl;

        cmsg_put(m, &used, IP_TTL, &ttl, sizeof ttl);
    }
    m->controllen = used;
    return got;
}

/* Options the kernel keeps itself, per descriptor; the rest are the
   network module's, and one it does not know is taken as read. */
static uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t name) {
    struct handle *h = sock_handle(fd);
    uint64_t value = arg[3], len = arg[4];
    uint32_t v = 0;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_RCVTIMEO_NEW)) {
        const int64_t *tv = (const int64_t *)value;

        if (!user_range(value, 16)) {
            return ERR(EFAULT);
        }
        h->size = (uint32_t)(tv[0] * 1000 + (tv[1] + 999) / 1000);
        return 0;
    }
    if (len > 0 && !user_range(value, len < 4 ? len : 4)) {
        return ERR(EFAULT);
    }
    if (len >= 4) {
        v = *(const uint32_t *)value;
    } else if (len >= 1) {
        v = *(const uint8_t *)value;
    }
    int r = stack_ops.setopt(SOCK(h), (int)level, (int)name, v);

    return r == -ENOPROTOOPT ? 0 : r < 0 ? ERR(-r) : 0;
}

static uint64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t name) {
    struct handle *h = sock_handle(fd);
    uint64_t value = arg[3], lenp = arg[4];
    uint32_t v = 0;

    if (h == NULL) {
        return ERR(ENOTSOCK);
    }
    if (!user_range(lenp, 4) || *(uint32_t *)lenp < 4 || !user_range(value, 4)) {
        return ERR(EINVAL);
    }
    if (level == SOL_SOCKET) {
        v = name == SO_TYPE ? KIND(h)
          : name == SO_ERROR ? (uint32_t)stack_ops.error(SOCK(h))
          : name == SO_SNDBUF || name == SO_RCVBUF ? 16384 : 0;
    } else {
        int r = stack_ops.getopt(SOCK(h), (int)level, (int)name, &v);

        if (r < 0 && r != -ENOPROTOOPT) {
            return ERR(-r);
        }
    }
    *(uint32_t *)value = v;
    *(uint32_t *)lenp = 4;
    return 0;
}

/* ---- interfaces, as ioctls on a socket ----------------------------------
 *
 * What ifconfig and route use: an interface named in an ifreq, and one
 * thing about it read or set. The network module has the interfaces. */

#define SIOCADDRT      0x890B
#define SIOCDELRT      0x890C
#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891A
#define SIOCGIFNETMASK 0x891B
#define SIOCSIFNETMASK 0x891C
#define SIOCGIFMETRIC  0x891D
#define SIOCSIFMETRIC  0x891E
#define SIOCGIFMTU     0x8921
#define SIOCSIFMTU     0x8922
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCGIFTXQLEN  0x8942
#define SIOCGIFMAP     0x8970
#define ENODEV         19
#define EOPNOTSUPP     95
#define EADDRNOTAVAIL  99
#define RTF_GATEWAY    0x2

struct ifreq {
    char name[16];
    union {
        struct sockaddr_in addr;
        struct {
            uint16_t family;
            uint8_t  data[14];
        } hw;
        int16_t  flags;
        int32_t  value;             /* index, MTU, metric, queue length */
        uint8_t  map[24];
    };
};

/* The interface an ifreq names. */
static unsigned iface_named(const char *name, struct net_iface *f) {
    for (unsigned i = 1; stack_ops.iface(i, f); i++) {
        if (strcmp(f->name, name) == 0) {
            return i;
        }
    }
    return 0;
}

uint64_t sock_ioctl(struct handle *h, uint64_t request, uint64_t out) {
    struct net_iface f;
    struct ifreq *r = (struct ifreq *)out;
    unsigned index;
    int err = 0;

    if (request == FIONREAD) {
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        *(int32_t *)out = (int32_t)stack_ops.pending(SOCK(h));
        return 0;
    }
    if (request == SIOCGIFCONF) {
        struct { int32_t len, pad; uint64_t buf; } *c = (void *)out;
        unsigned n = 0;

        if (!user_range(out, sizeof *c)) {
            return ERR(EFAULT);
        }
        for (unsigned i = 1; stack_ops.iface(i, &f); i++) {
            struct ifreq *to = (struct ifreq *)c->buf + n;

            if (f.ip == 0) {
                continue;
            }
            if (c->buf != 0) {
                if ((n + 1) * sizeof *to > (uint32_t)c->len || !user_range((uint64_t)to, sizeof *to)) {
                    break;
                }
                memset(to, 0, sizeof *to);
                strcpy(to->name, f.name);
                to->addr = (struct sockaddr_in){ AF_INET, 0, f.ip, { 0 } };
            }
            n++;
        }
        c->len = (int32_t)(n * sizeof(struct ifreq));
        return 0;
    }
    if (request == SIOCADDRT || request == SIOCDELRT) {
        /* struct rtentry: a pad, then the destination, gateway and mask,
           then the flags. Only the default route is there to change. */
        const uint8_t *rt = (const uint8_t *)out;
        const struct sockaddr_in *dst = (const void *)(rt + 8), *via = (const void *)(rt + 24);

        if (!user_range(out, 64)) {
            return ERR(EFAULT);
        }
        if (dst->addr != 0) {
            return ERR(EOPNOTSUPP);
        }
        err = stack_ops.iface_set(2, NET_SET_GATEWAY,
                             request == SIOCADDRT && (*(const uint16_t *)(rt + 56) & RTF_GATEWAY)
                             ? via->addr : 0);
        return err < 0 ? ERR(-err) : 0;
    }
    if (request < 0x8910 || request > 0x8980) {
        return ERR(ENOTTY);
    }
    if (!user_range(out, sizeof *r)) {
        return ERR(EFAULT);
    }
    if (request == SIOCGIFNAME) {
        if (r->value <= 0 || !stack_ops.iface((unsigned)r->value, &f)) {
            return ERR(ENODEV);
        }
        strcpy(r->name, f.name);
        return 0;
    }
    r->name[15] = '\0';
    if ((index = iface_named(r->name, &f)) == 0) {
        return ERR(ENODEV);
    }
    switch (request) {
    case SIOCGIFFLAGS:
        r->flags = (int16_t)f.flags;
        break;
    case SIOCSIFFLAGS:
        err = stack_ops.iface_set(index, NET_SET_FLAGS, (uint16_t)r->flags);
        break;
    case SIOCGIFADDR:
    case SIOCGIFDSTADDR:
    case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
        if (f.ip == 0) {
            return ERR(EADDRNOTAVAIL);
        }
        r->addr = (struct sockaddr_in){ AF_INET, 0,
            request == SIOCGIFADDR ? f.ip : request == SIOCGIFNETMASK ? f.mask :
            request == SIOCGIFBRDADDR ? f.broadcast : 0, { 0 } };
        break;
    case SIOCSIFADDR:
        err = stack_ops.iface_set(index, NET_SET_IP, r->addr.addr);
        break;
    case SIOCSIFNETMASK:
        err = stack_ops.iface_set(index, NET_SET_MASK, r->addr.addr);
        break;
    case SIOCSIFBRDADDR:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
        break;                      /* follows from the rest, or is fixed */
    case SIOCGIFMETRIC:
        r->value = 0;
        break;
    case SIOCGIFMTU:
        r->value = (int32_t)f.mtu;
        break;
    case SIOCGIFINDEX:
        r->value = (int32_t)index;
        break;
    case SIOCGIFTXQLEN:
        r->value = 1000;
        break;
    case SIOCGIFHWADDR:
        memset(&r->hw, 0, sizeof r->hw);
        r->hw.family = f.type;
        memcpy(r->hw.data, f.mac, 6);
        break;
    case SIOCGIFMAP:
        memset(r->map, 0, sizeof r->map);
        break;
    default:
        return ERR(EOPNOTSUPP);
    }
    return err < 0 ? ERR(-err) : 0;
}

/* ---- what the kernel calls ---------------------------------------------------- */

uint64_t sock_read(struct handle *h, uint64_t buf, uint64_t count) {
    struct net_from from;

    return sock_recv(h, buf, count, 0, 0, 0, &from, NULL);
}

uint64_t sock_write(struct handle *h, uint64_t buf, uint64_t count) {
    return sock_send(h, buf, count, 0, 0, 0);
}

syscall_fn sock_syscall(uint64_t number) {
    switch (number) {
    case SYS_SOCKET:      return sys_socket;
    case SYS_CONNECT:     return sys_connect;
    case SYS_BIND:        return sys_bind;
    case SYS_LISTEN:      return sys_listen;
    case SYS_ACCEPT:      return sys_accept;
    case SYS_ACCEPT4:     return sys_accept4;
    case SYS_SHUTDOWN:    return sys_shutdown;
    case SYS_GETSOCKNAME: return sys_getsockname;
    case SYS_GETPEERNAME: return sys_getpeername;
    case SYS_SENDTO:      return sys_sendto;
    case SYS_RECVFROM:    return sys_recvfrom;
    case SYS_SENDMSG:     return sys_sendmsg;
    case SYS_SENDMMSG:    return sys_sendmmsg;
    case SYS_RECVMSG:     return sys_recvmsg;
    case SYS_SETSOCKOPT:  return sys_setsockopt;
    case SYS_GETSOCKOPT:  return sys_getsockopt;
    }
    return NULL;
}
