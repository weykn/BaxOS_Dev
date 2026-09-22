#include "util.h"

/* Clears the screen, which is a thing one asks the console for rather than
   does: the escape below is what every terminal takes for it. */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    put("\033[2J");
    return 0;
}
