/* network/stack: the network stack.
 *
 * IPv4 with ARP, ICMP, UDP and TCP, DHCP to find an address, and the sockets
 * the kernel's syscalls stand on (net.h): TCP, UDP, ICMP echo, raw IP, and
 * netlink - rtnetlink for the interfaces, addresses, routes and neighbours,
 * sock_diag for the sockets - with /proc/net's files besides. The card is
 * whichever a driver module has registered with the kernel (net_card); the
 * stack loads and runs without one, looping back to itself alone. There is
 * no interrupt and no thread: nothing arrives unless something is looking,
 * so everything happens in net_poll, which every wait for a socket goes
 * round - TCP's timers included.
 *
 * There are two interfaces, as Linux numbers them: 1 is lo, 2 is eth0, the
 * card. Addresses and ports are kept as they go on the wire, in network
 * order. */

#include "debug.h"
#include "efi_kernel.h"
#include "fs.h"
#include "module.h"
#include "mem.h"
#include "net.h"
#include "proc.h"
#include "string.h"
#include "vga.h"

#define EPERM        1
#define ESRCH        3
#define EAGAIN       11
#define ENOMEM       12
#define EBUSY        16
#define ENODEV       19
#define EINVAL       22
#define ENFILE       23
#define EPIPE        32
#define EPROTO       71
#define EDESTADDRREQ 89
#define EMSGSIZE     90
#define ENOPROTOOPT  92
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define EADDRNOTAVAIL 99
#define EADDRINUSE   98
#define ENETUNREACH  101
#define ECONNRESET   104
#define EISCONN      106
#define ENOTCONN     107
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EHOSTDOWN    112
#define EHOSTUNREACH 113
#define EALREADY     114
#define EINPROGRESS  115

static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

#define IP(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define BROADCAST 0xFFFFFFFFu

static bool loopback(uint32_t ip) {
    return (ip & 0xFF) == 127;
}

static uint64_t now(void) {
    return efi_uptime_ms();
}

/* ---- the card, and the one page the stack sends from ------------------- */

static const struct net_card *card;
static bool started;                /* card->start has been, and worked */
static uint8_t *arp_buf, *loop_buf, *tx_buf;
static uint32_t my_ip, mask, gateway, dns;
static enum { NO_CARD, NO_ADDRESS, UP } state;
static bool down;                   /* eth0 taken down (ip link, ifconfig) */
static bool manual;                 /* its address was set by hand: no DHCP */
static uint16_t ip_id;

/* What went through each interface, for ip -s and /proc/net/dev. */
static struct counts {
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes, rx_dropped;
} lo_counts, eth_counts;
static bool polling;                /* inside net_poll: nothing may wait */
static const uint8_t *hop_src;      /* who handed over the frame being read;
                                       NULL for one that looped back */

/* ---- headers ----------------------------------------------------------- */

struct eth {
    uint8_t  dst[6], src[6];
    uint16_t type;
} __attribute__((packed));

struct arp {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip {
    uint8_t  vhl, tos;
    uint16_t len, id, frag;
    uint8_t  ttl, proto;
    uint16_t sum;
    uint32_t src, dst;
} __attribute__((packed));

struct udp {
    uint16_t sport, dport, len, sum;
};

struct icmp {
    uint8_t  type, code;
    uint16_t sum, id, seq;
};

struct tcp {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off, flags;
    uint16_t win, sum, urg;
};

#define ETH_IP  0x0800
#define ETH_ARP 0x0806
#define HEADERS (sizeof(struct eth) + sizeof(struct ip))
#define MTU     1500

static const uint8_t everyone[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint32_t sum_add(uint32_t sum, const void *data, size_t n) {
    const uint8_t *p = data;

    for (; n > 1; n -= 2, p += 2) {
        sum += (uint32_t)p[0] << 8 | p[1];
    }
    if (n) {
        sum += (uint32_t)p[0] << 8;
    }
    return sum;
}

static uint16_t sum_end(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return be16((uint16_t)~sum);
}

static uint32_t source_for(uint32_t ip) {
    return loopback(ip) ? ip : my_ip;
}

/* The checksum UDP and TCP share: the segment, and the addresses around it. */
static uint16_t sum_over(uint32_t dst, uint8_t proto, const void *seg, size_t len) {
    uint32_t src = source_for(dst);
    uint32_t sum = sum_add(sum_add(0, &src, 4), &dst, 4) + proto + (uint32_t)len;

    return sum_end(sum_add(sum, seg, len));
}

/* ---- ARP ------------------------------------------------------------------ */

#define ARPS 4

static struct {
    uint32_t ip;
    uint8_t  mac[6];
} arps[ARPS];
static unsigned arp_next;

void net_poll(void);

static const uint8_t *arp_find(uint32_t ip) {
    for (unsigned i = 0; i < ARPS; i++) {
        if (arps[i].ip == ip) {
            return arps[i].mac;
        }
    }
    return NULL;
}

static void arp_learn(uint32_t ip, const uint8_t *m) {
    uint8_t *slot = (uint8_t *)arp_find(ip);

    if (slot == NULL) {
        arps[arp_next].ip = ip;
        slot = arps[arp_next].mac;
        arp_next = (arp_next + 1) % ARPS;
    }
    memcpy(slot, m, 6);
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa) {
    static const uint8_t nobody[6];
    struct eth *e = (struct eth *)arp_buf;
    struct arp *a = (struct arp *)(e + 1);

    memcpy(e->dst, op == 1 ? everyone : tha, 6);
    memcpy(e->src, card->mac, 6);
    e->type = be16(ETH_ARP);
    *a = (struct arp){ be16(1), be16(ETH_IP), 6, 4, be16(op), { 0 }, my_ip, { 0 }, tpa };
    memcpy(a->sha, card->mac, 6);
    memcpy(a->tha, op == 1 ? nobody : tha, 6);
    card->send(arp_buf, sizeof *e + sizeof *a);
    eth_counts.tx_packets++;
    eth_counts.tx_bytes += sizeof *e + sizeof *a;
}

static bool arp_resolve(uint32_t ip, uint8_t out[6]) {
    for (unsigned tries = 0; tries < 3; tries++) {
        const uint8_t *m = arp_find(ip);

        if (m == NULL) {
            arp_send(1, NULL, ip);
            if (polling) {
                return false;       /* the answer comes next time round */
            }
            for (uint64_t until = now() + 300; (m = arp_find(ip)) == NULL && now() < until;) {
                net_poll();
            }
        }
        if (m != NULL) {
            memcpy(out, m, 6);
            return true;
        }
    }
    return false;
}

static void arp_input(const struct arp *a, size_t len) {
    if (len < sizeof *a || a->ptype != be16(ETH_IP) || my_ip == 0 || a->tpa != my_ip) {
        return;
    }
    arp_learn(a->spa, a->sha);
    if (a->op == be16(1)) {
        arp_send(2, a->sha, a->spa);
    }
}

/* ---- IP ------------------------------------------------------------------- */

/* Where the next hop is: its hardware address in out, or loop set when the
   packet never leaves the machine. */
static int route(uint32_t ip, uint8_t out[6], bool *loop) {
    *loop = loopback(ip) || (ip == my_ip && ip != 0);
    if (*loop) {
        return 0;
    }
    if (state != UP || down) {
        return -ENETUNREACH;
    }
    if (ip == BROADCAST || ip == (my_ip | ~mask)) {
        memcpy(out, everyone, 6);
        return 0;
    }
    uint32_t hop = ((ip ^ my_ip) & mask) != 0 ? gateway : ip;

    if (hop == 0) {
        return -ENETUNREACH;
    }
    return arp_resolve(hop, out) ? 0 : -EHOSTUNREACH;
}

/* The payload goes here, before ip_send is called. */
static uint8_t *payload(void) {
    return tx_buf + HEADERS;
}

static void ip_input(const uint8_t *p, size_t len);

/* Sends the packet whose header is at the start of tx_buf's IP part, all
   but its checksum filled in. One looping back is read at once, which may
   send a reply of its own from here before this returns: whoever calls this
   must be done with the packet it is answering. */
static void ip_out(struct ip *h, const uint8_t *to, bool loop) {
    struct eth *e = (struct eth *)tx_buf;
    size_t total = be16(h->len);

    h->sum = 0;
    h->sum = sum_end(sum_add(0, h, (h->vhl & 15) * 4u));
    if (loop) {
        const uint8_t *was = hop_src;

        lo_counts.tx_packets++;
        lo_counts.tx_bytes += total;
        lo_counts.rx_packets++;
        lo_counts.rx_bytes += total;
        memcpy(loop_buf, h, total);
        hop_src = NULL;
        ip_input(loop_buf, total);
        hop_src = was;
        return;
    }
    if (!started || down) {
        return;
    }
    memcpy(e->dst, to, 6);
    memcpy(e->src, card->mac, 6);
    e->type = be16(ETH_IP);
    card->send(tx_buf, sizeof *e + total);
    eth_counts.tx_packets++;
    eth_counts.tx_bytes += sizeof *e + total;
}

/* Sends what is at payload(), behind a header of its own. */
static void ip_send(uint32_t dst, const uint8_t *to, bool loop, uint8_t proto, size_t len,
                    uint8_t ttl) {
    struct ip *h = (struct ip *)(tx_buf + sizeof(struct eth));

    *h = (struct ip){ 0x45, 0, be16((uint16_t)(sizeof *h + len)), be16(ip_id++), 0,
                      ttl, proto, 0, source_for(dst), dst };
    ip_out(h, to, loop);
}

/* ---- sockets ----------------------------------------------------------------
 *
 * A datagram socket's datagrams wait in a page, one after another, each
 * behind who it came from; one that does not fit is dropped, as a full
 * socket buffer drops it on Linux. A TCP socket has a ring for what came in
 * and a buffer for what is going out, kept until the far end acknowledges
 * it. */

#define SOCKETS  32
#define DGRAM_Q  4096
#define RBUF     16384
#define SBUF     8192
#define WINDOW   (6 * 1460)         /* in flight to us at once: what a card's
                                       receive ring holds, with room over */
#define RTO      1000
#define LINGER   200                /* ms a close waits for its FIN to land */

enum {
    CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT1, FIN_WAIT2,
    CLOSE_WAIT, CLOSING, LAST_ACK,
};

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

/* A socket's proto besides NET_TCP, NET_UDP and NET_ICMP (an echo socket). */
#define SK_RAW     250              /* ipproto is its IP protocol */
#define SK_NETLINK 251              /* ipproto is its netlink protocol */

struct dgram {
    uint16_t size, port;
    uint32_t ip;
    uint32_t ttl;
};

/* One on the error queue, before what came back of the packet. */
struct errq {
    uint16_t size, port;
    uint8_t  origin, type, code, ttl;
    uint32_t ip, errnum, info, offender;
};

static struct sock {
    uint8_t  proto;                 /* 0 in a free slot */
    uint8_t  ipproto;
    uint8_t  ttl;                   /* its packets'; 0 for 64 */
    bool     recverr, hdrincl;
    uint32_t icmp_filter;           /* ICMP types a raw socket does not want */
    uint8_t *errq;                  /* IP_RECVERR's queue, once there is one */
    uint32_t errq_len;
    uint8_t  refs;                  /* descriptors; 0 once only the stack holds it */
    uint8_t  state;
    uint8_t  error;                 /* errno for the program, not yet told */
    uint8_t  parent;                /* not yet accepted: its listener, one-based */
    uint8_t  backlog;
    uint8_t  tries, dups;
    bool     loop, fin_in, fin_out, fin_sent;
    uint8_t  hop[6];                /* where its packets go first */
    uint16_t port, peer_port, mss;
    uint32_t peer_ip;
    uint8_t *in;                    /* datagrams, or the ring from in_head */
    uint32_t in_len, in_head;
    uint8_t *out;                   /* from snd_una on */
    uint32_t out_len;
    uint32_t snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint64_t timer;                 /* when to send again, 0 for never */
    uint32_t rto;
} socks[SOCKETS];

static struct sock *sock(int s) {
    return &socks[s - 1];
}

static int number(const struct sock *k) {
    return (int)(k - socks) + 1;
}

/* What its incoming queue holds: a TCP stream, or a netlink dump at once. */
static uint32_t in_size(const struct sock *k) {
    return k->proto == NET_TCP || k->proto == SK_NETLINK ? RBUF : DGRAM_Q;
}

static void sock_free(struct sock *k) {
    if (k->in != NULL) {
        mem_pages_free((uint64_t)k->in, in_size(k) / 4096);
    }
    if (k->out != NULL) {
        mem_pages_free((uint64_t)k->out, SBUF / 4096);
    }
    if (k->errq != NULL) {
        mem_pages_free((uint64_t)k->errq, 1);
    }
    *k = (struct sock){ 0 };
}

static struct sock *sock_new(int proto) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct sock *k = &socks[i];

        if (k->proto != 0) {
            continue;
        }
        *k = (struct sock){ .proto = (uint8_t)proto, .mss = 536, .rto = RTO };
        k->in = (uint8_t *)mem_pages(in_size(k) / 4096);
        if (proto == NET_TCP) {
            k->out = (uint8_t *)mem_pages(SBUF / 4096);
        }
        if (k->in == NULL || (proto == NET_TCP && k->out == NULL)) {
            sock_free(k);
            return NULL;
        }
        return k;
    }
    return NULL;
}

static bool port_taken(uint8_t proto, uint16_t port) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i].proto == proto && socks[i].port == port && socks[i].parent == 0) {
            return true;
        }
    }
    return false;
}

static void autobind(struct sock *k) {
    static uint16_t next;

    if (next == 0) {
        next = (uint16_t)(49152 + efi_uptime_us() % 16000);
    }
    while (k->port == 0) {
        uint16_t port = be16(next);

        next = next == 65535 ? 49152 : next + 1;
        if (!port_taken(k->proto, port)) {
            k->port = port;
        }
    }
}

static void deliver(struct sock *k, const struct ip *h, uint16_t port, const void *data,
                    size_t size) {
    struct dgram *d = (struct dgram *)(k->in + k->in_len);

    if (k->in_len + sizeof *d + size > in_size(k)) {
        return;
    }
    *d = (struct dgram){ (uint16_t)size, port, h->src, h->ttl };
    memcpy(d + 1, data, size);
    k->in_len += sizeof *d + (uint32_t)size;
}

static uint8_t ttl_of(const struct sock *k) {
    return k->ttl != 0 ? k->ttl : 64;
}

/* ---- ICMP ------------------------------------------------------------------ */

/* An error about a packet that came from here, sent to where it came from:
   the ICMP header, then its IP header and the first eight bytes after. */
static void icmp_send_error(const struct ip *h, uint8_t type, uint8_t code) {
    size_t head = (h->vhl & 15) * 4u, quote = head + 8;
    uint8_t to[6];
    bool loop;

    if (quote > be16(h->len) || h->dst == BROADCAST || route(h->src, to, &loop) < 0) {
        return;
    }
    struct icmp *m = (struct icmp *)payload();

    *m = (struct icmp){ type, code, 0, 0, 0 };
    memcpy(m + 1, h, quote);
    m->sum = sum_end(sum_add(0, m, sizeof *m + quote));
    ip_send(h->src, to, loop, NET_ICMP, sizeof *m + quote, 64);
}

/* The errno an ICMP error means, as Linux has them. */
static int icmp_errno(uint8_t type, uint8_t code) {
    static const uint8_t unreach[] = {
        ENETUNREACH, EHOSTUNREACH, ENOPROTOOPT, ECONNREFUSED, EMSGSIZE, EOPNOTSUPP,
        ENETUNREACH, EHOSTDOWN, ENETUNREACH, ENETUNREACH, EHOSTUNREACH, ENETUNREACH,
        EHOSTUNREACH, EHOSTUNREACH,
    };

    if (type == 3) {
        return code < sizeof unreach ? unreach[code] : EHOSTUNREACH;
    }
    return type == 12 ? EPROTO : EHOSTUNREACH;
}

/* Onto the socket's error queue, for recvmsg(MSG_ERRQUEUE). */
static void errq_push(struct sock *k, const struct errq *e, const void *data, size_t size) {
    if (k->errq == NULL && (k->errq = (uint8_t *)mem_pages(1)) == NULL) {
        return;
    }
    if (k->errq_len + sizeof *e + size > 4096) {
        return;
    }
    memcpy(k->errq + k->errq_len, e, sizeof *e);
    ((struct errq *)(k->errq + k->errq_len))->size = (uint16_t)size;
    memcpy(k->errq + k->errq_len + sizeof *e, data, size);
    k->errq_len += sizeof *e + (uint32_t)size;
}

/* An ICMP error, told to the socket whose packet it was about. */
static void sock_error(struct sock *k, const struct ip *h, const struct icmp *m, uint32_t dst,
                       uint16_t dport, const void *data, size_t size) {
    int err = icmp_errno(m->type, m->code);

    if (k->recverr) {
        struct errq e = {
            0, dport, 2 /* SO_EE_ORIGIN_ICMP */, m->type, m->code, h->ttl, dst, (uint32_t)err,
            m->type == 3 && m->code == 4 ? be16(m->seq) : 0, h->src,
        };

        errq_push(k, &e, data, size);
        k->error = (uint8_t)err;
    } else if (k->peer_ip != 0 && m->type == 3 && (m->code == 2 || m->code == 3)) {
        k->error = (uint8_t)err;
    }
}

static void tcp_closed(struct sock *k, int err);

/* Destination unreachable, time exceeded, a parameter problem: it quotes
   the packet it is about, which says whose it was. */
static void icmp_error(const struct ip *h, const uint8_t *p, size_t len) {
    const struct icmp *m = (const struct icmp *)p;
    const struct ip *in = (const struct ip *)(p + sizeof *m);
    size_t head;

    if (len < sizeof *m + sizeof *in || (head = (in->vhl & 15) * 4u) < sizeof *in ||
        len < sizeof *m + head + 8) {
        return;
    }
    const uint8_t *t = p + sizeof *m + head;
    size_t rest = len - sizeof *m - head;
    uint16_t sport, dport;

    memcpy(&sport, t, 2);
    memcpy(&dport, t + 2, 2);
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct sock *k = &socks[i];

        if (in->proto == NET_UDP && k->proto == NET_UDP && k->port == sport &&
            (k->peer_ip == 0 || (k->peer_ip == in->dst && k->peer_port == dport))) {
            sock_error(k, h, m, in->dst, dport, t + 8, rest - 8);
        } else if (in->proto == NET_ICMP && k->proto == NET_ICMP &&
                   k->port == ((const struct icmp *)t)->id) {
            sock_error(k, h, m, in->dst, 0, t, rest);
        } else if (in->proto == NET_TCP && k->proto == NET_TCP && k->port == sport &&
                   k->peer_ip == in->dst && k->peer_port == dport && k->state == SYN_SENT &&
                   m->type == 3 && (m->code == 2 || m->code == 3)) {
            tcp_closed(k, ECONNREFUSED);
        }
    }
}

static void icmp_input(const struct ip *h, const uint8_t *p, size_t len) {
    const struct icmp *m = (const struct icmp *)p;

    if (len < sizeof *m) {
        return;
    }
    if (m->type == 8) {             /* an echo request: answered */
        uint8_t to[6];
        bool loop;

        if (len > MTU - sizeof *h || route(h->src, to, &loop) < 0) {
            return;
        }
        struct icmp *r = (struct icmp *)payload();

        memmove(r, m, len);
        r->type = 0;
        r->sum = 0;
        r->sum = sum_end(sum_add(0, r, len));
        ip_send(h->src, to, loop, NET_ICMP, len, 64);
    } else if (m->type == 0) {      /* a reply, for the socket it answers */
        for (unsigned i = 0; i < SOCKETS; i++) {
            if (socks[i].proto == NET_ICMP && socks[i].port == m->id) {
                deliver(&socks[i], h, 0, p, len);
            }
        }
    } else if (m->type == 3 || m->type == 11 || m->type == 12) {
        icmp_error(h, p, len);
    }
}

/* ---- TCP ----------------------------------------------------------------------
 *
 * Enough of it to talk to anything: the handshake both ways, data both ways
 * with the far end's window respected, retransmission after a timeout or
 * three duplicate acknowledgements, and closing from either side. What it
 * leaves out is what a fast link needs rather than what a working one does:
 * no window scaling, no selective acknowledgements, no congestion control
 * beyond the window, and a segment out of order is dropped, to be sent
 * again. */

static bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static uint16_t window(const struct sock *k) {
    uint32_t room = RBUF - k->in_len;

    return (uint16_t)(room < WINDOW ? room : WINDOW);
}

/* One segment: flags, at seq, with len bytes of data. */
static void tcp_send(struct sock *k, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len) {
    struct tcp *t = (struct tcp *)payload();
    size_t head = sizeof *t + ((flags & SYN) ? 4 : 0);

    if (!k->loop && !started) {
        return;
    }
    *t = (struct tcp){ k->port, k->peer_port, be32(seq), be32(k->rcv_nxt),
                       (uint8_t)(head / 4 << 4), flags, be16(window(k)), 0, 0 };
    if (flags & SYN) {
        uint8_t *o = (uint8_t *)(t + 1);

        o[0] = 2;                   /* the largest segment we take */
        o[1] = 4;
        o[2] = 1460 >> 8;
        o[3] = 1460 & 0xFF;
    }
    memcpy((uint8_t *)t + head, data, len);
    t->sum = sum_over(k->peer_ip, NET_TCP, t, head + len);
    ip_send(k->peer_ip, k->hop, k->loop, NET_TCP, head + len, ttl_of(k));
}

/* A reset for a segment nothing here wanted. */
static void tcp_refuse(uint32_t src, uint16_t sport, uint16_t dport, uint32_t seq,
                       uint32_t ack, uint8_t flags, uint32_t dlen) {
    struct tcp *t = (struct tcp *)payload();

    *t = (struct tcp){ dport, sport, 0, 0, sizeof *t / 4 << 4, RST, 0, 0, 0 };
    if (flags & ACK) {
        t->seq = be32(ack);
    } else {
        t->ack = be32(seq + dlen + !!(flags & SYN) + !!(flags & FIN));
        t->flags |= ACK;
    }
    t->sum = sum_over(src, NET_TCP, t, sizeof *t);
    ip_send(src, hop_src, hop_src == NULL, NET_TCP, sizeof *t, 64);
}

/* It is over. A socket no program holds any more goes with it. */
static void tcp_closed(struct sock *k, int err) {
    k->state = CLOSED;
    k->timer = 0;
    if (err != 0 && k->error == 0) {
        k->error = (uint8_t)err;
    }
    if (k->refs == 0) {
        sock_free(k);
    }
}

static void tcp_abort(struct sock *k, int err) {
    if (k->state >= SYN_RCVD) {
        tcp_send(k, RST | ACK, k->snd_nxt, NULL, 0);
    }
    tcp_closed(k, err);
}

static void arm(struct sock *k) {
    if (k->timer == 0) {
        k->timer = now() + k->rto;
    }
}

/* Sends what the window allows of what is waiting, then the FIN once that
   is all gone - and, with ack, an acknowledgement if nothing else carried
   one. Every send may be answered before it returns (looping back), so the
   socket is looked at afresh each time round. */
static void tcp_push(struct sock *k, bool ack) {
    while (k->proto == NET_TCP && !k->fin_sent &&
           (k->state == ESTABLISHED || k->state == CLOSE_WAIT || k->state == FIN_WAIT1 ||
            k->state == CLOSING || k->state == LAST_ACK)) {
        uint32_t sent = k->snd_nxt - k->snd_una;
        uint32_t room = k->snd_wnd > sent ? k->snd_wnd - sent : 0;
        uint32_t n = k->out_len - sent;
        uint32_t seq = k->snd_nxt;

        if (n > room) {
            n = room;
        }
        if (n > k->mss) {
            n = k->mss;
        }
        if (n > 0) {
            k->snd_nxt += n;
            arm(k);
            ack = false;
            tcp_send(k, ACK | PSH, seq, k->out + sent, n);
            continue;
        }
        if (k->fin_out && sent == k->out_len) {
            k->snd_nxt++;
            k->fin_sent = true;
            if (k->state == ESTABLISHED) {
                k->state = FIN_WAIT1;
            } else if (k->state == CLOSE_WAIT) {
                k->state = LAST_ACK;
            }
            arm(k);
            ack = false;
            tcp_send(k, FIN | ACK, seq, NULL, 0);
        }
        break;
    }
    if (ack && k->proto == NET_TCP && k->state != CLOSED) {
        tcp_send(k, ACK, k->snd_nxt, NULL, 0);
    }
}

/* Its timer ran out: whatever is unacknowledged goes again, and after too
   many times the connection is given up. */
static void tcp_timeout(struct sock *k) {
    if (k->state == FIN_WAIT2) {
        tcp_closed(k, 0);           /* the far end never closed its half */
        return;
    }
    if (++k->tries > 8) {
        tcp_abort(k, ETIMEDOUT);
        return;
    }
    k->rto = k->rto < 30000 ? k->rto * 2 : 60000;
    k->timer = now() + k->rto;
    if (k->state == SYN_SENT) {
        tcp_send(k, SYN, k->snd_una, NULL, 0);
    } else if (k->state == SYN_RCVD) {
        tcp_send(k, SYN | ACK, k->snd_una, NULL, 0);
    } else {
        k->snd_nxt = k->snd_una;
        k->fin_sent = false;
        if (k->snd_wnd == 0) {
            k->snd_wnd = 1;         /* a probe, to hear the window again */
        }
        tcp_push(k, false);
    }
}

static void tcp_timers(void) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i].proto == NET_TCP && socks[i].timer != 0 && now() >= socks[i].timer) {
            tcp_timeout(&socks[i]);
        }
    }
}

static void ring_put(struct sock *k, const uint8_t *data, uint32_t n) {
    uint32_t tail = (k->in_head + k->in_len) % RBUF;
    uint32_t first = n < RBUF - tail ? n : RBUF - tail;

    memcpy(k->in + tail, data, first);
    memcpy(k->in, data + first, n - first);
    k->in_len += n;
}

static void ring_get(struct sock *k, uint8_t *to, uint32_t n, bool peek) {
    uint32_t first = n < RBUF - k->in_head ? n : RBUF - k->in_head;

    memcpy(to, k->in + k->in_head, first);
    memcpy(to + first, k->in, n - first);
    if (!peek) {
        k->in_head = (k->in_head + n) % RBUF;
        k->in_len -= n;
    }
}

static struct sock *tcp_find(uint32_t src, uint16_t sport, uint16_t dport) {
    struct sock *listener = NULL;

    for (unsigned i = 0; i < SOCKETS; i++) {
        struct sock *k = &socks[i];

        if (k->proto != NET_TCP || k->port != dport || k->state == CLOSED) {
            continue;
        }
        if (k->state == LISTEN) {
            listener = k;
        } else if (k->peer_ip == src && k->peer_port == sport) {
            return k;
        }
    }
    return listener;
}

/* A connection asked for on a listening socket. */
static void tcp_offer(struct sock *l, uint32_t src, uint16_t sport, uint32_t seq,
                      uint16_t win, uint16_t mss) {
    unsigned waiting = 0;
    struct sock *c;

    for (unsigned i = 0; i < SOCKETS; i++) {
        waiting += socks[i].proto == NET_TCP && socks[i].parent == number(l);
    }
    if (waiting >= l->backlog || (c = sock_new(NET_TCP)) == NULL) {
        return;                     /* it will ask again */
    }
    uint32_t iss = (uint32_t)efi_uptime_us() * 2654435761u;

    c->state = SYN_RCVD;
    c->parent = (uint8_t)number(l);
    c->port = l->port;
    c->peer_ip = src;
    c->peer_port = sport;
    c->rcv_nxt = seq + 1;
    c->snd_una = iss;
    c->snd_nxt = iss + 1;
    c->snd_wnd = win;
    c->mss = mss;
    c->loop = hop_src == NULL;
    if (hop_src != NULL) {
        memcpy(c->hop, hop_src, 6);
    }
    arm(c);
    tcp_send(c, SYN | ACK, iss, NULL, 0);
}

/* Everything of the segment is read into locals first: answering it may
   loop a reply back through here before this returns, over the buffer it
   arrived in. */
static void tcp_input(const struct ip *h, const uint8_t *p, size_t len) {
    const struct tcp *t = (const struct tcp *)p;
    size_t off;

    if (len < sizeof *t || (off = (t->off >> 4) * 4u) < sizeof *t || off > len) {
        return;
    }
    uint32_t src = h->src, seq = be32(t->seq), ack = be32(t->ack), dlen = (uint32_t)(len - off);
    uint16_t sport = t->sport, dport = t->dport, win = be16(t->win), mss = 536;
    uint8_t fl = t->flags;
    const uint8_t *data = p + off;

    for (size_t i = sizeof *t; i + 1 < off && p[i] != 0;) {
        if (p[i] == 1) {
            i++;
            continue;
        }
        if (p[i] == 2 && p[i + 1] == 4 && i + 4 <= off) {
            mss = (uint16_t)(p[i + 2] << 8 | p[i + 3]);
        }
        if (p[i + 1] < 2) {
            break;
        }
        i += p[i + 1];
    }
    if (mss > 1460) {
        mss = 1460;
    }

    struct sock *k = tcp_find(src, sport, dport);

    if (k == NULL) {
        if (!(fl & RST)) {
            tcp_refuse(src, sport, dport, seq, ack, fl, dlen);
        }
        return;
    }
    if (k->state == LISTEN) {
        if (fl & RST) {
            return;
        }
        if (fl & ACK) {
            tcp_refuse(src, sport, dport, seq, ack, fl, dlen);
        } else if (fl & SYN) {
            tcp_offer(k, src, sport, seq, win, mss);
        }
        return;
    }
    if (fl & RST) {
        if (k->state != SYN_SENT || ((fl & ACK) && ack == k->snd_nxt)) {
            tcp_closed(k, k->state == SYN_SENT ? ECONNREFUSED : ECONNRESET);
        }
        return;
    }
    if (k->state == SYN_SENT) {
        if ((fl & ACK) && ack != k->snd_nxt) {
            tcp_refuse(src, sport, dport, seq, ack, fl, dlen);
            return;
        }
        if ((fl & (SYN | ACK)) != (SYN | ACK)) {
            return;
        }
        k->rcv_nxt = seq + 1;
        k->mss = mss;
        k->snd_una = ack;
        k->snd_wnd = win;
        k->state = ESTABLISHED;
        k->timer = 0;
        k->tries = 0;
        tcp_push(k, true);
        return;
    }
    if (fl & SYN) {                 /* our answer to it was lost */
        tcp_send(k, k->state == SYN_RCVD ? SYN | ACK : ACK,
                 k->state == SYN_RCVD ? k->snd_una : k->snd_nxt, NULL, 0);
        return;
    }

    bool need_ack = false;

    /* Only what comes next is taken. What was had already is trimmed off;
       what is ahead of a gap is dropped, to come again. */
    if (dlen > 0 || (fl & FIN)) {
        if (seq_lt(seq, k->rcv_nxt)) {
            uint32_t skip = k->rcv_nxt - seq;

            if (skip > dlen) {
                skip = dlen;
                fl &= (uint8_t)~FIN;    /* had that too */
            }
            data += skip;
            dlen -= skip;
            seq += skip;
            need_ack = true;
        }
        if (seq != k->rcv_nxt) {
            dlen = 0;
            fl &= (uint8_t)~FIN;
            need_ack = true;
        }
    }
    if (!(fl & ACK)) {
        return;
    }
    if (k->state == SYN_RCVD) {
        if (ack != k->snd_nxt) {
            tcp_refuse(src, sport, dport, seq, ack, fl, dlen);
            return;
        }
        k->snd_una = ack;
        k->state = ESTABLISHED;
        k->timer = 0;
        k->tries = 0;
    } else if (seq_lt(k->snd_una, ack) && !seq_lt(k->snd_nxt, ack)) {
        uint32_t n = ack - k->snd_una;
        uint32_t gone = n < k->out_len ? n : k->out_len;

        memmove(k->out, k->out + gone, k->out_len - gone);
        k->out_len -= gone;
        k->snd_una = ack;
        k->tries = 0;
        k->dups = 0;
        k->rto = RTO;
        k->timer = k->snd_una == k->snd_nxt ? 0 : now() + k->rto;
        if (k->fin_sent && ack == k->snd_nxt) {     /* our FIN is in */
            if (k->state == LAST_ACK || k->state == CLOSING) {
                tcp_closed(k, 0);
                return;
            }
            if (k->state == FIN_WAIT1) {
                k->state = FIN_WAIT2;
                if (k->refs == 0) {
                    k->timer = now() + 30000;
                }
            }
        }
    } else if (seq_lt(k->snd_nxt, ack)) {
        tcp_send(k, ACK, k->snd_nxt, NULL, 0);      /* ahead of anything sent */
        return;
    } else if (ack == k->snd_una && dlen == 0 && !(fl & FIN) && k->snd_nxt != k->snd_una &&
               ++k->dups == 3) {
        k->snd_nxt = k->snd_una;    /* three the same: the first went missing */
        k->fin_sent = false;
    }
    k->snd_wnd = win;

    if (dlen > 0 && (k->state == ESTABLISHED || k->state == FIN_WAIT1 || k->state == FIN_WAIT2)) {
        uint32_t take = RBUF - k->in_len < dlen ? RBUF - k->in_len : dlen;

        ring_put(k, data, take);
        k->rcv_nxt += take;
        need_ack = true;
        if (take < dlen) {
            fl &= (uint8_t)~FIN;
        }
    }
    if ((fl & FIN) && !k->fin_in) {
        k->rcv_nxt++;
        k->fin_in = true;
        need_ack = true;
        if (k->state == ESTABLISHED) {
            k->state = CLOSE_WAIT;
        } else if (k->state == FIN_WAIT1) {
            k->state = CLOSING;
        } else if (k->state == FIN_WAIT2) {
            tcp_send(k, ACK, k->snd_nxt, NULL, 0);
            tcp_closed(k, 0);       /* no TIME_WAIT: nothing here reuses it soon */
            return;
        }
    }
    tcp_push(k, need_ack);
}

/* ---- DHCP ------------------------------------------------------------------ */

struct dhcp {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16], sname[64], file[128];
    uint32_t magic;
    uint8_t  options[64];
} __attribute__((packed));

#define DHCP_MAGIC 0x63538263u      /* 99.130.83.99, as it sits in memory */
#define DISCOVER 1
#define OFFER    2
#define REQUEST  3
#define DHCP_ACK 5

static uint32_t xid;
static uint8_t  dhcp_got;
static bool     dhcp_quick;         /* one try, briefly: the idle one */
static uint32_t offered, server, got_mask, got_gateway, got_dns;

static void dhcp_send(uint8_t type) {
    struct udp *u = (struct udp *)payload();
    struct dhcp *d = (struct dhcp *)(u + 1);
    uint8_t *o = d->options;

    memset(d, 0, sizeof *d);
    d->op = 1;
    d->htype = 1;
    d->hlen = 6;
    d->xid = xid;
    d->flags = be16(0x8000);        /* answer by broadcast: no address yet */
    memcpy(d->chaddr, card->mac, 6);
    d->magic = DHCP_MAGIC;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == REQUEST) {
        *o++ = 50; *o++ = 4; memcpy(o, &offered, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &server, 4); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o = 255;
    *u = (struct udp){ be16(68), be16(67), be16(sizeof *u + sizeof *d), 0 };
    ip_send(BROADCAST, everyone, false, NET_UDP, sizeof *u + sizeof *d, 64);
}

static void dhcp_input(const uint8_t *p, size_t len) {
    const struct dhcp *d = (const struct dhcp *)p;
    const uint8_t *o = d->options, *end = p + len;
    uint8_t type = 0;

    if (len < offsetof(struct dhcp, options) || d->op != 2 || d->xid != xid ||
        d->magic != DHCP_MAGIC) {
        return;
    }
    while (o + 2 <= end && *o != 255) {
        if (*o == 0) {
            o++;
            continue;
        }
        uint8_t code = o[0], size = o[1];

        o += 2;
        if (o + size > end) {
            break;
        }
        if (code == 53 && size >= 1) {
            type = o[0];
        } else if (size >= 4 && (code == 1 || code == 3 || code == 6 || code == 54)) {
            memcpy(code == 1 ? &got_mask : code == 3 ? &got_gateway :
                   code == 6 ? &got_dns : &server, o, 4);
        }
        o += size;
    }
    if (type == OFFER && dhcp_got == 0) {
        offered = d->yiaddr;
        dhcp_got = OFFER;
    } else if (type == DHCP_ACK && dhcp_got == OFFER) {
        dhcp_got = DHCP_ACK;
    }
}

static bool dhcp_wait(uint8_t want, uint64_t ms) {
    for (uint64_t until = now() + ms; now() < until;) {
        net_poll();
        if (dhcp_got == want) {
            return true;
        }
    }
    return false;
}

static void ip_text(char *out, uint32_t ip) {
    ksprintf(out, "%u.%u.%u.%u", ip & 0xFF, ip >> 8 & 0xFF, ip >> 16 & 0xFF, ip >> 24);
}

/* The name server goes where a libc looks for it, if it is not there
   already: a write is a disk write. */
static void write_resolv(void) {
    char text[40], ip[16];
    struct fs_file file;
    const char *was;
    size_t len, i = 0;

    ip_text(ip, dns);
    ksprintf(text, "nameserver %s\n", ip);
    len = strlen(text);
    if (fs_stat("/etc/resolv.conf", &file) == 0 && file.size == len &&
        (was = fs_sector(file.start, 0)) != NULL) {
        while (i < len && was[i] == text[i]) {
            i++;
        }
    }
    if (i != len) {
        fs_write("/etc/resolv.conf", text, len);
    }
}

static bool dhcp(void) {
    xid = (uint32_t)efi_uptime_us() ^ (uint32_t)card->mac[5] << 24;
    for (unsigned tries = 0; tries < (dhcp_quick ? 1u : 3u); tries++) {
        dhcp_got = 0;
        got_mask = got_gateway = got_dns = 0;
        dhcp_send(DISCOVER);
        if (!dhcp_wait(OFFER, 1000u << tries)) {
            continue;
        }
        dhcp_send(REQUEST);
        if (dhcp_wait(DHCP_ACK, 1000)) {
            my_ip = offered;
            mask = got_mask != 0 ? got_mask : IP(255, 255, 255, 0);
            gateway = got_gateway;
            dns = got_dns;
            if (dns != 0) {
                write_resolv();
            }
            return true;
        }
    }
    return false;
}

/* ---- in ------------------------------------------------------------------------ */

static void udp_input(const struct ip *h, const uint8_t *p, size_t len) {
    const struct udp *u = (const struct udp *)p;

    if (len < sizeof *u || be16(u->len) > len || be16(u->len) < sizeof *u) {
        return;
    }
    len = be16(u->len) - sizeof *u;
    if (u->dport == be16(68) && state == NO_ADDRESS) {
        dhcp_input(p + sizeof *u, len);
        return;
    }
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct sock *k = &socks[i];

        if (k->proto == NET_UDP && k->port == u->dport &&
            (k->peer_ip == 0 || (k->peer_ip == h->src && k->peer_port == u->sport))) {
            deliver(k, h, u->sport, p + sizeof *u, len);
            return;
        }
    }
    if (h->dst != BROADCAST && h->dst != (my_ip | ~mask)) {
        icmp_send_error(h, 3, 3);   /* nobody here: port unreachable */
    }
}

/* A copy of the whole packet, header and all, for each raw socket of its
   protocol - before the stack itself sees it, which it still does. */
static void raw_input(const struct ip *h, size_t total) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct sock *k = &socks[i];

        if (k->proto != SK_RAW || k->ipproto != h->proto) {
            continue;
        }
        if (h->proto == NET_ICMP) {
            const uint8_t *type = (const uint8_t *)h + (h->vhl & 15) * 4u;

            if (*type < 32 && (k->icmp_filter >> *type & 1) != 0) {
                continue;
            }
        }
        deliver(k, h, 0, h, total);
    }
}

static void ip_input(const uint8_t *p, size_t len) {
    const struct ip *h = (const struct ip *)p;
    size_t head, total;

    if (len < sizeof *h || h->vhl >> 4 != 4) {
        return;
    }
    head = (h->vhl & 15) * 4u;
    total = be16(h->len);
    if (total > len || total < head || (h->frag & be16(0x3FFF)) != 0) {
        return;                     /* short, or a fragment */
    }
    if (my_ip != 0 && h->dst != my_ip && h->dst != BROADCAST &&
        h->dst != (my_ip | ~mask) && !loopback(h->dst)) {
        return;
    }
    raw_input(h, total);
    if (h->proto == NET_ICMP) {
        icmp_input(h, p + head, total - head);
    } else if (h->proto == NET_UDP) {
        udp_input(h, p + head, total - head);
    } else if (h->proto == NET_TCP) {
        tcp_input(h, p + head, total - head);
    }
}

static void eth_input(const uint8_t *frame, size_t len) {
    const struct eth *e = (const struct eth *)frame;

    eth_counts.rx_packets++;
    eth_counts.rx_bytes += len;
    if (down) {
        eth_counts.rx_dropped++;
        return;
    }
    if (len >= HEADERS && e->type == be16(ETH_IP)) {
        const struct ip *h = (const struct ip *)(e + 1);

        if (my_ip != 0 && ((h->src ^ my_ip) & mask) == 0) {
            arp_learn(h->src, e->src);
        }
        hop_src = e->src;
        ip_input((const uint8_t *)h, len - sizeof *e);
        hop_src = NULL;
    } else if (len >= sizeof *e && e->type == be16(ETH_ARP)) {
        arp_input((const struct arp *)(e + 1), len - sizeof *e);
    }
}

void net_poll(void) {
    if (polling) {
        return;
    }
    polling = true;
    if (started) {
        card->poll(eth_input);
    }
    tcp_timers();
    polling = false;
}

/* ---- bringing it up -------------------------------------------------------------- */

static uint64_t tried;

static bool net_up(void) {
    if (state == NO_ADDRESS && !started && !(started = card->start())) {
        card = NULL;                /* there, but it would not start */
        state = NO_CARD;
    }
    if (state == NO_ADDRESS && !manual && !down && (tried == 0 || now() - tried > 10000)) {
        tried = now();
        if (dhcp()) {
            state = UP;
        }
    }
    return state == UP;
}

/* A driver registered its card with the kernel, or took it away: the stack
   takes up whatever is there now, starting it only when it is used. */
/* Once, at the first idle moment with a card and no address: one quick
   try, so that a network with no DHCP server costs the prompt a moment, not
   the seconds the full exchange can wait. */
static void op_idle(void) {
    static const struct net_card *tried_idle;

    if (card != NULL && card != tried_idle && state == NO_ADDRESS && !manual && !down) {
        tried_idle = card;
        dhcp_quick = true;
        net_up();
        dhcp_quick = false;
        if (state != UP) {
            tried = 0;              /* the full exchange, at the first use */
        }
    }
}

static void op_card_changed(void) {
    card = net_card;
    started = false;
    state = card != NULL ? NO_ADDRESS : NO_CARD;
    my_ip = 0;
    tried = 0;
    manual = down = false;
    memset(arps, 0, sizeof arps);
    if (card != NULL) {
        dbg("net: %s\n", card->name);
    }
}

static void net_show(char *args) {
    char a[16], b[16], c[16], hw[18];

    (void)args;
    net_up();
    if (card == NULL) {
        vga_puts("no network card\n");
        return;
    }
    for (unsigned i = 0; i < 6; i++) {
        hw[3 * i] = "0123456789abcdef"[card->mac[i] >> 4];
        hw[3 * i + 1] = "0123456789abcdef"[card->mac[i] & 15];
        hw[3 * i + 2] = i < 5 ? ':' : '\0';
    }
    kprintf("%s %s", card->name, hw);
    if (state != UP) {
        vga_puts(" no address\n");
        return;
    }
    unsigned bits = 0;

    for (uint32_t m = mask; m != 0; m >>= 1) {
        bits += m & 1;
    }
    ip_text(a, my_ip);
    ip_text(b, gateway);
    ip_text(c, dns);
    kprintf(" %s/%u via %s dns %s\n", a, bits, b, c);
}

static const struct proc_cmd net_cmd = { "net", "", net_show };

/* ---- the calls ------------------------------------------------------------------ */

static int op_socket(int proto) {
    uint8_t type = (proto & NET_NETLINK) ? SK_NETLINK : (proto & NET_RAW) ? SK_RAW : (uint8_t)proto;
    struct sock *k;

    if (type != SK_NETLINK) {
        net_up();
    }
    if ((k = sock_new(type)) == NULL) {
        return -ENFILE;
    }
    k->refs = 1;
    k->ipproto = (uint8_t)proto;
    k->hdrincl = type == SK_RAW && k->ipproto == 255;      /* IPPROTO_RAW */
    if (type == SK_NETLINK) {
        k->port = (uint16_t)(4096 + number(k));             /* its port id */
    }
    return number(k);
}

static void op_hold(int s) {
    sock(s)->refs++;
}

static void op_drop(int s) {
    struct sock *k = sock(s);

    if (k->proto == 0 || k->refs == 0 || --k->refs > 0) {
        return;
    }
    if (k->proto != NET_TCP) {
        sock_free(k);
        return;
    }
    switch (k->state) {
    case LISTEN:
        for (unsigned i = 0; i < SOCKETS; i++) {
            if (socks[i].proto == NET_TCP && socks[i].parent == s) {
                tcp_abort(&socks[i], 0);
            }
        }
        sock_free(k);
        return;
    case ESTABLISHED:
    case CLOSE_WAIT:
        if (k->in_len > 0) {
            tcp_abort(k, 0);        /* unread data: Linux resets too */
            return;
        }
        k->fin_out = true;
        tcp_push(k, false);
        break;
    case SYN_SENT:
    case CLOSED:
        sock_free(k);
        return;
    }
    /* Held by the stack alone now, until the far end has seen it closed. A
       moment's grace for that, since nothing looks at the card once the
       program has gone. */
    for (uint64_t until = now() + LINGER;
         k->proto == NET_TCP && k->state != FIN_WAIT2 && now() < until;) {
        net_poll();
    }
    if (k->proto == NET_TCP && k->state == FIN_WAIT2) {
        k->timer = now() + 30000;
    }
}

static void op_close_all(void) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i].proto != 0 && socks[i].refs > 0) {
            socks[i].refs = 1;
            op_drop((int)i + 1);
        }
    }
}

static int op_bind(int s, uint32_t ip, uint16_t port) {
    struct sock *k = sock(s);

    (void)ip;
    if (k->proto == SK_RAW || k->proto == SK_NETLINK) {
        return 0;
    }
    if (k->port != 0) {
        return -EINVAL;
    }
    if (port != 0 && port_taken(k->proto, port)) {
        return -EADDRINUSE;
    }
    k->port = port;
    autobind(k);
    return 0;
}

static int op_connect(int s, uint32_t ip, uint16_t port) {
    struct sock *k = sock(s);
    bool loop;
    int err;

    if (k->proto == SK_NETLINK) {
        return 0;
    }
    if (k->proto != NET_TCP) {
        if (ip != 0 && !loopback(ip) && state != UP) {
            return -ENETUNREACH;
        }
        k->peer_ip = ip;
        k->peer_port = port;
        if (k->proto != SK_RAW) {
            autobind(k);
        }
        return 0;
    }
    if (k->state == SYN_SENT) {
        return -EALREADY;
    }
    if (k->state != CLOSED || k->fin_in) {
        return -EISCONN;
    }
    if (ip == 0) {
        return -EINVAL;
    }
    if ((err = route(ip, k->hop, &loop)) < 0) {
        return err;
    }
    uint32_t iss = (uint32_t)efi_uptime_us() * 2654435761u;

    k->loop = loop;
    k->peer_ip = ip;
    k->peer_port = port;
    k->error = 0;
    autobind(k);
    k->snd_una = iss;
    k->snd_nxt = iss + 1;
    k->state = SYN_SENT;
    arm(k);
    tcp_send(k, SYN, iss, NULL, 0);
    return -EINPROGRESS;
}

static int op_listen(int s, int backlog) {
    struct sock *k = sock(s);

    if (k->proto != NET_TCP) {
        return -EOPNOTSUPP;
    }
    if (k->state != CLOSED && k->state != LISTEN) {
        return -EISCONN;
    }
    autobind(k);
    k->state = LISTEN;
    k->backlog = (uint8_t)(backlog < 1 ? 1 : backlog > 8 ? 8 : backlog);
    return 0;
}

static int op_accept(int s) {
    struct sock *l = sock(s);

    if (l->proto != NET_TCP || l->state != LISTEN) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct sock *c = &socks[i];

        if (c->proto == NET_TCP && c->parent == s && c->state != SYN_RCVD) {
            c->parent = 0;
            c->refs = 1;
            return (int)i + 1;
        }
    }
    net_poll();
    return -EAGAIN;
}

static int op_shutdown(int s, int how) {
    struct sock *k = sock(s);

    if (k->proto != NET_TCP || how == 0) {
        return 0;                   /* reading is simply not done */
    }
    if (k->state != ESTABLISHED && k->state != CLOSE_WAIT) {
        return k->state == CLOSED || k->state == LISTEN || k->state == SYN_SENT ? -ENOTCONN : 0;
    }
    k->fin_out = true;
    tcp_push(k, false);
    return 0;
}

static void op_name(int s, bool peer, uint32_t *ip, uint16_t *port) {
    struct sock *k = sock(s);

    if (k->proto == SK_NETLINK) {
        *ip = peer ? 0 : k->port;   /* the port id; the kernel's is 0 */
        *port = 0;
        return;
    }
    *ip = peer ? k->peer_ip : k->peer_ip != 0 ? source_for(k->peer_ip) : 0;
    *port = peer ? k->peer_port : k->port;
}

static int64_t tcp_write(struct sock *k, const void *data, size_t size) {
    if (k->state != ESTABLISHED && k->state != CLOSE_WAIT) {
        if (k->error != 0) {
            int err = k->error;

            k->error = 0;
            return -err;
        }
        return k->state == SYN_SENT ? -EAGAIN : k->state == LISTEN ? -ENOTCONN : -EPIPE;
    }
    if (k->fin_out) {
        return -EPIPE;
    }
    uint32_t n = SBUF - k->out_len;

    if (n > size) {
        n = (uint32_t)size;
    }
    if (n == 0) {
        net_poll();
        return -EAGAIN;
    }
    memcpy(k->out + k->out_len, data, n);
    k->out_len += n;
    tcp_push(k, false);
    return n;
}

static int64_t nl_request(struct sock *k, const uint8_t *data, size_t size);

/* A raw socket's packet: its payload, or with IP_HDRINCL the whole of it,
   whose header is finished off here as Linux does. */
static int64_t raw_send(struct sock *k, const uint8_t *data, size_t size, uint32_t ip) {
    uint8_t to[6];
    bool loop;
    int err;

    if (k->hdrincl) {
        const struct ip *given = (const struct ip *)data;

        if (size < sizeof *given || size > MTU) {
            return size > MTU ? -EMSGSIZE : -EINVAL;
        }
        uint32_t dst = given->dst;

        if ((err = route(dst, to, &loop)) < 0) {
            return err;
        }
        struct ip *h = (struct ip *)(tx_buf + sizeof(struct eth));

        memcpy(h, data, size);
        h->len = be16((uint16_t)size);
        if (h->id == 0) {
            h->id = be16(ip_id++);
        }
        if (h->src == 0) {
            h->src = source_for(dst);
        }
        ip_out(h, to, loop);
        return (int64_t)size;
    }
    if (size > MTU - sizeof(struct ip)) {
        return -EMSGSIZE;
    }
    if ((err = route(ip, to, &loop)) < 0) {
        return err;
    }
    memcpy(payload(), data, size);
    ip_send(ip, to, loop, k->ipproto, size, ttl_of(k));
    return (int64_t)size;
}

static int64_t op_send(int s, const void *data, size_t size, uint32_t ip, uint16_t port) {
    struct sock *k = sock(s);
    size_t head = k->proto == NET_UDP ? sizeof(struct udp) : 0;
    uint8_t to[6];
    bool loop;
    int err;

    if (k->proto == NET_TCP) {
        return tcp_write(k, data, size);
    }
    if (k->proto == SK_NETLINK) {
        return nl_request(k, data, size);
    }
    if (ip == 0) {
        if (k->peer_ip == 0) {
            return -EDESTADDRREQ;
        }
        ip = k->peer_ip;
        port = k->peer_port;
    }
    if (k->proto == SK_RAW) {
        return raw_send(k, data, size, ip);
    }
    if (size + head > MTU - sizeof(struct ip)) {
        /* Too big to go at all, which the error queue says with the MTU
           it would have to fit: how tracepath finds the path's. */
        if (k->recverr) {
            struct errq e = { 0, port, 1 /* SO_EE_ORIGIN_LOCAL */, 0, 0, 0, ip, EMSGSIZE, MTU, 0 };

            errq_push(k, &e, NULL, 0);
        }
        return -EMSGSIZE;
    }
    if (k->proto == NET_ICMP && (size < sizeof(struct icmp) || ((const uint8_t *)data)[0] != 8)) {
        return -EINVAL;
    }
    autobind(k);
    /* Found before anything is written where it is sent from: finding it
       may mean answering someone else's packet from there. */
    if ((err = route(ip, to, &loop)) < 0) {
        return err;
    }
    uint8_t *p = payload();

    memcpy(p + head, data, size);
    if (k->proto == NET_UDP) {
        struct udp *u = (struct udp *)p;

        *u = (struct udp){ k->port, port, be16((uint16_t)(head + size)), 0 };
        u->sum = sum_over(ip, NET_UDP, p, head + size);
        if (u->sum == 0) {
            u->sum = 0xFFFF;
        }
    } else {
        struct icmp *m = (struct icmp *)p;

        m->id = k->port;
        m->sum = 0;
        m->sum = sum_end(sum_add(0, p, size));
    }
    ip_send(ip, to, loop, (uint8_t)k->proto, head + size, ttl_of(k));
    return (int64_t)size;
}

static int64_t tcp_read(struct sock *k, void *data, size_t size, bool peek) {
    if (k->in_len > 0) {
        uint32_t n = size < k->in_len ? (uint32_t)size : k->in_len;
        uint32_t was = RBUF - k->in_len;

        ring_get(k, data, n, peek);
        /* Room made where there was little: say so, or the far end waits
           on a window it thinks is shut. */
        if (!peek && ((was < k->mss && was + n >= k->mss) ||
                      (was < RBUF / 2 && was + n >= RBUF / 2))) {
            tcp_send(k, ACK, k->snd_nxt, NULL, 0);
        }
        return n;
    }
    if (k->error != 0) {
        int err = k->error;

        k->error = 0;
        return -err;
    }
    if (k->fin_in || (k->state == CLOSED && k->peer_ip != 0)) {
        return 0;                   /* the end */
    }
    return k->state == LISTEN || k->state == CLOSED ? -ENOTCONN : -EAGAIN;
}

/* The oldest on the error queue; what is left says what sk_err is now. */
static int64_t errq_pop(struct sock *k, void *data, size_t size, struct net_from *from, bool peek) {
    const struct errq *e = (const struct errq *)k->errq;

    if (k->errq_len == 0) {
        return -EAGAIN;
    }
    size_t whole = e->size;

    memcpy(data, e + 1, whole < size ? whole : size);
    *from = (struct net_from){ e->ip, e->port, e->ttl, true, e->origin, e->type, e->code,
                               e->errnum, e->info, e->offender };
    if (!peek) {
        uint32_t used = sizeof *e + (uint32_t)whole;

        memmove(k->errq, k->errq + used, k->errq_len - used);
        k->errq_len -= used;
        k->error = k->errq_len > 0 ? (uint8_t)((const struct errq *)k->errq)->errnum : 0;
    }
    return (int64_t)whole;
}

static int64_t op_recv(int s, void *data, size_t size, struct net_from *from, unsigned flags) {
    struct sock *k = sock(s);
    struct dgram *d = (struct dgram *)k->in;
    bool peek = (flags & NET_PEEK) != 0;
    size_t whole;

    net_poll();
    *from = (struct net_from){ .ip = k->peer_ip, .port = k->peer_port, .ttl = 64 };
    if (flags & NET_ERRQUEUE) {
        return errq_pop(k, data, size, from, peek);
    }
    if (k->proto == NET_TCP) {
        return tcp_read(k, data, size, peek);
    }
    if (k->in_len == 0) {
        if (k->error != 0) {
            int err = k->error;

            k->error = 0;
            return -err;
        }
        return -EAGAIN;
    }
    whole = d->size;
    memcpy(data, d + 1, whole < size ? whole : size);
    from->ip = d->ip;
    from->port = d->port;
    from->ttl = (uint8_t)d->ttl;
    if (!peek) {
        uint32_t used = sizeof *d + (uint32_t)whole;

        memmove(k->in, k->in + used, k->in_len - used);
        k->in_len -= used;
    }
    return (int64_t)whole;
}

static unsigned op_ready(int s) {
    struct sock *k = sock(s);
    unsigned r = 0;

    net_poll();
    if (k->proto != NET_TCP) {
        return (k->in_len > 0 ? NET_IN : 0) | NET_OUT |
               (k->errq_len > 0 || k->error != 0 ? NET_ERR : 0);
    }
    if (k->state == LISTEN) {
        for (unsigned i = 0; i < SOCKETS; i++) {
            if (socks[i].proto == NET_TCP && socks[i].parent == s && socks[i].state != SYN_RCVD) {
                return NET_IN;
            }
        }
        return 0;
    }
    if (k->in_len > 0 || k->fin_in) {
        r |= NET_IN;
    }
    if ((k->state == ESTABLISHED || k->state == CLOSE_WAIT) && k->out_len < SBUF && !k->fin_out) {
        r |= NET_OUT;
    }
    if (k->error != 0) {
        r |= NET_ERR | NET_IN;
    }
    if (k->state == CLOSED) {
        r |= NET_HUP | NET_IN;
    }
    return r;
}

static int op_error(int s) {
    struct sock *k = sock(s);
    int err = k->error;

    k->error = 0;
    return err;
}

static uint32_t op_memory(void) {
    uint32_t bytes = 4096 + (started ? card->memory : 0);

    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i].in != NULL) {
            bytes += in_size(&socks[i]);
        }
        if (socks[i].out != NULL) {
            bytes += SBUF;
        }
        if (socks[i].errq != NULL) {
            bytes += 4096;
        }
    }
    return bytes;
}

static uint32_t op_pending(int s) {
    struct sock *k = sock(s);

    net_poll();
    if (k->proto == NET_TCP) {
        return k->in_len;
    }
    return k->in_len > 0 ? ((const struct dgram *)k->in)->size : 0;
}

/* ---- options -------------------------------------------------------------------- */

#define SOL_IP          0
#define SOL_RAW         255
#define SOL_NETLINK     270
#define IP_TOS          1
#define IP_TTL          2
#define IP_HDRINCL      3
#define IP_PKTINFO      8
#define IP_MTU_DISCOVER 10
#define IP_RECVERR      11
#define IP_RECVTTL      12
#define IP_RECVTOS      13
#define IP_MTU          14
#define ICMP_FILTER     1

static int op_setopt(int s, int level, int name, uint32_t value) {
    struct sock *k = sock(s);

    if (level == SOL_NETLINK && k->proto == SK_NETLINK) {
        return 0;                   /* memberships, strict checking: all fine */
    }
    if (level == SOL_RAW && name == ICMP_FILTER && k->proto == SK_RAW && k->ipproto == NET_ICMP) {
        k->icmp_filter = value;
        return 0;
    }
    if (level != SOL_IP || k->proto == SK_NETLINK) {
        return -ENOPROTOOPT;
    }
    switch (name) {
    case IP_TTL:
        if (value != 0xFFFFFFFFu && (value == 0 || value > 255)) {
            return -EINVAL;
        }
        k->ttl = value == 0xFFFFFFFFu ? 0 : (uint8_t)value;
        return 0;
    case IP_HDRINCL:
        if (k->proto != SK_RAW) {
            return -ENOPROTOOPT;
        }
        k->hdrincl = value != 0;
        return 0;
    case IP_RECVERR:
        k->recverr = value != 0;
        if (!k->recverr) {
            k->errq_len = 0;
        }
        return 0;
    case IP_TOS:
    case IP_PKTINFO:
    case IP_MTU_DISCOVER:
    case IP_RECVTTL:                /* the TTL comes with every datagram anyway */
    case IP_RECVTOS:
        return 0;
    }
    return name >= 32 && name <= 48 ? 0 : -ENOPROTOOPT;   /* multicast: nothing to join */
}

static int op_getopt(int s, int level, int name, uint32_t *value) {
    struct sock *k = sock(s);

    if (level == SOL_RAW && name == ICMP_FILTER && k->proto == SK_RAW) {
        *value = k->icmp_filter;
        return 0;
    }
    if (level != SOL_IP || k->proto == SK_NETLINK) {
        return -ENOPROTOOPT;
    }
    switch (name) {
    case IP_TTL:
        *value = ttl_of(k);
        return 0;
    case IP_HDRINCL:
        *value = k->hdrincl;
        return 0;
    case IP_RECVERR:
        *value = k->recverr;
        return 0;
    case IP_MTU:
        if (k->peer_ip == 0) {
            return -ENOTCONN;
        }
        *value = k->loop ? 65536 : MTU;
        return 0;
    case IP_TOS:
    case IP_MTU_DISCOVER:
        *value = 0;
        return 0;
    }
    return -ENOPROTOOPT;
}

/* ---- interfaces ----------------------------------------------------------------- */

static unsigned prefix(uint32_t m) {
    unsigned bits = 0;

    for (m = be32(m); m & 0x80000000u; m <<= 1) {
        bits++;
    }
    return bits;
}

static uint32_t prefix_mask(unsigned bits) {
    return bits == 0 ? 0 : be32(0xFFFFFFFFu << (32 - bits));
}

static void counts_to(struct net_iface *f, const struct counts *c) {
    f->rx_packets = c->rx_packets;
    f->rx_bytes = c->rx_bytes;
    f->tx_packets = c->tx_packets;
    f->tx_bytes = c->tx_bytes;
    f->rx_dropped = c->rx_dropped;
}

static bool op_iface(unsigned index, struct net_iface *f) {
    memset(f, 0, sizeof *f);
    if (index == 1) {
        strcpy(f->name, "lo");
        f->type = 772;
        f->flags = NET_IFF_UP | NET_IFF_LOOPBACK | NET_IFF_RUNNING | NET_IFF_LOWER_UP;
        f->ip = IP(127, 0, 0, 1);
        f->mask = IP(255, 0, 0, 0);
        f->mtu = 65536;
        counts_to(f, &lo_counts);
        return true;
    }
    if (index != 2 || card == NULL) {
        return false;
    }
    net_up();                       /* started, and asked for an address */
    if (card == NULL) {
        return false;
    }
    strcpy(f->name, "eth0");
    memcpy(f->mac, card->mac, 6);
    f->type = 1;
    f->flags = (down ? 0 : NET_IFF_UP) | NET_IFF_BROADCAST | NET_IFF_MULTICAST |
               (started && !down ? NET_IFF_RUNNING | NET_IFF_LOWER_UP : 0);
    if (state == UP) {
        f->ip = my_ip;
        f->mask = mask;
        f->broadcast = my_ip | ~mask;
        f->gateway = gateway;
    }
    f->mtu = MTU;
    counts_to(f, &eth_counts);
    return true;
}

static int op_iface_set(unsigned index, unsigned what, uint32_t value) {
    if (index == 1) {
        return what == NET_SET_FLAGS ? 0 : -EPERM;
    }
    if (index != 2 || card == NULL) {
        return -ENODEV;
    }
    switch (what) {
    case NET_SET_FLAGS:
        down = (value & NET_IFF_UP) == 0;
        if (!down) {
            net_up();
        }
        return 0;
    case NET_SET_IP:
        if (!started && !(started = card->start())) {
            return -ENODEV;
        }
        manual = true;
        my_ip = value;
        state = value != 0 ? UP : NO_ADDRESS;
        if (mask == 0) {
            mask = IP(255, 255, 255, 0);
        }
        return 0;
    case NET_SET_MASK:
        mask = value;
        return 0;
    case NET_SET_GATEWAY:
        gateway = value;
        return 0;
    }
    return -EINVAL;
}

/* ---- netlink ---------------------------------------------------------------------
 *
 * A request is answered at once, into the socket's queue: a dump as one
 * message a datagram and NLMSG_DONE after, anything else with its answer or
 * NLMSG_ERROR. NETLINK_ROUTE covers links, addresses, routes and the ARP
 * table, and changing an address, the default route or whether eth0 is up;
 * NETLINK_SOCK_DIAG lists the IPv4 sockets. */

struct nlmsghdr {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
};

#define NLMSG_ERROR   2
#define NLMSG_DONE    3
#define NLM_F_MULTI   0x2
#define NLM_F_ACK     0x4
#define NLM_F_ROOT    0x100

#define RTM_NEWLINK   16
#define RTM_GETLINK   18
#define RTM_SETLINK   19
#define RTM_NEWADDR   20
#define RTM_DELADDR   21
#define RTM_GETADDR   22
#define RTM_NEWROUTE  24
#define RTM_DELROUTE  25
#define RTM_GETROUTE  26
#define RTM_NEWNEIGH  28
#define RTM_GETNEIGH  30
#define SOCK_DIAG_BY_FAMILY 20

#define NETLINK_ROUTE     0
#define NETLINK_SOCK_DIAG 4
#define AF_INET           2

static struct sock *nl;             /* the socket being answered */
static struct dgram *nl_d;          /* the message being built in its queue */
static uint32_t nl_seq;
static uint8_t nl_spare[80];        /* what is built when there is no room */

static void *nl_begin(uint16_t type, uint16_t flags, size_t fixed) {
    if (nl->in_len + sizeof *nl_d + 1024 > in_size(nl)) {
        nl_d = NULL;
        memset(nl_spare, 0, sizeof nl_spare);
        return nl_spare;
    }
    nl_d = (struct dgram *)(nl->in + nl->in_len);

    struct nlmsghdr *n = (struct nlmsghdr *)(nl_d + 1);

    *n = (struct nlmsghdr){ (uint32_t)(sizeof *n + fixed), type, flags, nl_seq, nl->port };
    memset(n + 1, 0, fixed);
    return n + 1;
}

static void nl_attr(uint16_t type, const void *data, size_t len) {
    if (nl_d == NULL) {
        return;
    }
    struct nlmsghdr *n = (struct nlmsghdr *)(nl_d + 1);
    uint8_t *at = (uint8_t *)n + n->len;
    size_t whole = (4 + len + 3) & ~(size_t)3;

    *(uint16_t *)at = (uint16_t)(4 + len);
    *(uint16_t *)(at + 2) = type;
    memset(at + 4, 0, whole - 4);
    memcpy(at + 4, data, len);
    n->len += (uint32_t)whole;
}

static void nl_u32(uint16_t type, uint32_t v) {
    nl_attr(type, &v, 4);
}

static void nl_u8(uint16_t type, uint8_t v) {
    nl_attr(type, &v, 1);
}

static void nl_str(uint16_t type, const char *text) {
    nl_attr(type, text, strlen(text) + 1);
}

static void nl_end(void) {
    if (nl_d == NULL) {
        return;
    }
    const struct nlmsghdr *n = (const struct nlmsghdr *)(nl_d + 1);

    *nl_d = (struct dgram){ (uint16_t)n->len, 0, 0, 0 };
    nl->in_len += (uint32_t)(sizeof *nl_d + n->len);
}

static void nl_done(void) {
    nl_begin(NLMSG_DONE, NLM_F_MULTI, 4);
    nl_end();
}

/* An error, or with 0 the acknowledgement: either way, what it answers. */
static void nl_error(int err, const struct nlmsghdr *req) {
    int32_t *e = nl_begin(NLMSG_ERROR, 0, 4 + sizeof *req);

    e[0] = err;
    memcpy(e + 1, req, sizeof *req);
    nl_end();
}

/* The attribute of type among those from at to end, or NULL. */
static const uint8_t *nl_find(const uint8_t *at, const uint8_t *end, uint16_t type,
                              size_t *len) {
    while (at + 4 <= end) {
        uint16_t size = *(const uint16_t *)at, t = *(const uint16_t *)(at + 2) & 0x3FFF;

        if (size < 4 || at + size > end) {
            break;
        }
        if (t == type) {
            *len = size - 4u;
            return at + 4;
        }
        at += (size + 3u) & ~3u;
    }
    return NULL;
}

struct ifinfomsg {
    uint8_t  family, pad;
    uint16_t type;
    int32_t  index;
    uint32_t flags, change;
};

struct ifaddrmsg {
    uint8_t  family, prefixlen, flags, scope;
    uint32_t index;
};

struct rtmsg {
    uint8_t  family, dst_len, src_len, tos, table, protocol, scope, type;
    uint32_t flags;
};

struct ndmsg {
    uint8_t  family, pad1;
    uint16_t pad2;
    int32_t  index;
    uint16_t state;
    uint8_t  flags, type;
};

static void nl_link(unsigned index, uint16_t flags) {
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, none[6];
    struct net_iface f;
    uint64_t stats[24] = { 0 };
    uint32_t stats32[24] = { 0 };

    if (!op_iface(index, &f)) {
        return;
    }
    struct ifinfomsg *m = nl_begin(RTM_NEWLINK, flags, sizeof *m);

    m->type = f.type;
    m->index = (int32_t)index;
    m->flags = f.flags;
    nl_str(3, f.name);                                  /* IFLA_IFNAME */
    nl_u32(13, 1000);                                   /* IFLA_TXQLEN */
    nl_u8(16, index == 1 ? 0 : (f.flags & NET_IFF_RUNNING) ? 6 : 2);  /* IFLA_OPERSTATE */
    nl_u8(17, 0);                                       /* IFLA_LINKMODE */
    nl_u32(4, f.mtu);                                   /* IFLA_MTU */
    nl_u32(27, 0);                                      /* IFLA_GROUP */
    nl_u32(30, 0);                                      /* IFLA_PROMISCUITY */
    nl_u32(31, 1);                                      /* IFLA_NUM_TX_QUEUES */
    nl_u32(32, 1);                                      /* IFLA_NUM_RX_QUEUES */
    nl_str(6, index == 1 ? "noqueue" : "pfifo_fast");   /* IFLA_QDISC */
    nl_attr(1, f.mac, 6);                               /* IFLA_ADDRESS */
    nl_attr(2, index == 1 ? none : broadcast, 6);       /* IFLA_BROADCAST */
    stats[0] = f.rx_packets;
    stats[1] = f.tx_packets;
    stats[2] = f.rx_bytes;
    stats[3] = f.tx_bytes;
    stats[6] = f.rx_dropped;
    for (unsigned i = 0; i < 24; i++) {
        stats32[i] = (uint32_t)stats[i];
    }
    nl_attr(23, stats, sizeof stats);                   /* IFLA_STATS64 */
    nl_attr(7, stats32, sizeof stats32);                /* IFLA_STATS */
    nl_end();
}

static void nl_addr(unsigned index, uint16_t flags) {
    struct net_iface f;
    uint32_t cache[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0 };

    if (!op_iface(index, &f) || f.ip == 0) {
        return;
    }
    struct ifaddrmsg *a = nl_begin(RTM_NEWADDR, flags, sizeof *a);

    a->family = AF_INET;
    a->prefixlen = (uint8_t)prefix(f.mask);
    a->flags = index == 1 || manual ? 0x80 : 0;         /* IFA_F_PERMANENT */
    a->scope = index == 1 ? 254 : 0;                    /* host, universe */
    a->index = index;
    nl_attr(1, &f.ip, 4);                               /* IFA_ADDRESS */
    nl_attr(2, &f.ip, 4);                               /* IFA_LOCAL */
    if (index != 1) {
        nl_attr(4, &f.broadcast, 4);                    /* IFA_BROADCAST */
    }
    nl_str(3, f.name);                                  /* IFA_LABEL */
    nl_u32(8, a->flags);                                /* IFA_FLAGS */
    nl_attr(6, cache, sizeof cache);                    /* IFA_CACHEINFO: forever */
    nl_end();
}

static void nl_route(uint16_t flags, uint8_t table, uint8_t type, uint8_t proto,
                     uint8_t scope, uint32_t dst, uint8_t dst_len, uint32_t via,
                     uint32_t src, unsigned oif) {
    struct rtmsg *r = nl_begin(RTM_NEWROUTE, flags, sizeof *r);

    *r = (struct rtmsg){ AF_INET, dst_len, 0, 0, table, proto, scope, type, 0 };
    nl_u32(15, table);                                  /* RTA_TABLE */
    if (dst_len > 0) {
        nl_attr(1, &dst, 4);                            /* RTA_DST */
    }
    if (via != 0) {
        nl_attr(5, &via, 4);                            /* RTA_GATEWAY */
    }
    if (src != 0) {
        nl_attr(7, &src, 4);                            /* RTA_PREFSRC */
    }
    if (via != 0 && table == 254) {
        nl_u32(6, 100);                                 /* RTA_PRIORITY */
    }
    nl_u32(4, oif);                                     /* RTA_OIF */
    nl_end();
}

/* The main table, then the local one, as Linux keeps them. */
static void nl_routes(void) {
    uint32_t lo = IP(127, 0, 0, 1);
    bool eth = card != NULL && state == UP && !down;

    if (eth && gateway != 0) {
        nl_route(NLM_F_MULTI, 254, 1, manual ? 4 : 16, 0, 0, 0, gateway, my_ip, 2);
    }
    if (eth) {
        nl_route(NLM_F_MULTI, 254, 1, 2, 253, my_ip & mask, (uint8_t)prefix(mask), 0, my_ip, 2);
    }
    nl_route(NLM_F_MULTI, 255, 2, 2, 254, IP(127, 0, 0, 0), 8, 0, lo, 1);
    nl_route(NLM_F_MULTI, 255, 2, 2, 254, lo, 32, 0, lo, 1);
    nl_route(NLM_F_MULTI, 255, 3, 2, 253, IP(127, 255, 255, 255), 32, 0, lo, 1);
    if (eth) {
        nl_route(NLM_F_MULTI, 255, 2, 2, 254, my_ip, 32, 0, my_ip, 2);
        nl_route(NLM_F_MULTI, 255, 3, 2, 253, my_ip | ~mask, 32, 0, my_ip, 2);
    }
}

/* ip route get: the way one address would go. */
static int nl_route_to(uint32_t dst) {
    if (loopback(dst) || (dst == my_ip && dst != 0)) {
        nl_route(0, 255, 2, 2, 254, dst, 32, 0, loopback(dst) ? dst : my_ip, 1);
        return 0;
    }
    if (card == NULL || state != UP || down) {
        return -ENETUNREACH;
    }
    uint32_t via = ((dst ^ my_ip) & mask) != 0 ? gateway : 0;

    if (((dst ^ my_ip) & mask) != 0 && gateway == 0) {
        return -ENETUNREACH;
    }
    nl_route(0, 254, 1, 0, 0, dst, 32, via, my_ip, 2);
    return 0;
}

static void nl_neighbours(void) {
    for (unsigned i = 0; i < ARPS; i++) {
        if (arps[i].ip == 0) {
            continue;
        }
        struct ndmsg *n = nl_begin(RTM_NEWNEIGH, NLM_F_MULTI, sizeof *n);

        n->family = AF_INET;
        n->index = 2;
        n->state = 0x02;                                /* NUD_REACHABLE */
        n->type = 1;
        nl_attr(1, &arps[i].ip, 4);                     /* NDA_DST */
        nl_attr(2, arps[i].mac, 6);                     /* NDA_LLADDR */
        nl_end();
    }
}

/* The interface a request names, by index or by IFLA_IFNAME. */
static unsigned nl_index(int32_t index, const uint8_t *attrs, const uint8_t *end) {
    size_t len;
    const uint8_t *name = nl_find(attrs, end, 3, &len);

    if (index > 0) {
        return (unsigned)index;
    }
    if (name != NULL) {
        return strcmp((const char *)name, "lo") == 0 ? 1
             : strcmp((const char *)name, "eth0") == 0 ? 2 : 0;
    }
    return 0;
}

static int nl_change(const struct nlmsghdr *h, const uint8_t *body, const uint8_t *end) {
    size_t len;

    switch (h->type) {
    case RTM_NEWLINK:
    case RTM_SETLINK: {
        const struct ifinfomsg *m = (const struct ifinfomsg *)body;

        if (body + sizeof *m > end) {
            return -EINVAL;
        }
        unsigned index = nl_index(m->index, body + sizeof *m, end);

        if (index == 0) {
            return -ENODEV;
        }
        if (m->change == 0 || (m->change & NET_IFF_UP)) {
            return op_iface_set(index, NET_SET_FLAGS, m->flags);
        }
        return 0;
    }
    case RTM_NEWADDR:
    case RTM_DELADDR: {
        const struct ifaddrmsg *a = (const struct ifaddrmsg *)body;
        const uint8_t *ip;

        if (body + sizeof *a > end || a->family != AF_INET) {
            return -EAFNOSUPPORT;
        }
        if ((ip = nl_find(body + sizeof *a, end, 2, &len)) == NULL &&
            (ip = nl_find(body + sizeof *a, end, 1, &len)) == NULL) {
            return -EINVAL;
        }
        uint32_t addr;

        memcpy(&addr, ip, 4);
        if (h->type == RTM_DELADDR) {
            return a->index == 2 && addr == my_ip ? op_iface_set(2, NET_SET_IP, 0)
                                                  : -EADDRNOTAVAIL;
        }
        int err = op_iface_set(a->index, NET_SET_IP, addr);

        return err != 0 ? err : op_iface_set(a->index, NET_SET_MASK, prefix_mask(a->prefixlen));
    }
    case RTM_NEWROUTE:
    case RTM_DELROUTE: {
        const struct rtmsg *r = (const struct rtmsg *)body;
        const uint8_t *via = nl_find(body + sizeof *r, end, 5, &len);
        uint32_t addr = 0;

        if (body + sizeof *r > end || r->dst_len != 0) {
            return -EOPNOTSUPP;     /* one route of its own: the default */
        }
        if (via != NULL) {
            memcpy(&addr, via, 4);
        }
        if (h->type == RTM_DELROUTE) {
            return gateway != 0 ? op_iface_set(2, NET_SET_GATEWAY, 0) : -ESRCH;
        }
        if (via == NULL) {
            return -EINVAL;
        }
        return op_iface_set(2, NET_SET_GATEWAY, addr);
    }
    }
    return -EOPNOTSUPP;
}

static void nl_route_request(const struct nlmsghdr *h, const uint8_t *body, const uint8_t *end) {
    bool dump = (h->flags & NLM_F_ROOT) != 0;
    uint8_t family = body < end ? body[0] : 0;
    bool inet = family == 0 || family == AF_INET;
    int err = 0;
    size_t len;

    if (dump) {
        for (unsigned i = 1; i <= 2; i++) {
            if (h->type == RTM_GETLINK) {       /* whatever the family */
                nl_link(i, NLM_F_MULTI);
            } else if (inet && h->type == RTM_GETADDR) {
                nl_addr(i, NLM_F_MULTI);
            }
        }
        if (inet && h->type == RTM_GETROUTE) {
            nl_routes();
        } else if (inet && h->type == RTM_GETNEIGH) {
            nl_neighbours();
        }
        nl_done();                  /* and an empty dump for the rest */
        return;
    }
    if (h->type == RTM_GETLINK) {
        const struct ifinfomsg *m = (const struct ifinfomsg *)body;
        unsigned index = body + sizeof *m <= end ? nl_index(m->index, body + sizeof *m, end) : 0;
        struct net_iface f;

        if (index == 0 || !op_iface(index, &f)) {
            err = -ENODEV;
        } else {
            nl_link(index, 0);
        }
    } else if (h->type == RTM_GETROUTE) {
        const uint8_t *dst = nl_find(body + sizeof(struct rtmsg), end, 1, &len);
        uint32_t addr = 0;

        if (dst != NULL && len >= 4) {
            memcpy(&addr, dst, 4);
        }
        err = nl_route_to(addr);
    } else {
        err = nl_change(h, body, end);
    }
    if (err != 0 || (h->flags & NLM_F_ACK)) {
        nl_error(err, h);
    }
}

struct inet_diag_sockid {
    uint16_t sport, dport;
    uint32_t src[4], dst[4];
    uint32_t ifindex, cookie[2];
};

struct inet_diag_req_v2 {
    uint8_t  family, protocol, ext, pad;
    uint32_t states;
    struct inet_diag_sockid id;
};

struct inet_diag_msg {
    uint8_t  family, state, timer, retrans;
    struct inet_diag_sockid id;
    uint32_t expires, rqueue, wqueue, uid, inode;
};

/* A socket's state, numbered as Linux's TCP states are; a datagram socket
   is established once connected, and closed before. */
static uint8_t linux_state(const struct sock *k) {
    static const uint8_t tcp[] = { 7, 10, 2, 3, 1, 4, 5, 8, 11, 9 };

    return k->proto == NET_TCP ? tcp[k->state] : k->peer_ip != 0 ? 1 : 7;
}

static uint32_t local_ip(const struct sock *k) {
    return k->peer_ip != 0 ? source_for(k->peer_ip) : 0;
}

static void nl_diag_request(const struct nlmsghdr *h, const uint8_t *body, const uint8_t *end) {
    const struct inet_diag_req_v2 *r = (const struct inet_diag_req_v2 *)body;

    if (h->type == SOCK_DIAG_BY_FAMILY && body + sizeof *r <= end && r->family == AF_INET) {
        uint8_t want = r->protocol == 255 ? SK_RAW : r->protocol;

        for (unsigned i = 0; i < SOCKETS; i++) {
            const struct sock *k = &socks[i];
            uint8_t st = linux_state(k);

            if (k->proto == 0 || k->proto != want || (r->states & (1u << st)) == 0) {
                continue;
            }
            struct inet_diag_msg *m = nl_begin(SOCK_DIAG_BY_FAMILY, NLM_F_MULTI, sizeof *m);

            m->family = AF_INET;
            m->state = st;
            m->id.sport = k->proto == SK_RAW ? be16(k->ipproto) : k->port;
            m->id.dport = k->peer_port;
            m->id.src[0] = local_ip(k);
            m->id.dst[0] = k->peer_ip;
            m->id.cookie[0] = i + 1;
            m->id.cookie[1] = 0;
            m->rqueue = k->proto == NET_TCP ? k->in_len : 0;
            m->wqueue = k->proto == NET_TCP ? k->out_len : 0;
            m->inode = 10000 + i;
            nl_end();
        }
    }
    nl_done();
}

/* Every message of a request, each answered. */
static int64_t nl_request(struct sock *k, const uint8_t *data, size_t size) {
    size_t at = 0;

    nl = k;
    while (at + sizeof(struct nlmsghdr) <= size) {
        const struct nlmsghdr *h = (const struct nlmsghdr *)(data + at);

        if (h->len < sizeof *h || at + h->len > size) {
            break;
        }
        const uint8_t *body = (const uint8_t *)(h + 1), *end = data + at + h->len;

        nl_seq = h->seq;
        if (k->ipproto == NETLINK_ROUTE) {
            nl_route_request(h, body, end);
        } else if (k->ipproto == NETLINK_SOCK_DIAG) {
            nl_diag_request(h, body, end);
        } else {
            nl_error(-EOPNOTSUPP, h);
        }
        at += (h->len + 3) & ~3u;
    }
    return (int64_t)size;
}

/* ---- /proc/net ------------------------------------------------------------------- */

static char *pr_out;
static size_t pr_max, pr_len;

static void pr(const char *text) {
    for (; *text != '\0'; text++, pr_len++) {
        if (pr_len < pr_max) {
            pr_out[pr_len] = *text;
        }
    }
}

/* text in width, right-aligned if width is positive and left if not. */
static void pr_in(const char *text, int width) {
    int pad = (width < 0 ? -width : width) - (int)strlen(text);

    if (width < 0) {
        pr(text);
    }
    while (pad-- > 0) {
        pr(" ");
    }
    if (width >= 0) {
        pr(text);
    }
}

static void pr_dec(uint64_t v, int width) {
    char text[24], *at = text + sizeof text - 1;

    *at = '\0';
    do {
        *--at = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    pr_in(at, width);
}

static void pr_hex(uint32_t v, unsigned digits) {
    char text[9];

    for (unsigned i = 0; i < digits; i++) {
        text[i] = "0123456789ABCDEF"[v >> (4 * (digits - 1 - i)) & 15];
    }
    text[digits] = '\0';
    pr(text);
}

static void proc_dev(void) {
    struct net_iface f;

    pr("Inter-|   Receive                                                |  Transmit\n"
       " face |bytes    packets errs drop fifo frame compressed multicast|"
       "bytes    packets errs drop fifo colls carrier compressed\n");
    for (unsigned i = 1; op_iface(i, &f); i++) {
        static const int8_t widths[16] = { 8, 7, 4, 4, 4, 5, 10, 9, 8, 7, 4, 4, 4, 5, 7, 10 };
        uint64_t v[16] = { f.rx_bytes, f.rx_packets, 0, f.rx_dropped, 0, 0, 0, 0,
                           f.tx_bytes, f.tx_packets };

        pr_in(f.name, 6);
        pr(":");
        for (unsigned n = 0; n < 16; n++) {
            pr_dec(v[n], widths[n]);
            pr(n < 15 ? " " : "\n");
        }
    }
}

static void route_line(uint32_t dst, uint32_t via, unsigned flags, uint32_t m) {
    pr("eth0\t");
    pr_hex(dst, 8);
    pr("\t");
    pr_hex(via, 8);
    pr("\t");
    pr_hex(flags, 4);
    pr("\t0\t0\t100\t");
    pr_hex(m, 8);
    pr("\t0\t0\t0\n");
}

static void proc_route(void) {
    pr("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    if (card != NULL && state == UP && !down) {
        if (gateway != 0) {
            route_line(0, gateway, 3, 0);                   /* RTF_UP | RTF_GATEWAY */
        }
        route_line(my_ip & mask, 0, 1, mask);
    }
}

static void proc_arp(void) {
    pr("IP address       HW type     Flags       HW address            Mask     Device\n");
    for (unsigned i = 0; i < ARPS; i++) {
        char ip[16], hw[18];

        if (arps[i].ip == 0) {
            continue;
        }
        ip_text(ip, arps[i].ip);
        for (unsigned b = 0; b < 6; b++) {
            hw[3 * b] = "0123456789abcdef"[arps[i].mac[b] >> 4];
            hw[3 * b + 1] = "0123456789abcdef"[arps[i].mac[b] & 15];
            hw[3 * b + 2] = b < 5 ? ':' : '\0';
        }
        pr_in(ip, -16);
        pr(" 0x1         0x2         ");
        pr(hw);
        pr("     *        eth0\n");
    }
}

static void proc_socks(uint8_t proto) {
    unsigned n = 0;

    pr(proto == NET_TCP
       ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt"
         "   uid  timeout inode\n"
       : "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt"
         "   uid  timeout inode ref pointer drops\n");
    for (unsigned i = 0; i < SOCKETS; i++) {
        const struct sock *k = &socks[i];

        if (k->proto == 0 || k->proto != proto) {
            continue;
        }
        pr_dec(n++, proto == NET_TCP ? 4 : 5);
        pr(": ");
        pr_hex(local_ip(k), 8);
        pr(":");
        pr_hex(proto == SK_RAW ? k->ipproto : be16(k->port), 4);
        pr(" ");
        pr_hex(k->peer_ip, 8);
        pr(":");
        pr_hex(be16(k->peer_port), 4);
        pr(" ");
        pr_hex(linux_state(k), 2);
        pr(" ");
        pr_hex(proto == NET_TCP ? k->out_len : 0, 8);
        pr(":");
        pr_hex(k->in_len, 8);
        pr(" 00:00000000 00000000     0        0 ");
        pr_dec(10000 + i, 0);
        pr(proto == NET_TCP ? " 1 0000000000000000 100 0 0 10 0\n"
                            : " 2 0000000000000000 0\n");
    }
}

static void proc_sockstat(void) {
    unsigned tcp = 0, udp = 0, raw = 0;

    for (unsigned i = 0; i < SOCKETS; i++) {
        tcp += socks[i].proto == NET_TCP;
        udp += socks[i].proto == NET_UDP;
        raw += socks[i].proto == SK_RAW;
    }
    pr("sockets: used ");
    pr_dec(tcp + udp + raw, 0);
    pr("\nTCP: inuse ");
    pr_dec(tcp, 0);
    pr(" orphan 0 tw 0 alloc ");
    pr_dec(tcp, 0);
    pr(" mem 0\nUDP: inuse ");
    pr_dec(udp, 0);
    pr(" mem 0\nUDPLITE: inuse 0\nRAW: inuse ");
    pr_dec(raw, 0);
    pr("\nFRAG: inuse 0 memory 0\n");
}

/* The counters netstat -s and ss -s read. Only the current connections
   are counted here. */
static void proc_snmp(void) {
    unsigned estab = 0;

    for (unsigned i = 0; i < SOCKETS; i++) {
        estab += socks[i].proto == NET_TCP && (socks[i].state == ESTABLISHED ||
                                               socks[i].state == CLOSE_WAIT);
    }
    pr("Ip: Forwarding DefaultTTL InReceives InHdrErrors InAddrErrors ForwDatagrams"
       " InUnknownProtos InDiscards InDelivers OutRequests OutDiscards OutNoRoutes"
       " ReasmTimeout ReasmReqds ReasmOKs ReasmFails FragOKs FragFails FragCreates\n"
       "Ip: 2 64 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
       "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails"
       " EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors\n"
       "Tcp: 1 1000 60000 -1 0 0 0 0 ");
    pr_dec(estab, 0);
    pr(" 0 0 0 0 0 0\n"
       "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors"
       " InCsumErrors IgnoredMulti MemErrors\n"
       "Udp: 0 0 0 0 0 0 0 0 0\n");
}

static size_t op_proc(const char *name, char *out, size_t max) {
    pr_out = out;
    pr_max = out != NULL ? max : 0;
    pr_len = 0;
    if (strcmp(name, "dev") == 0) {
        proc_dev();
    } else if (strcmp(name, "route") == 0) {
        proc_route();
    } else if (strcmp(name, "arp") == 0) {
        proc_arp();
    } else if (strcmp(name, "tcp") == 0) {
        proc_socks(NET_TCP);
    } else if (strcmp(name, "udp") == 0) {
        proc_socks(NET_UDP);
    } else if (strcmp(name, "raw") == 0) {
        proc_socks(SK_RAW);
    } else if (strcmp(name, "sockstat") == 0) {
        proc_sockstat();
    } else if (strcmp(name, "snmp") == 0) {
        proc_snmp();
    } else if (strcmp(name, "if_inet6") != 0) {
        return (size_t)-1;          /* which is empty: no IPv6 */
    }
    return pr_len;
}

uint64_t   sock_read(struct handle *h, uint64_t buf, uint64_t count);
uint64_t   sock_write(struct handle *h, uint64_t buf, uint64_t count);
uint64_t   sock_ioctl(struct handle *h, uint64_t request, uint64_t out);
syscall_fn sock_syscall(uint64_t number);

const struct net_ops stack_ops = {
    .socket = op_socket, .hold = op_hold, .drop = op_drop, .close_all = op_close_all,
    .bind = op_bind, .connect = op_connect, .listen = op_listen, .accept = op_accept,
    .shutdown = op_shutdown, .name = op_name, .send = op_send, .recv = op_recv,
    .ready = op_ready, .error = op_error, .memory = op_memory,
    .card_changed = op_card_changed, .setopt = op_setopt, .getopt = op_getopt,
    .pending = op_pending, .iface = op_iface, .iface_set = op_iface_set, .proc = op_proc,
    .syscall = sock_syscall, .read = sock_read, .write = sock_write, .ioctl = sock_ioctl,
    .idle = op_idle,
};

/* ---- the module -------------------------------------------------------------- */

MODULE_EXPORT int module_init(void) {
    uint64_t page = mem_pages(1);

    if (page == 0) {
        return -ENOMEM;
    }
    /* The ARP frame, a packet looping back, the one being sent. */
    arp_buf = (uint8_t *)page;
    loop_buf = (uint8_t *)page + 64;
    tx_buf = (uint8_t *)page + 2048;
    net_register(&stack_ops);
    op_card_changed();              /* one may be there already */
    proc_add(&net_cmd);
    return 0;
}

MODULE_EXPORT int module_exit(void) {
    for (unsigned i = 0; i < SOCKETS; i++) {
        if (socks[i].proto == NET_TCP && socks[i].state >= SYN_RCVD) {
            tcp_send(&socks[i], RST | ACK, socks[i].snd_nxt, NULL, 0);
        }
        if (socks[i].proto != 0) {
            sock_free(&socks[i]);
        }
    }
    proc_remove(&net_cmd);
    net_register(NULL);
    mem_pages_free((uint64_t)arp_buf, 1);
    return 0;
}
