#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

/* What the kernel knows of the network: nothing, until the network/stack
 * module registers these. A socket is a descriptor like any other, so the
 * kernel keeps it, and reads, writes, polls, duplicates and closes it
 * through these; the calls that are only for sockets - socket, connect,
 * sendmsg and the rest - are the module's too, and without it are not
 * there, as on a Linux built without networking.
 *
 * Nothing here waits. A call that would answers -EAGAIN (or -EINPROGRESS),
 * and the syscall does the waiting, on ready(). Addresses and ports are in
 * network order; failures are negated Linux errno values. */

#define NET_UDP  17
#define NET_TCP  6
#define NET_ICMP 1

/* What socket() is asked for besides the three above: a raw socket for the
   IP protocol in the low byte, or a netlink one for the netlink protocol. */
#define NET_RAW     0x100
#define NET_NETLINK 0x200

/* What ready() says. */
#define NET_IN  1                   /* something to read, or the end */
#define NET_OUT 2                   /* room to write */
#define NET_ERR 4                   /* an error is waiting */
#define NET_HUP 8                   /* the connection is over */

/* Where a datagram came from, and what came with it. One off the error
   queue (IP_RECVERR) says what went wrong, and from where: ip and port are
   then where the packet it was about was going. */
struct net_from {
    uint32_t ip;
    uint16_t port;
    uint8_t  ttl;
    bool     err;
    uint8_t  origin, type, code;    /* sock_extended_err's */
    uint32_t errnum, info, offender;
};

/* recv's flags. */
#define NET_PEEK     1
#define NET_ERRQUEUE 2

/* An interface, numbered from 1 as Linux numbers them: lo, then the card. */
struct net_iface {
    char     name[16];
    uint8_t  mac[6];
    uint16_t type;                  /* ARPHRD_: 1 Ethernet, 772 loopback */
    uint32_t flags;                 /* IFF_, below */
    uint32_t ip, mask, broadcast, gateway;
    uint32_t mtu;
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes, rx_dropped;
};

#define NET_IFF_UP        0x1
#define NET_IFF_BROADCAST 0x2
#define NET_IFF_LOOPBACK  0x8
#define NET_IFF_RUNNING   0x40
#define NET_IFF_MULTICAST 0x1000
#define NET_IFF_LOWER_UP  0x10000

/* What iface_set changes. */
enum { NET_SET_IP, NET_SET_MASK, NET_SET_GATEWAY, NET_SET_FLAGS };

struct net_ops {
    int      (*socket)(int proto);          /* its number, one-based */
    void     (*hold)(int s);                /* one more descriptor on it */
    void     (*drop)(int s);                /* one fewer: the last closes it */
    void     (*close_all)(void);            /* what a program left open */
    int      (*bind)(int s, uint32_t ip, uint16_t port);
    int      (*connect)(int s, uint32_t ip, uint16_t port);
    int      (*listen)(int s, int backlog);
    int      (*accept)(int s);              /* a connected socket, or -EAGAIN */
    int      (*shutdown)(int s, int how);
    void     (*name)(int s, bool peer, uint32_t *ip, uint16_t *port);
    int64_t  (*send)(int s, const void *data, size_t size, uint32_t ip, uint16_t port);
    /* The size of what was there, which may be more than size. */
    int64_t  (*recv)(int s, void *data, size_t size, struct net_from *from, unsigned flags);
    unsigned (*ready)(int s);               /* NET_* bits, after looking at the card */
    int      (*error)(int s);               /* SO_ERROR: the pending one, cleared */
    uint32_t (*memory)(void);               /* bytes of RAM it holds */
    void     (*card_changed)(void);         /* net_card came or went */

    /* Socket options, level and name as Linux has them; -ENOPROTOOPT for
       one it does not know. */
    int      (*setopt)(int s, int level, int name, uint32_t value);
    int      (*getopt)(int s, int level, int name, uint32_t *value);
    uint32_t (*pending)(int s);             /* bytes the next read would get */

    /* Interface index, or false past the last; and a change to one. */
    bool     (*iface)(unsigned index, struct net_iface *out);
    int      (*iface_set)(unsigned index, unsigned what, uint32_t value);

    /* /proc/net/<name>: up to max bytes of it into out, returning its whole
       length, or (size_t)-1 if there is no such file. */
    size_t   (*proc)(const char *name, char *out, size_t max);

    /* The socket calls: the handler for a syscall number, or NULL; and
       read, write and ioctl on a socket descriptor. Answers as a syscall's. */
    syscall_fn (*syscall)(uint64_t number);
    uint64_t (*read)(struct handle *h, uint64_t buf, uint64_t count);
    uint64_t (*write)(struct handle *h, uint64_t buf, uint64_t count);
    uint64_t (*ioctl)(struct handle *h, uint64_t request, uint64_t arg);

    /* The machine is idle at the prompt: the moment to find an address, as
       a DHCP client does at boot on Linux, before a program needs one. */
    void     (*idle)(void);
};

extern const struct net_ops *net;

/* Called by the module as it starts, and with NULL as it goes. */
void net_register(const struct net_ops *ops);

/* A network card, as a driver module registers it (network/e1000 and the
 * rest), for the network module to use - the two meet here, and neither
 * needs the other to load. A driver registers as it loads, having only
 * found its card; the card is started when the network is first used. Modules load before the kernel
 * lets the firmware go, and the firmware's own network drivers reset their
 * cards on the way out - so nothing may be set up on one before then.
 *
 * Frames are whole Ethernet frames without the CRC. The calls are made only
 * from inside the stack, never one inside another's frame handling except
 * that input may send - a reply - while poll is running. */

struct net_card {
    const char *name;
    uint8_t     mac[6];
    uint32_t    memory;             /* bytes of RAM its rings and buffers hold,
                                       once started */

    /* Sets the card up and fills in mac; false if it cannot. */
    bool (*start)(void);

    /* Puts one frame on the wire, and returns once the card has it. */
    void (*send)(const uint8_t *frame, size_t len);

    /* Hands every frame that has arrived to input, then returns. */
    void (*poll)(void (*input)(const uint8_t *frame, size_t len));
};

/* The card, if a driver has registered one: only one at a time. Returns
   -EBUSY while another is registered. NULL takes this one away again. */
extern const struct net_card *net_card;
int net_card_register(const struct net_card *card);
