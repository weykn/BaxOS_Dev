#include "util.h"

/* Starts the machine again. */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sys_reboot(REBOOT_RESTART);
    put_error("reboot", NULL, "the firmware would not");
    return 1;
}
