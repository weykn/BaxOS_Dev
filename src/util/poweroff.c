#include "util.h"

/* Switches the machine off. */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sys_reboot(REBOOT_POWER_OFF);
    put_error("poweroff", NULL, "the firmware would not; it is safe to switch off");
    return 1;
}
