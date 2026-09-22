#include "util.h"

/* Reads a Markdown file the way it was meant to be read: headings in their
   own colour, bullets as bullets, `code` and **bold** picked out. What it
   does not understand it prints as it stands, which is the point of the
   format. */

static char buffer[4096];
static char line[256];

static void inline_text(const char *text, const char *base) {
    bool bold = false, code = false;

    put(base);
    while (*text != '\0') {
        if (text[0] == '*' && text[1] == '*') {
            bold = !bold;
            text += 2;
            put(bold ? BRIGHT : base);
        } else if (*text == '`') {
            code = !code;
            text++;
            put(code ? GREEN : base);
        } else {
            sys_write(STDOUT, text++, 1);
        }
    }
    put(PLAIN);
}

static void show(const char *text) {
    size_t indent = 0;
    unsigned level = 0;

    while (text[indent] == ' ') {
        indent++;
    }
    const char *body = text + indent;

    while (body[level] == '#') {
        level++;
    }
    if (level > 0 && body[level] == ' ') {
        /* Headings stand on their own, the deeper ones dimmer, so the shape
           of the document reads at a glance. */
        put(level == 1 ? BRIGHT : level == 2 ? ACCENT : GREEN);
        put(body + level + 1);
        put(PLAIN "\n");
        return;
    }
    for (size_t i = 0; i < indent; i++) {
        put(" ");
    }
    if ((*body == '-' || *body == '*') && body[1] == ' ') {
        put(ACCENT "  \x07 ");      /* a bullet, in the console's own glyphs */
        inline_text(body + 2, PLAIN);
    } else if (*body == '>' && body[1] == ' ') {
        inline_text(body + 2, DIM);
    } else {
        inline_text(body, PLAIN);
    }
    put("\n");
}

int main(int argc, char **argv) {
    size_t length = 0;
    long fd, got;

    if (argc != 2) {
        put_error("md", NULL, "usage: md <file>");
        return 1;
    }
    fd = sys_open(argv[1], O_RDONLY);
    if (fd < 0) {
        put_error("md", argv[1], "no such file");
        return 1;
    }
    while ((got = sys_read((int)fd, buffer, sizeof buffer)) > 0) {
        for (long i = 0; i < got; i++) {
            char c = buffer[i];

            if (c == '\r') {
                continue;
            }
            if (c == '\n' || length + 1 >= sizeof line) {
                line[length] = '\0';
                show(line);
                length = 0;
                if (c != '\n') {
                    line[length++] = c;
                }
            } else {
                line[length++] = c;
            }
        }
    }
    sys_close((int)fd);
    if (length > 0) {
        line[length] = '\0';
        show(line);
    }
    put("\n");                      /* a blank line before the prompt */
    return 0;
}
