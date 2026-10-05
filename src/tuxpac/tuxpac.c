/* tuxpac: installs and manages Debian packages.
 *
 *   tuxpac -y              sync the package list
 *   tuxpac -s <pkg>...     install, with what they depend on
 *   tuxpac -e              install Debian's essential packages
 *   tuxpac -re             remove them, but for what other packages need
 *   tuxpac -r <pkg>        remove, with dependencies nothing else needs
 *   tuxpac -n <pkg>        the same, configuration files too
 *   tuxpac -u [pkg]        upgrade everything, or one
 *   tuxpac -f <text>       find packages in the list
 *   tuxpac -q [text]       installed packages
 *   tuxpac -i <pkg>        about one
 *
 * /etc/tuxlet/mirror has a line per source, as sources.list has them
 * ("deb" in front or not): http://deb.debian.org/debian trixie main.
 * An https:// one is fetched by /usr/bin/curl, so it works once curl and
 * ca-certificates are installed - over http.
 *
 * Everything stays on the disk. -y streams each Packages.gz (or .xz, where
 * a suite has only that) into /var/lib/tuxpac/index, a line per package,
 * of only what tuxpac reads; a lookup reads that through, and a dependency
 * tree is resolved a level a pass. /var/lib/tuxpac/installed has a line per package installed,
 * <name>.list beside it the paths it put there ('*' before a conffile).
 *
 * Maintainer scripts run as dpkg runs them: each package's preinst, postinst,
 * prerm and postrm are kept as <name>.<script> beside its list; preinst runs
 * before its files go in, postinst once a command has unpacked everything
 * it installs, and file and named triggers after that. /var/lib/dpkg/status
 * says what is installed, so dpkg-query and the scripts that ask it agree.
 *
 * Not done: versions in dependencies, signatures, zstd.
 * Documentation, man pages and translations are not unpacked. */

#include "tuxpac.h"

#define MIRROR  "/etc/tuxlet/mirror"
#define LIB     "/var/lib/tuxpac"
#define INDEX   LIB "/index"
#define DB      LIB "/installed"
#define CACHE   "/var/cache/tuxpac"
#define DEB     CACHE "/package.deb"
#define LIST    CACHE "/Packages"   /* a package list curl fetched */
#define CURL    "/usr/bin/curl"
#define CERTS   "/usr/share/ca-certificates/mozilla"
#define BUNDLE  "/etc/ssl/certs/ca-certificates.crt"
#define STATUS  "/var/lib/dpkg/status"
#define ALTS    "/usr/bin/update-alternatives"

#define FBUF    65536               /* reading the disk, and downloads */
#define NBUF    16384               /* the socket */
#define LINE    5120                /* a line of the index */
#define MIRRORS 8
#define PLAN    256                 /* packages one command installs */
#define WANTS   512                 /* dependencies one round looks for */
#define VER     40
#define WTEXT   16384
#define NAMES   8192
#define CONF    4096
#define POST    16384               /* a postinst, for its alternatives */
#define PATH    256                 /* a path */

/* Index fields, tab-separated. F_ESS is "e" for a package Debian marks
   Essential - one every system is taken to have, never depended on. */
enum { F_NAME, F_VER, F_MIRROR, F_SIZE, F_ISIZE, F_FILE, F_DEPS, F_PROV, F_DESC, F_ESS, FIELDS };

static uint8_t      *fbuf, *nbuf;
static struct source fsrc, nsrc;
static char          line[LINE], tmp[LINE];

/* ---- text ------------------------------------------------------------------ */

/* How far s goes before one of stops, or its end. */
static uint32_t span(const char *s, const char *stops) {
    uint32_t n = 0;

    while (s[n] != '\0' && strchr(stops, s[n]) == NULL) {
        n++;
    }
    return n;
}

static bool same(const char *a, uint32_t an, const char *b, uint32_t bn) {
    return an == bn && memcmp_n(a, b, an);
}

/* Whether name is in list: names between commas, up to a tab or the end. */
static bool listed(const char *list, const char *name, uint32_t n) {
    while (*list != '\0' && *list != '\t') {
        uint32_t k = span(list, ",\t");

        if (same(list, k, name, n)) {
            return true;
        }
        list += k + (list[k] == ',');
    }
    return false;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool contains(const char *s, const char *what) {
    for (; *s != '\0'; s++) {
        size_t i = 0;

        while (what[i] != '\0' && lower(s[i]) == lower(what[i])) {
            i++;
        }
        if (what[i] == '\0') {
            return true;
        }
    }
    return *what == '\0';
}

static char *cat(char *p, const char *s) {
    size_t n = strlen(s);

    memcpy(p, s, n);
    return p + n;
}

static uint32_t number(const char *s) {
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s++ - '0');
    }
    return v;
}

/* ---- versions, as dpkg orders them ---------------------------------------- */

static int order(char c) {
    return c >= '0' && c <= '9' ? 0 : (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ? c :
           c == '~' ? -1 : c != '\0' ? c + 256 : 0;
}

static bool digit(char c) {
    return c >= '0' && c <= '9';
}

static int verrevcmp(const char *a, const char *b) {
    while (*a != '\0' || *b != '\0') {
        int first = 0;

        while ((*a != '\0' && !digit(*a)) || (*b != '\0' && !digit(*b))) {
            if (order(*a) != order(*b)) {
                return order(*a) - order(*b);
            }
            a++;
            b++;
        }
        while (*a == '0') {
            a++;
        }
        while (*b == '0') {
            b++;
        }
        while (digit(*a) && digit(*b)) {
            first = first != 0 ? first : *a - *b;
            a++;
            b++;
        }
        if (digit(*a) || digit(*b)) {
            return digit(*a) ? 1 : -1;
        }
        if (first != 0) {
            return first;
        }
    }
    return 0;
}


/* Above 0 if version a is newer than b: epoch, then upstream, then the
   Debian revision after the last '-'. */
static int vcmp(const char *a, const char *b) {
    char up[2][VER];
    const char *v[2] = { a, b }, *rev[2];
    uint32_t epoch[2];

    for (int i = 0; i < 2; i++) {
        const char *colon = strchr(v[i], ':'), *dash;
        size_t n;

        epoch[i] = colon != NULL ? number(v[i]) : 0;
        v[i] = colon != NULL ? colon + 1 : v[i];
        dash = strrchr(v[i], '-');
        n = dash != NULL ? (size_t)(dash - v[i]) : strlen(v[i]);
        n = n < VER - 1 ? n : VER - 1;
        memcpy(up[i], v[i], n);
        up[i][n] = '\0';
        rev[i] = dash != NULL ? dash + 1 : "";
    }
    if (epoch[0] != epoch[1]) {
        return epoch[0] > epoch[1] ? 1 : -1;
    }
    int r = verrevcmp(up[0], up[1]);

    return r != 0 ? r : verrevcmp(rev[0], rev[1]);
}

/* The version field of an index line, into out. */
static void version_of(const char *l, char *out) {
    const char *v = l + span(l, "\t");
    uint32_t n;

    v += *v == '\t';
    n = span(v, "\t");
    n = n < VER - 1 ? n : VER - 1;
    memcpy(out, v, n);
    out[n] = '\0';
}

/* Cuts a line into its tab-separated fields. */
static void split(char *l, char **f, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        f[i] = l;
        l += span(l, "\t");
        if (*l != '\0') {
            *l++ = '\0';
        }
    }
}

/* ---- files ----------------------------------------------------------------- */

/* A file open for reading, and its size; a negated errno if it is not
   there. */
static int open_read(const char *path, uint32_t *size) {
    struct stat st;
    long fd = sys_open(path, O_RDONLY, 0);

    if (fd >= 0 && sys_fstat((int)fd, &st) < 0) {
        sys_close((int)fd);
        fd = -ENOENT;
    }
    *size = fd >= 0 ? (uint32_t)st.size : 0;
    return (int)fd;
}

/* A whole file, NUL after it, or NULL. */
static char *load(const char *path, uint32_t *size) {
    int fd = open_read(path, size);
    char *t = fd >= 0 ? xalloc(*size + 1) : NULL;

    if (t != NULL && *size > 0 && sys_pread(fd, t, *size, 0) != (long)*size) {
        xfree(t);
        t = NULL;
    }
    if (fd >= 0) {
        sys_close(fd);
    }
    return t;
}

static bool exists(const char *path) {
    struct stat st;

    return sys_lstat(path, &st) == 0;
}

/* Every folder down to path, path included. */
static void mkdirs(const char *path) {
    char p[PATH];
    size_t n = strlen(path);

    if (n >= sizeof p) {
        return;
    }
    strcpy(p, path);
    for (size_t i = 1; i <= n; i++) {
        if (p[i] == '/' || p[i] == '\0') {
            p[i] = '\0';
            sys_mkdir(p);
            p[i] = path[i];
        }
    }
}

static void mkparent(const char *path) {
    char p[PATH];
    char *slash;

    if (strlen(path) < sizeof p) {
        strcpy(p, path);
        if ((slash = strrchr(p, '/')) != NULL && slash != p) {
            *slash = '\0';
            mkdirs(p);
        }
    }
}

/* A file written from the front, a piece at a time. */
struct out {
    int      fd;
    uint32_t done;
    long     err;
};

static void out_open(struct out *o, const char *path) {
    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    o->fd = (int)fd;
    o->done = 0;
    o->err = fd < 0 ? fd : 0;
}

static void out_put(struct out *o, const void *d, uint32_t n) {
    while (o->err == 0 && n > 0) {
        long k = sys_write(o->fd, d, n);

        if (k <= 0) {
            o->err = k < 0 ? k : -28;
            break;
        }
        o->done += (uint32_t)k;
        d = (const char *)d + k;
        n -= (uint32_t)k;
    }
}

static long out_close(struct out *o) {
    if (o->fd >= 0) {
        sys_close(o->fd);
    }
    o->fd = -1;
    return o->err;
}

/* A whole file written at once. */
static long write_file(const char *path, const void *d, uint32_t n) {
    struct out o;

    out_open(&o, path);
    out_put(&o, d, n);
    return out_close(&o);
}

/* ---- mirrors --------------------------------------------------------------- */

static struct mirror {
    char *url, *suite, *comps;
} mirrors[MIRRORS];
static unsigned nmirrors;

static bool load_mirrors(void) {
    uint32_t size;
    char *t = load(MIRROR, &size);

    nmirrors = 0;
    if (t == NULL) {
        fail("tuxpac: no " MIRROR "\n");
        return false;
    }
    for (char *l = t; *l != '\0' && nmirrors < MIRRORS;) {
        char *end = l + span(l, "\n"), *rest = l, *url;
        bool last = *end == '\0';

        *end = '\0';
        for (char *c = l; c < end; c++) {
            if (*c == '\t' || *c == '\r') {
                *c = ' ';
            }
        }
        while (*rest == ' ') {
            rest++;
        }
        url = str_word(&rest);
        if (strcmp(url, "deb") == 0) {
            url = str_word(&rest);
        }
        if (*url != '\0' && *url != '#') {
            struct mirror *m = &mirrors[nmirrors];
            size_t n = strlen(url);

            while (n > 0 && url[n - 1] == '/') {
                url[--n] = '\0';
            }
            m->url = url;
            m->suite = str_word(&rest);
            m->comps = *rest != '\0' && *rest != '#' ? rest : "main";
            if (*m->suite != '\0') {
                nmirrors++;
            }
        }
        l = last ? end : end + 1;
    }
    if (nmirrors == 0) {
        fail("tuxpac: no mirror in " MIRROR "\n");
    }
    return nmirrors > 0;
}

/* ---- https, through curl -------------------------------------------------- */

static bool https(const char *url) {
    return memcmp_n(url, "https://", 8);
}

/* curl fetches url into to. Its exit code 22 is an HTTP error, which the
   caller hears of as http_status 404 - as -y wants it, for Packages.xz. */
static bool curl(const char *url, const char *to) {
    const char *argv[] = { CURL, "-fsLo", to, url, NULL };
    int code;

    if (!exists(CURL)) {
        fail("tuxpac: https needs curl and ca-certificates, installed over http\n");
        return false;
    }
    mkdirs(CACHE);
    http_status = 0;
    if ((code = run(CURL, argv)) == 22) {
        http_status = 404;
    } else if (code != 0) {
        fail("tuxpac: %s: curl failed (%u)\n", url, (unsigned)(code < 0 ? -code : code));
    }
    return code == 0;
}

/* The body of url in s: off the socket for http, from curl's file for
   https - which is s's own buffer then, so fbuf stays free. */
static bool open_url(const char *url, struct source *s) {
    uint32_t size;
    int fd;

    if (!https(url)) {
        return http_open(url, s);
    }
    if (!curl(url, LIST) || (fd = open_read(LIST, &size)) < 0) {
        return false;
    }
    src_file(s, fd, 0, size);
    return true;
}

static void close_url(const char *url, struct source *s) {
    if (https(url)) {
        sys_close(s->fd);
        sys_unlink(LIST);
    } else {
        http_close(s);
    }
}

/* ---- -y: the package list -------------------------------------------------- */

static char r_name[128], r_ver[128], r_file[256], r_size[16], r_isize[16];
static char r_deps[3072], r_prov[1024], r_desc[160], r_ess[8];
static unsigned   r_mirror;
static uint32_t   r_count, linelen, ixlen;
static struct out ix;

static void ix_put(const char *s) {
    for (; *s != '\0'; s++) {
        if (ixlen == FBUF) {
            out_put(&ix, fbuf, ixlen);
            ixlen = 0;
        }
        fbuf[ixlen++] = (uint8_t)*s;
    }
}

static void take(char *to, size_t cap, const char *v) {
    size_t n = 0;

    while (*v == ' ') {
        v++;
    }
    while (*v != '\0' && *v != '\r' && n < cap - 1) {
        to[n++] = *v++;
    }
    to[n] = '\0';
}

/* A Depends-style list without versions, qualifiers, spaces or restrictions:
   "libc6 (>= 2.34), libfoo:any | libbar" becomes "libc6,libfoo|libbar". */
static void deps_add(char *to, size_t cap, const char *v) {
    size_t n = strlen(to);
    char close = 0;
    bool qualifier = false;

    if (n > 0 && n + 1 < cap) {
        to[n++] = ',';
    }
    for (; *v != '\0' && *v != '\r' && n + 1 < cap; v++) {
        char c = *v;

        if (close != 0) {
            close = c == close ? 0 : close;
            continue;
        }
        if (c == '(' || c == '[' || c == '<') {
            close = c == '(' ? ')' : c == '[' ? ']' : '>';
            continue;
        }
        if (c == ' ') {
            continue;
        }
        if (c == ',' || c == '|') {
            qualifier = false;
        } else if (c == ':') {
            qualifier = true;
        }
        if (!qualifier) {
            to[n++] = c;
        }
    }
    while (n > 0 && (to[n - 1] == ',' || to[n - 1] == '|')) {
        n--;
    }
    to[n] = '\0';
}

static void record_end(void) {
    if (r_name[0] != '\0' && r_file[0] != '\0') {
        char m[2] = { (char)('0' + r_mirror), '\0' };
        const char *f[FIELDS] = { r_name, r_ver, m, r_size, r_isize, r_file, r_deps, r_prov, r_desc,
                                  strcmp(r_ess, "yes") == 0 ? "e" : "" };

        for (unsigned i = 0; i < FIELDS; i++) {
            ix_put(f[i]);
            ix_put(i + 1 < FIELDS ? "\t" : "\n");
        }
        r_count++;
    }
    r_name[0] = r_ver[0] = r_file[0] = r_size[0] = r_isize[0] = '\0';
    r_deps[0] = r_prov[0] = r_desc[0] = r_ess[0] = '\0';
}

static const char *field(const char *l, const char *name) {
    size_t n = strlen(name);

    return memcmp_n(l, name, n) ? l + n : NULL;
}

static void record_line(void) {
    const char *v;

    if (linelen == 0) {
        record_end();
    } else if (line[0] == ' ') {
        /* the rest of a description */
    } else if ((v = field(line, "Package:")) != NULL) {
        take(r_name, sizeof r_name, v);
    } else if ((v = field(line, "Version:")) != NULL) {
        take(r_ver, sizeof r_ver, v);
    } else if ((v = field(line, "Filename:")) != NULL) {
        take(r_file, sizeof r_file, v);
    } else if ((v = field(line, "Size:")) != NULL) {
        take(r_size, sizeof r_size, v);
    } else if ((v = field(line, "Installed-Size:")) != NULL) {
        take(r_isize, sizeof r_isize, v);
    } else if ((v = field(line, "Description:")) != NULL) {
        take(r_desc, sizeof r_desc, v);
    } else if ((v = field(line, "Depends:")) != NULL || (v = field(line, "Pre-Depends:")) != NULL) {
        deps_add(r_deps, sizeof r_deps, v);
    } else if ((v = field(line, "Provides:")) != NULL) {
        deps_add(r_prov, sizeof r_prov, v);
    } else if ((v = field(line, "Essential:")) != NULL) {
        take(r_ess, sizeof r_ess, v);
    }
}

static bool index_sink(const uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (d[i] == '\n') {
            line[linelen] = '\0';
            record_line();
            linelen = 0;
        } else if (linelen < LINE - 1) {
            line[linelen++] = d[i] == '\t' ? ' ' : (char)d[i];
        }
    }
    return ix.err == 0;
}

static void sync(void) {
    static char url[512], comps[128];
    bool ok = true;

    if (!load_mirrors()) {
        return;
    }
    mkdirs(LIB);
    out_open(&ix, INDEX ".new");
    ixlen = 0;
    for (unsigned m = 0; m < nmirrors && ok && ix.err == 0; m++) {
        char *rest = comps, *c;

        if (strlen(mirrors[m].comps) >= sizeof comps) {
            fail("tuxpac: " MIRROR ": line too long\n");
            ok = false;
            break;
        }
        strcpy(comps, mirrors[m].comps);
        for (c = str_word(&rest); *c != '\0' && *c != '#' && ok; c = str_word(&rest)) {
            if (strlen(mirrors[m].url) + strlen(mirrors[m].suite) + strlen(c) + 48 > sizeof url) {
                fail("tuxpac: " MIRROR ": line too long\n");
                ok = false;
                break;
            }
            /* .gz if there is one: its window is 32 KiB, xz's megabytes. */
            format(url, "%s/dists/%s/%s/binary-amd64/Packages.gz", mirrors[m].url, mirrors[m].suite, c);
            bool gz = open_url(url, &nsrc), xz = false;

            if (!gz && http_status == 404) {
                url[strlen(url) - 2] = 'x';             /* Packages.xz */
                xz = open_url(url, &nsrc);
            }
            if (!gz && !xz) {
                if (http_status != 0) {
                    fail("tuxpac: %s: HTTP %u\n", url, http_status);
                }
                ok = false;
                break;
            }
            r_mirror = m;
            r_count = linelen = 0;
            record_end();
            ok = gz ? gunzip(&nsrc, index_sink) : unxz(&nsrc, 0, index_sink);
            close_url(url, &nsrc);
            line[linelen] = '\0';
            record_line();
            record_end();
            if (!ok && ix.err == 0) {
                fail("tuxpac: %s: damaged\n", url);
            }
            if (ok) {
                print("%s/%s %u\n", mirrors[m].suite, c, r_count);
            }
        }
    }
    if (ok) {
        out_put(&ix, fbuf, ixlen);
    }
    if (out_close(&ix) != 0) {
        fail("tuxpac: " INDEX ": %s\n", errstr(ix.err));
        ok = false;
    }
    if (ok) {
        ok = sys_rename(INDEX ".new", INDEX) == 0;
    }
    if (!ok) {
        sys_unlink(INDEX ".new");
    }
}

/* ---- reading the list ------------------------------------------------------ */

static int      ixfd = -1;
static uint32_t ixsize;

/* Every line of the index to each, with where it starts, until each says
   false. False if there is no index, or Ctrl-C. */
static bool scan(bool (*each)(char *l, uint32_t at)) {
    uint32_t at = 0, start = 0, n = 0;

    if (ixfd < 0 && (ixfd = open_read(INDEX, &ixsize)) < 0) {
        fail("tuxpac: no package list, run tuxpac -y\n");
        return false;
    }
    src_file(&fsrc, ixfd, 0, ixsize);
    for (;;) {
        char c = (char)src_byte(&fsrc);

        if (fsrc.bad) {
            return true;
        }
        at++;
        if (c != '\n') {
            if (n < LINE - 1) {
                line[n++] = c;
            }
            continue;
        }
        line[n] = '\0';
        n = 0;
        if (!each(line, start)) {
            return true;
        }
        start = at;
    }
}

static char *read_line(uint32_t at, char *to) {
    uint32_t n = 0;

    src_file(&fsrc, ixfd, at, ixsize);
    for (;;) {
        char c = (char)src_byte(&fsrc);

        if (fsrc.bad || c == '\n') {
            break;
        }
        if (n < LINE - 1) {
            to[n++] = c;
        }
    }
    to[n] = '\0';
    return to;
}

/* ---- what is installed ----------------------------------------------------- */

static struct inst {
    char *name, *ver, *deps, *prov;
    char  flag;                     /* 'm' asked for, 'a' pulled in */
    bool  gone, mark;
} *db;
static uint32_t ndb;
static char    *dbtext;

static bool db_load(void) {
    uint32_t size, lines = 0;

    xfree(dbtext);
    xfree(db);
    ndb = 0;
    dbtext = load(DB, &size);
    for (uint32_t i = 0; i < size; i++) {
        lines += dbtext[i] == '\n';
    }
    if ((db = xalloc((lines + 1) * sizeof *db)) == NULL) {
        return false;
    }
    for (char *l = dbtext; l != NULL && *l != '\0';) {
        char *end = l + span(l, "\n"), *f[5];
        bool last = *end == '\0';

        *end = '\0';
        split(l, f, 5);
        if (*f[0] != '\0') {
            db[ndb++] = (struct inst){ f[0], f[1], f[3], f[4], f[2][0], false, false };
        }
        l = last ? NULL : end + 1;
    }
    return true;
}

static void status_write(void);

/* Writes it back, gone ones left out and extra added, and reads it again. */
static bool db_save(const char *extra) {
    size_t n = extra != NULL ? strlen(extra) : 0;
    char *t, *p;

    for (uint32_t i = 0; i < ndb; i++) {
        n += strlen(db[i].name) + strlen(db[i].ver) + strlen(db[i].deps) + strlen(db[i].prov) + 7;
    }
    if ((t = p = xalloc(n + 1)) == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < ndb; i++) {
        if (!db[i].gone) {
            p = cat(p, db[i].name);
            *p++ = '\t';
            p = cat(p, db[i].ver);
            *p++ = '\t';
            *p++ = db[i].flag;
            *p++ = '\t';
            p = cat(p, db[i].deps);
            *p++ = '\t';
            p = cat(p, db[i].prov);
            *p++ = '\n';
        }
    }
    if (extra != NULL) {
        p = cat(p, extra);
    }
    mkdirs(LIB);
    long err = write_file(DB, t, (uint32_t)(p - t));

    xfree(t);
    if (err != 0) {
        fail("tuxpac: " DB ": %s\n", errstr(err));
    }
    if (err != 0 || !db_load()) {
        return false;
    }
    status_write();
    return true;
}

static int find_inst(const char *name, uint32_t n) {
    for (uint32_t i = 0; i < ndb; i++) {
        if (!db[i].gone && same(db[i].name, (uint32_t)strlen(db[i].name), name, n)) {
            return (int)i;
        }
    }
    return -1;
}

/* Whether installed package i is what alternative a names, by name or by
   what it provides. */
static bool answers(uint32_t i, const char *a, uint32_t n) {
    return !db[i].gone && (same(db[i].name, (uint32_t)strlen(db[i].name), a, n) ||
                           listed(db[i].prov, a, n));
}

/* ---- installing ------------------------------------------------------------ */

/* Packages wanted - a dependency's alternatives, "a|b" - and the line of
   the index that answers each best: rank 2k for alternative k by name,
   2k+1 by what a package provides. Resolved a round at a time, each round
   one pass through the index for every want of the round before. */
enum { DEP, TARGET, UPGRADE };
#define NONE 255

struct want {
    uint16_t at, len;
    uint8_t  rank, kind;
    uint32_t off;
    char     ver[VER];              /* of the line at off */
};

static struct work {
    struct want wants[WANTS];
    char        wtext[WTEXT];
    struct {
        uint32_t off;
        uint16_t name;
        char     flag;
    }           plan[PLAN];
    char        names[NAMES];
} *w;
static uint32_t nwants, wused, nplan, nused, from, to;

static bool planned(const char *name, uint32_t n) {
    for (uint32_t i = 0; i < nplan; i++) {
        const char *p = w->names + w->plan[i].name;

        if (same(p, (uint32_t)strlen(p), name, n)) {
            return true;
        }
    }
    return false;
}

static bool want(const char *g, uint32_t n, uint8_t kind) {
    for (uint32_t i = to; i < nwants; i++) {
        if (same(w->wtext + w->wants[i].at, w->wants[i].len, g, n)) {
            return true;
        }
    }
    if (nwants == WANTS || wused + n > WTEXT) {
        fail("tuxpac: too many packages\n");
        return false;
    }
    memcpy(w->wtext + wused, g, n);
    w->wants[nwants++] = (struct want){ (uint16_t)wused, (uint16_t)n, NONE, kind, 0, "" };
    wused += n;
    return true;
}

/* Whether an alternative of group g is installed, or about to be. */
static bool satisfied(const char *g, uint32_t n) {
    for (const char *end = g + n; g < end;) {
        uint32_t k = 0;

        while (g + k < end && g[k] != '|') {
            k++;
        }
        if (planned(g, k)) {
            return true;
        }
        for (uint32_t i = 0; i < ndb; i++) {
            if (answers(i, g, k)) {
                return true;
            }
        }
        g += k + 1;
    }
    return false;
}

/* Line l answers x at rank r: taken if that is better, or as good and a
   newer version - the same package in two suites. */
static bool better(struct want *x, unsigned r, uint32_t at, const char *l) {
    char ver[VER];

    version_of(l, ver);
    if (r < x->rank || vcmp(ver, x->ver) > 0) {
        x->rank = (uint8_t)r;
        x->off = at;
        strcpy(x->ver, ver);
    }
    return true;
}

static bool match(char *l, uint32_t at) {
    uint32_t nn = span(l, "\t");
    const char *prov = NULL;

    for (uint32_t i = from; i < to; i++) {
        struct want *x = &w->wants[i];
        const char *g = w->wtext + x->at, *end = g + x->len;

        for (unsigned r = 0; g < end && r <= x->rank; r += 2) {
            uint32_t k = 0;

            while (g + k < end && g[k] != '|') {
                k++;
            }
            if (same(g, k, l, nn) && better(x, r, at, l)) {
                break;
            }
            if (x->kind != UPGRADE && r + 1 <= x->rank) {
                if (prov == NULL) {
                    prov = l;
                    for (int f = 0; f < F_PROV; f++) {
                        prov += span(prov, "\t");
                        prov += *prov != '\0';
                    }
                }
                if (listed(prov, g, k) && better(x, r + 1, at, l)) {
                    break;
                }
            }
            g += k + 1;
        }
    }
    return true;
}

/* What a package needs that Debian does not say: curl only recommends the
   certificates, and https - which tuxpac itself fetches through curl -
   fails without them. */
static const char *const implied[][2] = {
    { "curl", "ca-certificates" },
};

/* Turns the wants into the plan, the packages to install, pulling in what
   each depends on and is not there yet. */
static bool resolve(void) {
    for (from = 0; from < nwants; from = 0) {
        to = nwants;
        if (!scan(match)) {
            return false;
        }
        for (uint32_t i = from; i < to; i++) {
            struct want *x = &w->wants[i];
            char *f[FIELDS];

            if (x->rank == NONE) {
                memcpy(tmp, w->wtext + x->at, x->len);
                tmp[x->len] = '\0';
                if (x->kind == TARGET) {
                    fail("tuxpac: %s: not found\n", tmp);
                    return false;
                }
                if (x->kind == DEP) {
                    fail("tuxpac: %s: not found, skipped\n", tmp);
                }
                continue;
            }
            split(read_line(x->off, tmp), f, FIELDS);
            uint32_t nn = (uint32_t)strlen(f[F_NAME]);
            int k = find_inst(f[F_NAME], nn);

            if (planned(f[F_NAME], nn) || (x->kind == DEP && k >= 0) ||
                (x->kind == UPGRADE && k >= 0 && vcmp(f[F_VER], db[k].ver) <= 0)) {
                continue;
            }
            if (nplan == PLAN || nused + nn + 1 > NAMES) {
                fail("tuxpac: too many packages\n");
                return false;
            }
            w->plan[nplan].off = x->off;
            w->plan[nplan].name = (uint16_t)nused;
            w->plan[nplan++].flag = x->kind == TARGET ? 'm' : k >= 0 ? db[k].flag : 'a';
            strcpy(w->names + nused, f[F_NAME]);
            nused += nn + 1;
            for (const char *d = f[F_DEPS]; *d != '\0';) {
                uint32_t g = span(d, ",");

                if (g > 0 && !satisfied(d, g) && !want(d, g, DEP)) {
                    return false;
                }
                d += g + (d[g] == ',');
            }
            for (unsigned k = 0; k < sizeof implied / sizeof implied[0]; k++) {
                const char *i = implied[k][1];

                if (strcmp(f[F_NAME], implied[k][0]) == 0 && !satisfied(i, (uint32_t)strlen(i)) &&
                    !want(i, (uint32_t)strlen(i), DEP)) {
                    return false;
                }
            }
        }
        /* The round is done with: the next one's wants move down. */
        uint32_t base = to < nwants ? w->wants[to].at : wused;

        memmove(w->wants, w->wants + to, (nwants - to) * sizeof w->wants[0]);
        memmove(w->wtext, w->wtext + base, wused - base);
        nwants -= to;
        wused -= base;
        for (uint32_t i = 0; i < nwants; i++) {
            w->wants[i].at = (uint16_t)(w->wants[i].at - base);
        }
        to = 0;
    }
    return true;
}

/* ---- unpacking ------------------------------------------------------------- */

enum { CONTROL = 1, DATA };
enum { K_SKIP, K_FILE, K_NAME, K_LINK, K_CONF, K_POST, K_SCRIPT };

static struct {
    uint8_t  hdr[512];
    uint32_t hlen, pad, failed, longlen;
    int      fd;                    /* the file being written */
    uint64_t left;
    uint8_t  kind, mode;
    bool     first, has_name, has_link;     /* first: the file's data starts next */
    char     longname[256], longlink[256];
} tar;

static int      deb = -1;
static uint32_t deb_size;
static char     path[PATH + 264];
static char     conf[CONF], post[POST];
static uint32_t conf_len, post_len;
static char    *list;
static uint32_t list_len, list_cap;
static const char *unpacking;       /* the package whose scripts are being kept */

/* The maintainer scripts dpkg keeps, and the triggers file. */
static const char *const scripts[] = { "preinst", "postinst", "prerm", "postrm", "triggers" };

/* Where package pkg's script is kept. */
static void script_path(char *out, const char *pkg, const char *script) {
    format(out, LIB "/%s.%s", pkg, script);
}

/* Unpacked by nobody's choice on a small machine. */
static const char *const skipped[] = {
    "/usr/share/doc", "/usr/share/man", "/usr/share/info", "/usr/share/locale", "/usr/share/lintian",
};

static bool excluded(const char *p) {
    for (unsigned i = 0; i < sizeof skipped / sizeof skipped[0]; i++) {
        size_t n = strlen(skipped[i]);

        if (memcmp_n(p, skipped[i], n) && (p[n] == '\0' || p[n] == '/')) {
            return true;
        }
    }
    return false;
}

static bool is_conf(const char *p) {
    uint32_t n = (uint32_t)strlen(p);

    for (const char *c = conf; *c != '\0';) {
        if (same(c, span(c, " \n"), p, n)) {
            return true;
        }
        c += span(c, "\n");
        c += *c == '\n';
    }
    return false;
}

static void list_add(const char *mark, const char *p, const char *tail) {
    uint32_t n = (uint32_t)(strlen(mark) + strlen(p) + strlen(tail) + 1);

    if (list_len + n > list_cap) {
        uint32_t cap = list_cap != 0 ? list_cap * 2 : 16384;
        char *bigger;

        while (cap < list_len + n) {
            cap *= 2;
        }
        if ((bigger = xalloc(cap)) == NULL) {
            tar.failed++;
            return;
        }
        memcpy(bigger, list, list_len);
        xfree(list);
        list = bigger;
        list_cap = cap;
    }
    char *at = cat(cat(cat(list + list_len, mark), p), tail);

    *at++ = '\n';
    list_len = (uint32_t)(at - list);
}

static uint64_t octal(const uint8_t *p, unsigned n) {
    uint64_t v = 0;

    for (unsigned i = 0; i < n; i++) {
        if (p[i] == ' ' && v == 0) {
            continue;
        }
        if (p[i] < '0' || p[i] > '7') {
            break;
        }
        v = v * 8 + (p[i] - '0');
    }
    return v;
}

/* "./usr/bin/x" as "/usr/bin/x", into out, no slash at the end. */
static void absolute(const char *name, char *out) {
    if (name[0] == '.' && (name[1] == '/' || name[1] == '\0')) {
        name++;
    }
    if (*name != '/') {
        *out++ = '/';
    }
    strcpy(out, name);
    size_t n = strlen(out);

    while (n > 1 && out[n - 1] == '/') {
        out[--n] = '\0';
    }
}

static void tar_entry(void) {
    static char name[256 + 160], link[256];
    const uint8_t *h = tar.hdr;
    char type = (char)h[156];
    uint64_t size = octal(h + 124, 12);
    bool zero = true;
    struct stat st;
    long err;

    tar.kind = K_SKIP;
    tar.left = tar.pad = 0;
    for (unsigned i = 0; i < 512 && zero; i++) {
        zero = h[i] == 0;
    }
    if (zero) {
        return;                     /* the end */
    }
    tar.left = size;
    tar.pad = (uint32_t)((512 - size % 512) % 512);
    if (type == 'L' || type == 'K') {
        tar.kind = type == 'L' ? K_NAME : K_LINK;
        tar.longlen = 0;
        return;
    }
    if (type == 'x' || type == 'g') {
        return;
    }
    if (tar.has_name) {
        strcpy(name, tar.longname);
    } else {
        size_t n = 0;

        if (memcmp_n((const char *)h + 257, "ustar", 5) && h[345] != 0) {
            for (unsigned i = 0; i < 155 && h[345 + i] != 0; i++) {
                name[n++] = (char)h[345 + i];
            }
            name[n++] = '/';
        }
        for (unsigned i = 0; i < 100 && h[i] != 0; i++) {
            name[n++] = (char)h[i];
        }
        name[n] = '\0';
    }
    if (tar.has_link) {
        strcpy(link, tar.longlink);
    } else {
        unsigned i = 0;

        for (; i < 100 && h[157 + i] != 0; i++) {
            link[i] = (char)h[157 + i];
        }
        link[i] = '\0';
    }
    tar.has_name = tar.has_link = false;
    absolute(name, path);

    if (tar.mode == CONTROL) {
        if (strcmp(path, "/conffiles") == 0) {
            tar.kind = K_CONF;
            return;
        }
        for (unsigned i = 0; i < sizeof scripts / sizeof scripts[0]; i++) {
            char sp[PATH];

            if (strcmp(path + 1, scripts[i]) != 0 || strlen(unpacking) + 32 > sizeof sp) {
                continue;
            }
            script_path(sp, unpacking, scripts[i]);
            mkdirs(LIB);
            tar.fd = (int)sys_open(sp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
            tar.kind = strcmp(scripts[i], "postinst") == 0 ? K_POST : K_SCRIPT;
            if (tar.fd < 0) {
                tar.kind = K_SKIP;
            } else if (size == 0) {
                sys_close(tar.fd);
                tar.fd = -1;
            }
        }
        return;
    }
    if (strcmp(path, "/") == 0 || excluded(path)) {
        return;
    }
    if (strlen(path) >= PATH) {
        tar.failed++;
        return;
    }
    switch (type) {
    case '5':
        if ((err = sys_mkdir(path)) == -ENOENT) {
            mkparent(path);
            err = sys_mkdir(path);
        }
        if (err == 0) {
            list_add("", path, "/");
        }
        break;
    case '1':                       /* a hard link: a symbolic one will do */
    case '2':
        if (type == '1') {
            absolute(link, name);
            strcpy(link, name);
        }
        if (sys_lstat(path, &st) == 0 && (st.mode & S_IFMT) != S_IFDIR) {
            sys_unlink(path);
        }
        if ((err = sys_symlink(link, path)) == -ENOENT) {
            mkparent(path);
            err = sys_symlink(link, path);
        }
        if (err == 0) {
            list_add("", path, "");
        } else if (err != -EEXIST) {
            tar.failed++;
        }
        break;
    case '0':
    case '\0':
    case '7': {
        bool c = is_conf(path);

        int mode = (int)octal(h + 100, 8) & 07777;

        tar.kind = K_FILE;
        tar.fd = -1;
        if (!(c && exists(path))) {                     /* a conffile there stays */
            if (sys_lstat(path, &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) {
                sys_unlink(path);
            }
            if ((err = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode)) == -ENOENT) {
                mkparent(path);
                err = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
            }
            if (err < 0) {
                tar.failed++;
                break;
            }
            tar.fd = (int)err;
            tar.first = true;
            if (size == 0) {
                sys_close(tar.fd);
                tar.fd = -1;
            }
        }
        list_add(c ? "*" : "", path, "");
        break;
    }
    }
}

/* ---- libraries nothing said were needed ------------------------------------
 *
 * Debian leaves out of a package's Depends what every Debian system has -
 * libcom-err2, ncurses-base - so what a program links to is read from it:
 * each library it needs that is not on the disk is installed too, under the
 * name Debian gives a library's package, libcom_err.so.2 as libcom-err2. */

static char     elves[8192], wanted_libs[4096];     /* ELF files unpacked; libraries they need */
static uint32_t elves_len, wanted_len;
static char     unpacked[16384];        /* packages this command unpacked: name, then
                                           the version replaced or "-", a line each */
static uint32_t unpacked_len;

static void add_line(char *to, uint32_t *len, uint32_t cap, const char *what) {
    uint32_t n = (uint32_t)strlen(what);

    for (const char *c = to; c < to + *len; c += span(c, "\n") + 1) {
        if (same(c, span(c, "\n"), what, n)) {
            return;                 /* there already */
        }
    }
    if (*len + n + 2 <= cap) {
        memcpy(to + *len, what, n);
        *len += n;
        to[(*len)++] = '\n';
        to[*len] = '\0';
    }
}

static void elf_add(const char *file) {
    add_line(elves, &elves_len, sizeof elves, file);
}

struct elf_header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_program {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

/* The libraries the file at path asks the loader for: its DT_NEEDEDs. */
static void elf_needed(const char *file) {
    static struct elf_program ph[16];
    static uint64_t dyn[256];
    static char name[64];
    struct elf_header eh;
    uint64_t strtab = 0, strtab_at = 0;
    uint32_t size, ndyn = 0;
    int fd = open_read(file, &size);

    if (fd < 0) {
        return;
    }
    if (sys_pread(fd, &eh, sizeof eh, 0) == sizeof eh && eh.ident[4] == 2 &&
        eh.phentsize == sizeof ph[0] && eh.phnum <= 16 &&
        sys_pread(fd, ph, eh.phnum * sizeof ph[0], eh.phoff) == (long)(eh.phnum * sizeof ph[0])) {
        for (unsigned i = 0; i < eh.phnum; i++) {
            if (ph[i].type == 2) {          /* PT_DYNAMIC */
                uint64_t n = ph[i].filesz < sizeof dyn ? ph[i].filesz : sizeof dyn;
                long got = sys_pread(fd, dyn, n, ph[i].offset);

                ndyn = got > 0 ? (uint32_t)got / 16 : 0;
            }
        }
        for (uint32_t i = 0; i < ndyn && dyn[2 * i] != 0; i++) {
            strtab = dyn[2 * i] == 5 ? dyn[2 * i + 1] : strtab;     /* DT_STRTAB */
        }
        for (unsigned i = 0; i < eh.phnum; i++) {
            if (ph[i].type == 1 && strtab >= ph[i].vaddr && strtab < ph[i].vaddr + ph[i].filesz) {
                strtab_at = strtab - ph[i].vaddr + ph[i].offset;
            }
        }
        for (uint32_t i = 0; strtab_at != 0 && i < ndyn && dyn[2 * i] != 0; i++) {
            if (dyn[2 * i] == 1 &&      /* DT_NEEDED */
                sys_pread(fd, name, sizeof name - 1, strtab_at + dyn[2 * i + 1]) > 0) {
                name[sizeof name - 1] = '\0';
                add_line(wanted_libs, &wanted_len, sizeof wanted_libs, name);
            }
        }
    }
    sys_close(fd);
}

static bool lib_present(const char *so) {
    static const char *const dirs[] = {
        "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib", "/lib",
    };
    char at[PATH];
    struct stat st;

    for (unsigned i = 0; i < 4; i++) {
        if (strlen(dirs[i]) + strlen(so) + 2 <= sizeof at) {
            format(at, "%s/%s", dirs[i], so);
            if (sys_stat(at, &st) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* libfoo_bar.so.2 is in libfoo-bar2, libpcre2-8.so.0 in libpcre2-8-0. */
static bool lib_package(const char *so, char *out, size_t max) {
    const char *dot = so;
    size_t n = 0;

    while (*dot != '\0' && !memcmp_n(dot, ".so.", 4)) {
        dot++;
    }
    if (*dot == '\0' || (size_t)(dot - so) + strlen(dot + 4) + 2 > max) {
        return false;
    }
    for (const char *c = so; c < dot; c++) {
        out[n++] = *c == '_' ? '-' : *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
    }
    if (n > 0 && out[n - 1] >= '0' && out[n - 1] <= '9') {
        out[n++] = '-';
    }
    strcpy(out + n, dot + 4);
    return true;
}

static bool tar_sink(const uint8_t *d, uint32_t n) {
    while (n > 0) {
        uint32_t k;

        if (tar.left > 0) {
            k = tar.left < n ? (uint32_t)tar.left : n;
            if (tar.kind == K_FILE && tar.fd >= 0) {
                if (tar.first && k >= 4 && memcmp_n((const char *)d, "\177ELF", 4)) {
                    elf_add(path);  /* a program or a library: what it links to is checked */
                }
                tar.first = false;
                if (sys_write(tar.fd, d, k) != (long)k) {
                    tar.failed++;
                    sys_close(tar.fd);
                    tar.fd = -1;
                }
                if (tar.left == k && tar.fd >= 0) {
                    sys_close(tar.fd);  /* all of it */
                    tar.fd = -1;
                }
            } else if (tar.kind == K_NAME || tar.kind == K_LINK) {
                char *to = tar.kind == K_NAME ? tar.longname : tar.longlink;

                for (uint32_t i = 0; i < k && tar.longlen < 255; i++) {
                    to[tar.longlen++] = (char)d[i];
                }
                to[tar.longlen] = '\0';
                *(tar.kind == K_NAME ? &tar.has_name : &tar.has_link) = true;
            } else if (tar.kind == K_CONF) {
                for (uint32_t i = 0; i < k && conf_len < CONF - 1; i++) {
                    conf[conf_len++] = (char)d[i];
                }
                conf[conf_len] = '\0';
            } else if (tar.kind == K_POST || tar.kind == K_SCRIPT) {
                for (uint32_t i = 0; tar.kind == K_POST && i < k && post_len < POST - 1; i++) {
                    post[post_len++] = (char)d[i];
                }
                post[post_len] = '\0';
                if (tar.fd >= 0 && sys_write(tar.fd, d, k) != (long)k) {
                    tar.failed++;
                }
                if (tar.left == k && tar.fd >= 0) {
                    sys_close(tar.fd);
                    tar.fd = -1;
                }
            }
            tar.left -= k;
        } else if (tar.pad > 0) {
            k = tar.pad < n ? tar.pad : n;
            tar.pad -= k;
        } else {
            k = 512 - tar.hlen < n ? 512 - tar.hlen : n;
            memcpy(tar.hdr + tar.hlen, d, k);
            if ((tar.hlen += k) == 512) {
                tar.hlen = 0;
                tar_entry();
            }
        }
        d += k;
        n -= k;
    }
    return true;
}

static uint64_t svli(void) {
    uint64_t v = 0;

    for (unsigned i = 0; i < 9; i++) {
        uint8_t b = src_byte(&fsrc);

        v |= (uint64_t)(b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) {
            break;
        }
    }
    return v;
}

/* What the xz data at from..to unpacks to, from its index at the end, or 0. */
static uint64_t xz_size(uint32_t from, uint32_t to) {
    uint8_t foot[12];
    uint64_t total = 0;

    if (to - from < 32) {
        return 0;
    }
    src_file(&fsrc, deb, to - 12, to);
    if (!src_read(&fsrc, foot, 12) || foot[10] != 'Y' || foot[11] != 'Z') {
        return 0;
    }
    uint32_t back = ((uint32_t)foot[4] | (uint32_t)foot[5] << 8 | (uint32_t)foot[6] << 16 |
                     (uint32_t)foot[7] << 24) + 1;

    if (back > (to - from - 24) / 4) {
        return 0;
    }
    src_file(&fsrc, deb, to - 12 - back * 4, to - 12);
    if (src_byte(&fsrc) != 0) {
        return 0;
    }
    for (uint64_t n = svli(); n > 0 && !fsrc.bad; n--) {
        svli();
        total += svli();
    }
    return fsrc.bad ? 0 : total;
}

static bool untar(const char *pkg, const char *member, uint32_t from, uint32_t to, uint8_t mode) {
    const char *ext = member + span(member, ".") + 4;      /* past ".tar" */
    bool ok;

    tar = (__typeof__(tar)){ .mode = mode };
    if (strcmp(ext, ".xz") == 0) {
        uint64_t size = xz_size(from, to);

        src_file(&fsrc, deb, from, to);
        ok = unxz(&fsrc, size, tar_sink);
    } else if (strcmp(ext, ".gz") == 0) {
        src_file(&fsrc, deb, from, to);
        ok = gunzip(&fsrc, tar_sink);
    } else if (*ext == '\0') {
        src_file(&fsrc, deb, from, to);
        ok = true;
        while (ok && (fsrc.pos < fsrc.len || fsrc.fill(&fsrc) > 0)) {
            ok = tar_sink(fsrc.buf + fsrc.pos, fsrc.len - fsrc.pos);
            fsrc.pos = fsrc.len;
        }
    } else {
        fail("tuxpac: %s: %s is not supported\n", pkg, member);
        return false;
    }
    if (!ok) {
        fail("tuxpac: %s: %s is damaged\n", pkg, member);
    }
    return ok;
}

/* What update-ca-certificates makes, which ca-certificates leaves to its
   install script: every certificate, one after another, for curl. */
static void ca_bundle(void) {
    static uint8_t dents[1024];
    char cert[PATH];
    struct out o;
    long dir = sys_open(CERTS, O_RDONLY | O_DIRECTORY, 0), got;

    if (dir < 0) {
        return;
    }
    mkparent(BUNDLE);
    out_open(&o, BUNDLE);
    while (o.err == 0 && (got = sys_getdents((int)dir, dents, sizeof dents)) > 0) {
        for (long at = 0; at < got; at += *(uint16_t *)(dents + at + 16)) {
            const char *name = (const char *)dents + at + 19;
            size_t n = strlen(name);
            uint32_t size;
            int fd;

            if (n < 5 || strcmp(name + n - 4, ".crt") != 0 || n + sizeof CERTS + 1 > sizeof cert) {
                continue;
            }
            format(cert, CERTS "/%s", name);
            if ((fd = open_read(cert, &size)) < 0) {
                continue;
            }
            if (size > 0 && size < NBUF && sys_pread(fd, nbuf, size, 0) == (long)size) {
                if (nbuf[size - 1] != '\n') {
                    nbuf[size++] = '\n';
                }
                out_put(&o, nbuf, size);
            }
            sys_close(fd);
        }
    }
    sys_close((int)dir);
    if (out_close(&o) == 0) {
        list_add("*", BUNDLE, "");
    }
}

static bool unpack_members(const char *pkg, char *h, char *member);
static char upgrading_from[VER + 1];    /* the version an upgrade replaces, "" for none */

/* ---- alternatives ---------------------------------------------------------- *
 *
 * /usr/bin/vim, editor, awk, pager: links a package's postinst makes with
 * update-alternatives, since it is not run. Each --install and --slave in
 * it becomes its link, if nothing is there yet and what it points at is. */

static char *find_text(char *s, const char *what) {
    for (size_t n = strlen(what); *s != '\0'; s++) {
        if (memcmp_n(s, what, n)) {
            return s;
        }
    }
    return NULL;
}

static void alt_link(const char *link, const char *target) {
    if (*link != '/' || strchr(link, '$') != NULL || strchr(target, '$') != NULL ||
        excluded(link) || exists(link) || !exists(target)) {
        return;
    }
    mkparent(link);
    if (sys_symlink(target, link) == 0) {
        list_add("", link, "");
    }
}

/* One update-alternatives command, its arguments up to its end. */
static void alt_command(char *c) {
    char *w[64];
    unsigned n = 0;

    while (*c != '\0' && n < 64) {
        while (*c == ' ' || *c == '\t' || *c == '"' || *c == '\'') {
            *c++ = '\0';
        }
        if (*c != '\0') {
            w[n++] = c;
        }
        while (*c != '\0' && *c != ' ' && *c != '\t' && *c != '"' && *c != '\'') {
            c++;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(w[i], "--install") == 0 && i + 3 < n) {
            alt_link(w[i + 1], w[i + 3]);
            i += 4;
        } else if (strcmp(w[i], "--slave") == 0 && i + 3 < n) {
            alt_link(w[i + 1], w[i + 3]);
            i += 3;
        }
    }
}

static void alternatives(void) {
    for (char *c = post; *c != '\0'; c++) {
        if (c[0] == '\\' && c[1] == '\n') {
            c[0] = c[1] = ' ';      /* a line continued */
        }
    }
    for (char *c = post; (c = find_text(c, "update-alternatives")) != NULL;) {
        char *end = c + span(c, "\n;&|");
        bool last = *end == '\0';

        *end = '\0';
        alt_command(c + sizeof "update-alternatives" - 1);
        c = last ? end : end + 1;
    }
}

/* ---- maintainer scripts ----------------------------------------------------
 *
 * As dpkg runs them: the script, its arguments, and the variables dpkg sets.
 * Nobody is there to answer a question, so debconf takes its defaults. A
 * script that fails is said to have, and the rest carries on. */

static bool run_script(const char *pkg, const char *script, const char *a1, const char *a2,
                       const char *a3) {
    static char sp[PATH], ev_pkg[160], ev_name[48];
    const char *argv[] = { sp, a1, a2, a3, NULL };
    const char *env[] = { ev_pkg, ev_name, "DPKG_MAINTSCRIPT_ARCH=amd64", "DPKG_ROOT=",
                          "DPKG_ADMINDIR=/var/lib/dpkg", "DPKG_RUNNING_VERSION=1.22.21",
                          "DEBIAN_FRONTEND=noninteractive", NULL };
    int code;

    if (strlen(pkg) + 32 > sizeof sp) {
        return false;
    }
    script_path(sp, pkg, script);
    if (!exists(sp)) {
        return true;                /* none: nothing to do */
    }
    /* The program it is a script for - /bin/sh, mostly - may be in this
       very command, not unpacked yet: then, as when debootstrap lays the
       first packages down, there is nothing to run it with. */
    uint32_t size;
    char *head = load(sp, &size);

    if (head != NULL && head[0] == '#' && head[1] == '!') {
        char *interp = head + 2;

        while (*interp == ' ' || *interp == '\t') {
            interp++;
        }

        interp[span(interp, " \t\n")] = '\0';
        if (!exists(interp)) {
            xfree(head);
            return true;
        }
    }
    xfree(head);
    format(ev_pkg, "DPKG_MAINTSCRIPT_PACKAGE=%s", pkg);
    format(ev_name, "DPKG_MAINTSCRIPT_NAME=%s", script);
    if ((code = run_env(sp, argv, env)) != 0) {
        fail("tuxpac: %s: %s failed (%u)\n", pkg, script, (unsigned)(code < 0 ? -code : code));
        return false;
    }
    return true;
}

static void scripts_drop(const char *pkg) {
    char sp[PATH];

    for (unsigned i = 0; i < sizeof scripts / sizeof scripts[0] && strlen(pkg) + 32 < sizeof sp; i++) {
        script_path(sp, pkg, scripts[i]);
        sys_unlink(sp);
    }
}

/* /var/lib/dpkg/status: a paragraph per package installed, as dpkg-query
   reads it - what tells a script asking dpkg that its package is there. */
static void status_write(void) {
    struct out o;

    mkdirs("/var/lib/dpkg/info");
    mkdirs("/var/lib/dpkg/updates");
    mkdirs("/var/lib/dpkg/alternatives");
    mkdirs("/etc/alternatives");
    out_open(&o, STATUS);
    for (uint32_t i = 0; i < ndb && o.err == 0; i++) {
        if (db[i].gone) {
            continue;
        }
        format(tmp, "Package: %s\nStatus: install ok installed\nPriority: optional\n"
                    "Section: misc\nMaintainer: tuxpac\nArchitecture: amd64\nVersion: %s\n"
                    "Description: installed by tuxpac\n\n", db[i].name, db[i].ver);
        out_put(&o, tmp, (uint32_t)strlen(tmp));
    }
    out_close(&o);
}

/* The downloaded package into place, and its list of paths beside the
   index. */
static bool unpack(const char *pkg) {
    char h[60], member[17], lp[PATH];
    bool ok = unpack_members(pkg, h, member);

    sys_close(deb);
    deb = -1;
    if (!ok) {
        return false;
    }
    for (char *c = elves; c < elves + elves_len; c += span(c, "\n") + 1) {
        c[span(c, "\n")] = '\0';
        elf_needed(c);
    }
    elves_len = 0;
    if (strcmp(pkg, "ca-certificates") == 0) {
        ca_bundle();
    }
    if (strlen(pkg) + sizeof LIB "/.list" > sizeof lp) {
        return false;
    }
    format(lp, LIB "/%s.list", pkg);
    write_file(lp, list != NULL ? list : "", list_len);
    if (tar.failed > 0) {
        fail("tuxpac: %s: %u paths not written\n", pkg, tar.failed);
    }
    return true;
}

/* The control and data members of the package, unpacked. */
static bool unpack_members(const char *pkg, char *h, char *member) {
    uint32_t failed = 0;

    if ((deb = open_read(DEB, &deb_size)) < 0) {
        return false;
    }
    src_file(&fsrc, deb, 0, deb_size);
    if (!src_read(&fsrc, h, 8) || !memcmp_n(h, "!<arch>\n", 8)) {
        fail("tuxpac: %s: not a package\n", pkg);
        return false;
    }
    conf_len = post_len = list_len = 0;
    post[0] = '\0';
    conf[0] = '\0';
    unpacking = pkg;
    scripts_drop(pkg);              /* an upgrade's are the new version's */

    for (uint32_t at = 8; at + 60 <= deb_size;) {
        src_file(&fsrc, deb, at, at + 60);
        if (!src_read(&fsrc, h, 60)) {
            return false;
        }
        uint32_t size = number(h + 48), n = 0;

        while (n < 16 && h[n] != ' ' && h[n] != '/') {
            member[n] = h[n];
            n++;
        }
        member[n] = '\0';
        at += 60;
        if (size > deb_size - at) {
            fail("tuxpac: %s: not a package\n", pkg);
            return false;
        }
        uint8_t mode = memcmp_n(member, "control.tar", 11) ? CONTROL :
                       memcmp_n(member, "data.tar", 8) ? DATA : 0;

        if (mode == DATA) {
            /* Before its files go in, as dpkg runs it: a new install, or
               an upgrade from the version there. */
            run_script(pkg, "preinst", upgrading_from[0] != '\0' ? "upgrade" : "install",
                       upgrading_from[0] != '\0' ? upgrading_from : NULL, NULL);
        }
        if (mode != 0) {
            if (!untar(pkg, member, at, at + size, mode)) {
                return false;
            }
            failed += tar.failed;
        }
        at += size + (size & 1);
    }
    tar.failed = failed;
    return true;
}

static bool download(const char *url, uint32_t size) {
    struct out o;
    uint32_t held = 0;

    if (https(url)) {
        struct stat st = { .size = 0 };

        if (!curl(url, DEB)) {
            if (http_status != 0) {
                fail("tuxpac: %s: HTTP error\n", url);
            }
            return false;
        }
        if (sys_stat(DEB, &st) < 0 || st.size != size) {
            fail("tuxpac: %s: %u of %u bytes\n", url, (uint32_t)st.size, size);
            return false;
        }
        return true;
    }
    mkdirs(CACHE);
    out_open(&o, DEB);
    if (o.err != 0) {
        fail("tuxpac: " DEB ": %s\n", errstr(o.err));
        return false;
    }
    if (!http_open(url, &nsrc)) {
        if (http_status != 0) {
            fail("tuxpac: %s: HTTP %u\n", url, http_status);
        }
        return false;
    }
    while (o.err == 0 && (nsrc.pos < nsrc.len || nsrc.fill(&nsrc) > 0)) {
        uint32_t k = nsrc.len - nsrc.pos;

        if (held + k > FBUF) {
            out_put(&o, fbuf, held);
            held = 0;
        }
        memcpy(fbuf + held, nsrc.buf + nsrc.pos, k);
        held += k;
        nsrc.pos = nsrc.len;
    }
    out_put(&o, fbuf, held);
    out_close(&o);
    http_close(&nsrc);
    if (o.err != 0) {
        fail("tuxpac: " DEB ": %s\n", errstr(o.err));
    } else if (o.done != size) {
        fail("tuxpac: %s: %u of %u bytes\n", url, o.done, size);
    }
    return o.err == 0 && o.done == size;
}

/* /bin, /sbin, /lib and /lib64 as links into /usr, if nothing is there. */
static void merged_usr(void) {
    static const char *const dirs[] = { "bin", "sbin", "lib", "lib64" };
    char a[16], b[16];

    for (unsigned i = 0; i < 4; i++) {
        format(a, "/%s", dirs[i]);
        if (!exists(a)) {
            format(b, "/usr/%s", dirs[i]);
            mkdirs(b);
            sys_symlink(b + 1, a);
        }
    }
}

static bool install(uint32_t off, char flag) {
    static char url[512], entry[LINE];
    char *f[FIELDS];
    unsigned m;

    split(read_line(off, tmp), f, FIELDS);
    m = (unsigned)(f[F_MIRROR][0] - '0');
    if (m >= nmirrors) {
        fail("tuxpac: " MIRROR " changed, run tuxpac -y\n");
        return false;
    }
    if (strlen(mirrors[m].url) + strlen(f[F_FILE]) + 2 > sizeof url) {
        fail("tuxpac: %s: too long\n", f[F_FILE]);
        return false;
    }
    format(url, "%s/%s", mirrors[m].url, f[F_FILE]);

    int k = find_inst(f[F_NAME], (uint32_t)strlen(f[F_NAME]));

    upgrading_from[0] = '\0';
    if (k >= 0 && strlen(db[k].ver) <= VER) {
        strcpy(upgrading_from, db[k].ver);
    }
    if (k >= 0 && strcmp(db[k].ver, f[F_VER]) != 0) {
        print("upgrade %s %s -> %s\n", f[F_NAME], db[k].ver, f[F_VER]);
    } else {
        print("install %s %s\n", f[F_NAME], f[F_VER]);
    }
    if (!download(url, number(f[F_SIZE]))) {
        sys_unlink(DEB);
        return false;
    }
    bool ok = unpack(f[F_NAME]);

    sys_unlink(DEB);
    if (!ok) {
        return false;
    }
    if (k >= 0) {
        db[k].gone = true;
    }
    char *p = cat(entry, f[F_NAME]);

    *p++ = '\t';
    p = cat(p, f[F_VER]);
    *p++ = '\t';
    *p++ = flag;
    *p++ = '\t';
    p = cat(p, f[F_DEPS]);
    *p++ = '\t';
    p = cat(p, f[F_PROV]);
    *p++ = '\n';
    *p = '\0';
    /* Its name, and the version it replaced: lines that repeat, so not
       add_line, which keeps one of each. */
    if (unpacked_len + strlen(f[F_NAME]) + VER + 4 < sizeof unpacked) {
        char *u = cat(unpacked + unpacked_len, f[F_NAME]);

        *u++ = '\n';
        u = cat(u, upgrading_from[0] != '\0' ? upgrading_from : "-");
        *u++ = '\n';
        *u = '\0';
        unpacked_len = (uint32_t)(u - unpacked);
    }
    return db_save(entry);
}

/* ---- configuring ---------------------------------------------------------- *
 *
 * Once everything a command installs is unpacked, as dpkg does it: each
 * package's postinst configure, what is depended on first - and then the
 * triggers: a package interested in a folder hears that something was put
 * in it (fontconfig, of fonts), and one interested in a name hears when a
 * package activates it (libc-bin, of ldconfig). */

/* Past the spaces and tabs at s. */
static char *blanks_past(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Whether path p - a list line, with its '*' or '/' - is name or under it. */
static bool under(const char *p, const char *name, uint32_t n) {
    p += *p == '*';
    return memcmp_n(p, name, n) && (p[n] == '\0' || p[n] == '/' || p[n] == '\n');
}

/* Whether any package unpacked this time put a file under name, or - for a
   name that is not a path - activates it. */
static bool touched(const char *name, uint32_t n) {
    char lp[PATH], pkg[160];

    for (char *c = unpacked; c < unpacked + unpacked_len;) {
        uint32_t len = span(c, "\n"), size;
        char *t;

        if (len < sizeof pkg) {
            memcpy(pkg, c, len);
            pkg[len] = '\0';
            format(lp, LIB "/%s.%s", pkg, *name == '/' ? "list" : "triggers");
            if ((t = load(lp, &size)) != NULL) {
                for (char *l = t; *l != '\0'; l += span(l, "\n"), l += *l == '\n') {
                    char *what = blanks_past(l + span(l, " \t"));
                    if (*name == '/' ? under(l, name, n)
                                     : memcmp_n(l, "activate", 8) &&
                                       same(what, span(what, " \t\n"), name, n)) {
                        xfree(t);
                        return true;
                    }
                }
                xfree(t);
            }
        }
        c += len + 1;
        c += span(c, "\n") + 1;    /* past its version line */
    }
    return false;
}

static void triggers(void) {
    char tp[PATH], names[1024];

    for (uint32_t i = 0; i < ndb; i++) {
        uint32_t size, len = 0;
        char *t;

        if (db[i].gone || strlen(db[i].name) + 32 > sizeof tp) {
            continue;
        }
        script_path(tp, db[i].name, "triggers");
        if ((t = load(tp, &size)) == NULL) {
            continue;
        }
        names[0] = '\0';
        for (char *l = t; *l != '\0'; l += span(l, "\n"), l += *l == '\n') {
            char *what = blanks_past(l + span(l, " \t"));
            uint32_t n = span(what, " \t\n");

            if (memcmp_n(l, "interest", 8) && n > 0 && len + n + 2 < sizeof names &&
                touched(what, n)) {
                if (len > 0) {
                    names[len++] = ' ';
                }
                memcpy(names + len, what, n);
                names[len += n] = '\0';
            }
        }
        xfree(t);
        if (len > 0) {
            run_script(db[i].name, "postinst", "triggered", names, NULL);
        }
    }
}

/* postinst configure for each package unpacked, in the order they went in;
   with no update-alternatives on the disk yet, what one would have made is
   made from the script's own lines. */
static void configure(void) {
    char sp[PATH];

    for (char *c = unpacked; c < unpacked + unpacked_len;) {
        char *name = c, *old;

        c += span(c, "\n");
        *c++ = '\0';
        old = c;
        c += span(c, "\n");
        *c++ = '\0';
        if (!exists(ALTS) && strlen(name) + 32 < sizeof sp) {
            uint32_t size;
            char *t;

            script_path(sp, name, "postinst");
            if ((t = load(sp, &size)) != NULL) {
                post_len = size < POST - 1 ? size : POST - 1;
                memcpy(post, t, post_len);
                post[post_len] = '\0';
                xfree(t);
                format(sp, LIB "/%s.list", name);
                if ((t = load(sp, &size)) != NULL) {
                    xfree(list);
                    list = t;
                    list_len = list_cap = size;
                    alternatives();
                    write_file(sp, list, list_len);
                }
            }
        }
        if (strlen(name) + 32 < sizeof sp) {
            script_path(sp, name, "postinst");
            if (exists(sp)) {
                print("configure %s\n", name);
            }
        }
        run_script(name, "postinst", "configure", strcmp(old, "-") != 0 ? old : NULL, NULL);
    }
    for (char *c = unpacked; c < unpacked + unpacked_len; c++) {
        if (*c == '\0') {
            *c = '\n';             /* back as lines, for the triggers */
        }
    }
    triggers();
    unpacked_len = 0;
}

/* A want for the package of each library needed and not on the disk. */
static bool want_missing(void) {
    char pkg[128];
    bool any = false;

    for (char *c = wanted_libs; c < wanted_libs + wanted_len; c += span(c, "\n") + 1) {
        c[span(c, "\n")] = '\0';
        if (!lib_present(c) && lib_package(c, pkg, sizeof pkg) &&
            find_inst(pkg, (uint32_t)strlen(pkg)) < 0) {
            any = want(pkg, (uint32_t)strlen(pkg), DEP) || any;
        }
    }
    wanted_len = 0;
    return any;
}

/* -s with names, -u with or without: resolve, then install the plan, what
   is depended on first. */
static void get(char *args, bool upgrade) {
    if (!db_load() || !load_mirrors() || (w = xalloc(sizeof *w)) == NULL) {
        return;
    }
    nwants = wused = nplan = nused = from = to = 0;
    if (upgrade && *args == '\0') {
        for (uint32_t i = 0; i < ndb; i++) {
            if (!want(db[i].name, (uint32_t)strlen(db[i].name), UPGRADE)) {
                return;
            }
        }
    }
    for (char *name = str_word(&args); *name != '\0'; name = str_word(&args)) {
        if (upgrade && find_inst(name, (uint32_t)strlen(name)) < 0) {
            fail("tuxpac: %s: not installed\n", name);
            return;
        }
        if (!want(name, (uint32_t)strlen(name), upgrade ? UPGRADE : TARGET)) {
            return;
        }
    }
    /* What was installed may need libraries no package said it did: a
       round more for those, and for what they need in turn. */
    for (unsigned round = 0; round < 4; round++) {
        if (!resolve()) {
            return;
        }
        if (nplan > 0) {
            merged_usr();
        }
        for (uint32_t i = nplan; i-- > 0;) {
            if (!install(w->plan[i].off, w->plan[i].flag)) {
                configure();
                return;
            }
        }
        configure();
        nwants = wused = nplan = nused = from = to = 0;
        if (!want_missing()) {
            return;
        }
    }
}

/* -e: every package the list marks Essential and is not installed, asked
   for as -s would ask - the ones Debian's packages count on without
   saying so. */
static char     essential[2048];
static uint32_t essential_len;

static bool essential_each(char *l, uint32_t at) {
    char *f[FIELDS];
    uint32_t n;

    (void)at;
    split(l, f, FIELDS);
    n = (uint32_t)strlen(f[F_NAME]);
    if (f[F_ESS][0] == 'e' && find_inst(f[F_NAME], n) < 0 &&
        !listed(essential, f[F_NAME], n) && essential_len + n + 2 < sizeof essential) {
        if (essential_len > 0) {
            essential[essential_len++] = ',';
        }
        memcpy(essential + essential_len, f[F_NAME], n + 1);
        essential_len += n;
    }
    return true;
}

static void get_essential(void) {
    if (!db_load() || !scan(essential_each)) {
        return;
    }
    if (essential_len == 0) {
        print("tuxpac: nothing essential to install\n");
        return;
    }
    for (char *c = essential; *c != '\0'; c++) {
        *c = *c == ',' ? ' ' : *c;
    }
    get(essential, false);
}

/* ---- removing -------------------------------------------------------------- */

/* /bin, /sbin, /lib and /lib64: the links into /usr that merged_usr makes.
   A package may list them too - base-files does - but they are tuxpac's,
   and every program's loader is reached through one. */
static bool top_link(const char *p) {
    static const char *const dirs[] = { "/bin", "/sbin", "/lib", "/lib64" };

    for (unsigned i = 0; i < 4; i++) {
        size_t n = strlen(dirs[i]);

        if (memcmp_n(p, dirs[i], n) && (p[n] == '\0' || (p[n] == '/' && p[n + 1] == '\0'))) {
            return true;
        }
    }
    return false;
}

static void remove_files(const char *pkg, bool purge) {
    char lp[PATH];
    uint32_t size;
    char *t;

    if (strlen(pkg) + sizeof LIB "/.list" > sizeof lp) {
        return;
    }
    format(lp, LIB "/%s.list", pkg);
    if ((t = load(lp, &size)) == NULL) {
        return;
    }
    /* Backwards: what is in a folder goes before the folder. */
    for (uint32_t end = size; end > 0;) {
        uint32_t e = end, s;

        e -= t[e - 1] == '\n';
        for (s = e; s > 0 && t[s - 1] != '\n'; s--) {
        }
        t[e] = '\0';
        char *p = t + s;

        if (*p == '*') {
            if (purge) {
                sys_unlink(p + 1);
            }
        } else if (*p != '\0' && !top_link(p)) {
            size_t n = strlen(p);

            if (n > 1 && p[n - 1] == '/') {
                p[n - 1] = '\0';
                sys_rmdir(p);       /* a folder goes only once it is empty */
            } else {
                sys_unlink(p);
            }
        }
        end = s;
    }
    xfree(t);
    sys_unlink(lp);
}

/* Whether installed package i depends on installed package k. */
static bool needs(uint32_t i, uint32_t k) {
    for (const char *a = db[i].deps; *a != '\0';) {
        uint32_t n = span(a, ",|");

        if (answers(k, a, n)) {
            return true;
        }
        a += n + (a[n] != '\0');
    }
    return false;
}

/* Marks what the packages asked for - all but skip - depend on, all the
   way down. */
static void mark(uint32_t skip) {
    bool more = true;

    for (uint32_t i = 0; i < ndb; i++) {
        db[i].mark = !db[i].gone && db[i].flag == 'm' && i != skip;
    }
    while (more) {
        more = false;
        for (uint32_t i = 0; i < ndb; i++) {
            if (!db[i].mark) {
                continue;
            }
            for (uint32_t j = 0; j < ndb; j++) {
                if (!db[j].mark && !db[j].gone && needs(i, j)) {
                    db[j].mark = more = true;
                }
            }
        }
    }
}

static void cmd_remove(char *args, bool purge) {
    char *name = str_word(&args);
    int k;

    if (!db_load()) {
        return;
    }
    if ((k = find_inst(name, (uint32_t)strlen(name))) < 0) {
        fail("tuxpac: %s: not installed\n", name);
        return;
    }
    mark((uint32_t)k);
    if (db[k].mark) {
        for (uint32_t i = 0; i < ndb; i++) {
            if (db[i].mark && (int)i != k && needs(i, (uint32_t)k)) {
                fail("tuxpac: %s: needed by %s\n", name, db[i].name);
                return;
            }
        }
    }
    for (uint32_t i = 0; i < ndb; i++) {
        if (!db[i].gone && !db[i].mark) {
            print("remove %s\n", db[i].name);
            run_script(db[i].name, "prerm", "remove", NULL, NULL);
            remove_files(db[i].name, purge);
            run_script(db[i].name, "postrm", "remove", NULL, NULL);
            if (purge) {
                run_script(db[i].name, "postrm", "purge", NULL, NULL);
            }
            scripts_drop(db[i].name);
            db[i].gone = true;
        }
    }
    db_save(NULL);
}

/* -re: the essential packages count as pulled in rather than asked for,
   so that the ones nothing else needs go - with what only they needed. */
static bool unessential_each(char *l, uint32_t at) {
    char *f[FIELDS];
    int k;

    (void)at;
    split(l, f, FIELDS);
    if (f[F_ESS][0] == 'e' && (k = find_inst(f[F_NAME], (uint32_t)strlen(f[F_NAME]))) >= 0) {
        db[k].flag = 'a';
    }
    return true;
}

static void remove_essential(void) {
    bool *kept;

    if (!db_load() || (kept = xalloc(ndb + 1)) == NULL) {
        return;
    }
    /* What is kept now goes only if the essential packages were all that
       kept it: a package nothing lists as a dependency stays as it is. */
    mark(ndb);
    for (uint32_t i = 0; i < ndb; i++) {
        kept[i] = db[i].mark || db[i].flag == 'm';
    }
    if (!scan(unessential_each)) {
        return;
    }
    mark(ndb);
    for (uint32_t i = 0; i < ndb; i++) {
        if (!db[i].gone && !db[i].mark && kept[i]) {
            print("remove %s\n", db[i].name);
            remove_files(db[i].name, false);
            db[i].gone = true;
        }
    }
    db_save(NULL);
}

/* ---- looking --------------------------------------------------------------- */

static const char *term;
static uint32_t    found_at;
static bool        found;

static bool find_each(char *l, uint32_t at) {
    char *f[FIELDS];

    (void)at;
    split(l, f, FIELDS);
    if (contains(f[F_NAME], term) || contains(f[F_DESC], term)) {
        print("%s %s%s\n    %s\n", f[F_NAME], f[F_VER],
                find_inst(f[F_NAME], (uint32_t)strlen(f[F_NAME])) >= 0 ? " [installed]" : "",
                f[F_DESC]);
    }
    return true;
}

static bool info_each(char *l, uint32_t at) {
    static char best[VER];
    char ver[VER];

    if (same(l, span(l, "\t"), term, (uint32_t)strlen(term))) {
        version_of(l, ver);
        if (!found || vcmp(ver, best) > 0) {
            strcpy(best, ver);
            found = true;
            found_at = at;
        }
    }
    return true;
}

static void row(const char *label, const char *value) {
    if (*value != '\0') {
        print("%s", label);
        for (size_t n = strlen(label); n < 12; n++) {
            print(" ");
        }
        print("%s\n", value);
    }
}

static void cmd_info(const char *name) {
    char *f[FIELDS], text[64];
    int k;

    if (!db_load()) {
        return;
    }
    k = find_inst(name, (uint32_t)strlen(name));
    term = name;
    found = false;
    if ((exists(INDEX) || k < 0) && !scan(info_each)) {
        return;
    }
    if (!found && k < 0) {
        fail("tuxpac: %s: not found\n", name);
        return;
    }
    if (found) {
        split(read_line(found_at, tmp), f, FIELDS);
    } else {
        f[F_NAME] = db[k].name;
        f[F_VER] = db[k].ver;
        f[F_DEPS] = db[k].deps;
        f[F_PROV] = db[k].prov;
        f[F_SIZE] = f[F_ISIZE] = f[F_FILE] = f[F_DESC] = "";
    }
    row("name", f[F_NAME]);
    row("version", f[F_VER]);
    if (k >= 0) {
        row("installed", db[k].ver);
        row("reason", db[k].flag == 'm' ? "asked for" : "dependency");
    }
    row("depends", f[F_DEPS]);
    row("provides", f[F_PROV]);
    if (*f[F_SIZE] != '\0') {
        format(text, "%u KiB", (number(f[F_SIZE]) + 1023) / 1024);
        row("download", text);
        format(text, "%u KiB", number(f[F_ISIZE]));
        row("size", text);
    }
    row("file", f[F_FILE]);
    row("description", f[F_DESC]);
}

/* ---- the command ----------------------------------------------------------- */

int main(int argc, char **argv) {
    static char words[1024];
    char *args = words, *op = argc > 1 ? argv[1] : "";
    char c = op[0] == '-' && op[1] != '\0' && op[2] == '\0' ? op[1] : 0;
    size_t n = 0;

    for (int i = 2; i < argc; i++) {        /* the rest, one line, as the
                                               kernel command took them */
        size_t k = strlen(argv[i]);

        if (n + k + 2 > sizeof words) {
            break;
        }
        if (n > 0) {
            words[n++] = ' ';
        }
        memcpy(words + n, argv[i], k + 1);
        n += k;
    }
    fbuf = xalloc(FBUF);
    nbuf = xalloc(NBUF);
    fsrc = (struct source){ .buf = fbuf, .cap = FBUF, .fd = -1 };
    nsrc = (struct source){ .buf = nbuf, .cap = NBUF, .fd = -1 };

    if (fbuf == NULL || nbuf == NULL) {
        /* said already */
    } else if (c == 'y' && *args == '\0') {
        sync();
    } else if (c == 's' && *args != '\0') {
        get(args, false);
    } else if (c == 'e' && *args == '\0') {
        get_essential();
    } else if (strcmp(op, "-re") == 0 && *args == '\0') {
        remove_essential();
    } else if (c == 'u') {
        get(args, true);
    } else if ((c == 'r' || c == 'n') && *args != '\0') {
        cmd_remove(args, c == 'n');
    } else if (c == 'f' && *args != '\0') {
        term = args;
        if (db_load()) {
            scan(find_each);
        }
    } else if (c == 'q') {
        if (db_load()) {
            for (uint32_t i = 0; i < ndb; i++) {
                if (contains(db[i].name, args)) {
                    print("%s %s\n", db[i].name, db[i].ver);
                }
            }
        }
    } else if (c == 'i' && *args != '\0') {
        cmd_info(str_word(&args));
    } else {
        fail("usage: tuxpac -s|-r|-n|-i <package>, -u [package], -e, -re, -y, -f <text>, -q [text]\n");
    }
    return status;
}
