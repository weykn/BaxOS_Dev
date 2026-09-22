#include "util.h"

/* Prints its arguments, a space between each. */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            put(" ");
        }
        put(argv[i]);
    }
    put("\n");
    return 0;
}
