/* Plain HTTP over a socket, and the one DNS query it needs, to the
 * nameserver in /etc/resolv.conf.
 *
 * As apt does it: the name is looked up once, and one connection carries
 * every request to the same server - HTTP/1.1, each body read to its
 * Content-Length so the next request can follow on the same socket. A
 * hundred packages were a hundred lookups and a hundred connections, and
 * the setting up was most of what fetching small ones cost. A server that
 * answers in chunks, or without a length, is asked again over HTTP/1.0,
 * whose body runs to the close. */

#include "tuxpac.h"

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

unsigned http_status;

struct sockaddr_in {
    uint16_t family, port;
    uint32_t ip;
    uint8_t  zero[8];
};

static uint16_t be16(unsigned v) {
    return (uint16_t)((v >> 8 & 0xFF) | (v & 0xFF) << 8);
}

/* Whether s is ready for events (POLLIN, POLLOUT) within ms. */
static bool wait_for(int s, short events, int ms) {
    struct pollfd p = { s, events, 0 };

    return sys_poll(&p, 1, ms) > 0;
}

static bool parse_ip(const char *s, uint32_t *ip) {
    uint32_t v = 0, n = 0;
    unsigned parts = 0;
    bool digit = false;

    for (;; s++) {
        if (*s >= '0' && *s <= '9' && n < 256) {
            n = n * 10 + (uint32_t)(*s - '0');
            digit = true;
        } else if ((*s == '.' || *s == '\0') && digit && n < 256 && parts < 4) {
            v |= n << (8 * parts++);
            n = 0;
            digit = false;
            if (*s == '\0') {
                break;
            }
        } else {
            return false;
        }
    }
    *ip = v;
    return parts == 4;
}

static uint32_t nameserver(void) {
    static char t[1024];
    char word[16];
    uint32_t ip = 0;
    long fd = sys_open("/etc/resolv.conf", O_RDONLY, 0), size;

    if (fd < 0) {
        return 0;
    }
    size = sys_read((int)fd, t, sizeof t - 1);
    sys_close((int)fd);
    for (long i = 0; i + 11 < size; i++) {
        if ((i == 0 || t[i - 1] == '\n') && memcmp_n(t + i, "nameserver", 10)) {
            long j = i + 10, n = 0;

            while (j < size && (t[j] == ' ' || t[j] == '\t')) {
                j++;
            }
            while (j < size && n < (long)sizeof word - 1 && t[j] > ' ') {
                word[n++] = t[j++];
            }
            word[n] = '\0';
            if (parse_ip(word, &ip)) {
                return ip;
            }
        }
    }
    return 0;
}

static unsigned skip_name(const uint8_t *p, unsigned at, unsigned end) {
    while (at < end) {
        if (p[at] == 0) {
            return at + 1;
        }
        if ((p[at] & 0xC0) == 0xC0) {
            return at + 2;
        }
        at += p[at] + 1u;
    }
    return end;
}

static bool resolve(const char *host, uint32_t *ip) {
    static uint8_t q[300], a[512];
    static char last[128];
    static uint32_t last_ip;
    unsigned n = 12;
    bool found = false;

    if (parse_ip(host, ip)) {
        return true;
    }
    if (last[0] != '\0' && strcmp(last, host) == 0) {
        *ip = last_ip;              /* asked a moment ago */
        return true;
    }
    long u = sys_socket(AF_INET, SOCK_DGRAM, 0);   /* brings the network up first */
    struct sockaddr_in ns = { AF_INET, be16(53), u >= 0 ? nameserver() : 0, { 0 } };

    memset(q, 0, 12);
    q[0] = 0x74;
    q[1] = 0x70;
    q[2] = 1;                               /* recursion desired */
    q[5] = 1;                               /* one question */
    for (const char *h = host; *h != '\0' && n < 280;) {
        const char *dot = strchr(h, '.');
        unsigned l = dot != NULL ? (unsigned)(dot - h) : (unsigned)strlen(h);

        if (l == 0 || l > 63 || n + l > 280) {
            break;
        }
        q[n++] = (uint8_t)l;
        memcpy(q + n, h, l);
        n += l;
        h += l + (dot != NULL);
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1;                             /* A */
    q[n++] = 0;
    q[n++] = 1;                             /* IN */

    for (int tries = 0; ns.ip != 0 && tries < 3 && !found; tries++) {
        sys_sendto((int)u, q, n, &ns, sizeof ns);
        if (!wait_for((int)u, POLLIN, 2000)) {
            continue;
        }
        long got = sys_recv((int)u, a, sizeof a);

        if (got < 12 || a[0] != q[0] || a[1] != q[1]) {
            continue;
        }
        unsigned end = (unsigned)got;
        unsigned answers = (unsigned)a[6] << 8 | a[7];
        unsigned at = skip_name(a, 12, end) + 4;

        while (answers-- > 0 && at + 10 <= end) {
            at = skip_name(a, at, end);
            if (at + 10 > end) {
                break;
            }
            unsigned type = (unsigned)a[at] << 8 | a[at + 1];
            unsigned len = (unsigned)a[at + 8] << 8 | a[at + 9];

            at += 10;
            if (type == 1 && len == 4 && at + 4 <= end) {
                memcpy(ip, a + at, 4);
                found = true;
                break;
            }
            at += len;
        }
    }
    if (u >= 0) {
        sys_close((int)u);
    }
    if (found && strlen(host) < sizeof last) {
        strcpy(last, host);
        last_ip = *ip;
    }
    return found;
}

/* What arrived, up to cap bytes, waiting up to 30 s for some; 0 at the end. */
static uint32_t receive(int s, uint8_t *to, uint32_t cap) {
    long r = wait_for(s, POLLIN, 30000) ? sys_recv(s, to, cap) : 0;

    return r > 0 ? (uint32_t)r : 0;
}

static uint32_t sock_fill(struct source *b) {
    b->pos = 0;
    b->len = b->left == 0 ? 0 : receive(b->fd, b->buf, b->left < b->cap ? (uint32_t)b->left : b->cap);
    if (b->left != ~(uint64_t)0) {
        b->left -= b->len;
    }
    return b->len;
}

/* The connection kept open for the next request, and where it goes. */
static int kept_fd = -1;
static bool keep_open;            /* the response being read may leave it open */
static char kept_host[128];
static uint32_t kept_port;

static void kept_drop(void) {
    if (kept_fd >= 0) {
        sys_close(kept_fd);
    }
    kept_fd = -1;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* The value of header name (lower case, colon included) in the head, or NULL. */
static const char *header(const char *head, const char *name) {
    for (const char *line = head; line != NULL; line = strchr(line, '\n')) {
        size_t i = 0;

        line += *line == '\n';
        while (name[i] != '\0' && lower(line[i]) == name[i]) {
            i++;
        }
        if (name[i] == '\0') {
            line += i;
            while (*line == ' ') {
                line++;
            }
            return line;
        }
    }
    return NULL;
}

bool http_open(const char *url, struct source *b) {
    static char where[384], host[128], next[384];

    if (strlen(url) >= sizeof where) {
        fail("tuxpac: %s: too long\n", url);
        return false;
    }
    strcpy(where, url);
    http_status = 0;
    b->fd = -1;
    for (int hops = 0; hops < 5; hops++) {
        const char *p = where + 7, *path;
        uint32_t ip, port = 80;
        size_t n = 0;

        if (!memcmp_n(where, "http://", 7)) {
            fail("tuxpac: %s: only http:// works here\n", where);
            return false;
        }
        while (*p != '\0' && *p != '/' && *p != ':' && n < sizeof host - 1) {
            host[n++] = *p++;
        }
        host[n] = '\0';
        if (*p == ':') {
            for (port = 0, p++; *p >= '0' && *p <= '9'; p++) {
                port = port * 10 + (uint32_t)(*p - '0');
            }
        }
        path = *p == '/' ? p : "/";

        bool reused = false, old = false;
        long s;
        uint32_t got = 0, end = 0;

    again:
        got = end = 0;
        if (!old && kept_fd >= 0 && kept_port == port && strcmp(kept_host, host) == 0) {
            s = kept_fd;            /* the connection the last request left open */
            kept_fd = -1;
            reused = true;
        } else {
            kept_drop();
            reused = false;
            s = sys_socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) {
                fail("tuxpac: no network\n");
                return false;
            }
            if (!resolve(host, &ip)) {
                fail("tuxpac: %s: cannot resolve\n", host);
                sys_close((int)s);
                return false;
            }
            struct sockaddr_in to = { AF_INET, be16(port), ip, { 0 } };

            if (sys_connect((int)s, &to, sizeof to) < 0) {
                fail("tuxpac: %s: cannot connect\n", host);
                sys_close((int)s);
                return false;
            }
        }
        b->fd = (int)s;
        keep_open = false;
        format((char *)b->buf, old ? "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: tuxpac\r\n\r\n"
                                   : "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: tuxpac\r\n"
                                     "Connection: keep-alive\r\n\r\n",
               path, host);
        bool sent_all = true;

        for (size_t sent = 0, len = strlen((char *)b->buf); sent < len;) {
            long k = sys_write((int)s, b->buf + sent, len - sent);

            if (k <= 0) {
                sent_all = false;
                break;
            }
            sent += (size_t)k;
        }
        /* The head, whole, then whatever of the body came with it. */
        while (sent_all && end == 0) {
            uint32_t k = got < b->cap - 1 ? receive((int)s, b->buf + got, b->cap - 1 - got) : 0;

            if (k == 0) {
                break;
            }
            for (uint32_t i = got >= 3 ? got - 3 : 0; i + 3 < got + k && end == 0; i++) {
                if (memcmp_n((char *)b->buf + i, "\r\n\r\n", 4)) {
                    end = i + 4;
                }
            }
            got += k;
        }
        if (end == 0) {
            http_close(b);
            if (reused) {
                goto again;         /* the server had let the old one go */
            }
            fail("tuxpac: %s: %s\n", host, sent_all ? "no answer" : "cannot send");
            return false;
        }
        char *head = (char *)b->buf;
        unsigned status_code = 0;

        head[end - 1] = '\0';
        for (const char *c = strchr(head, ' '); c != NULL && *++c >= '0' && *c <= '9';) {
            status_code = status_code * 10 + (unsigned)(*c - '0');
        }
        const char *length = header(head, "content-length:");
        const char *coding = header(head, "transfer-encoding:");
        const char *conn = header(head, "connection:");
        const char *loc = header(head, "location:");
        uint64_t body = 0;

        for (const char *c = length; c != NULL && *c >= '0' && *c <= '9'; c++) {
            body = body * 10 + (uint64_t)(*c - '0');
        }
        if (!old && (length == NULL || (coding != NULL && memcmp_n(coding, "chunked", 7)))) {
            http_close(b);          /* no length to stop at: asked the old way */
            old = true;
            goto again;
        }

        if (status_code >= 300 && status_code < 400 && loc != NULL) {
            size_t k = 0;

            if (*loc == '/') {
                format(next, "http://%s:%u", host, port);
                k = strlen(next);
            }
            for (const char *c = loc; *c > ' ' && k < sizeof next - 1; c++) {
                next[k++] = *c;
            }
            next[k] = '\0';
            strcpy(where, next);
            http_close(b);
            continue;
        }
        if (status_code != 200) {
            http_status = status_code;
            http_close(b);
            return false;
        }
        b->pos = end;
        b->len = got;
        b->left = ~(uint64_t)0;
        if (!old) {
            uint64_t have = got - end;

            if (have > body) {
                b->len = end + (uint32_t)body;
                have = body;
            }
            b->left = body - have;
            keep_open = conn == NULL || !memcmp_n(conn, "close", 5);
            if (keep_open && strlen(host) < sizeof kept_host) {
                strcpy(kept_host, host);
                kept_port = port;
            } else {
                keep_open = false;
            }
        }
        b->count = 0;
        b->bad = false;
        b->fill = sock_fill;
        return true;
    }
    fail("tuxpac: %s: too many redirects\n", url);
    return false;
}

/* Done with a response: its connection kept for the next request if it
   was read to the end and the server keeps it open too, else closed. */
void http_close(struct source *b) {
    if (b->fd >= 0 && keep_open && b->left == 0) {
        kept_drop();
        kept_fd = b->fd;
    } else if (b->fd >= 0) {
        sys_close(b->fd);
    }
    keep_open = false;
    b->fd = -1;
}
