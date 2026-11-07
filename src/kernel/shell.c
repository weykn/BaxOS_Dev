#include "shell.h"

#include <stdbool.h>

#include "fs.h"
#include "io.h"
#include "string.h"
#include "syscall.h"
#include "vga.h"

/* Bringing the machine up: the shell, which brings up the rest.
 *
 * The shell is tsh, /usr/bin/tsh - a TEF program of under three
 * kilobytes, src/tsh. The kernel starts it as -tsh, the login shell, handing
 * it /etc/tuxlet/boot the first time: the machine itself - the modules, the
 * size of the screen, the environment - as a script. Then it runs
 * /etc/tuxlet/shell, which may start another shell such as bash, or is
 * the shell itself - and should it ever end, it is started again, there
 * being nothing else for the machine to do.
 *
 * What the kernel keeps of the environment is what its own commands use:
 * PATH, for Tab to complete a command, and MODPATH, where modman looks.
 * Every execve that carries them updates them - at boot, the boot
 * script's `export`s, from its first command on - and one that does not
 * leaves them as they were. */

#define TSH       "/usr/bin/tsh"
#define BOOT_CONF "/etc/tuxlet/boot"

/* "PATH=..." and "MODPATH=...", or empty. */
static char path_var[192], modpath_var[192];

static char *var_of(const char *name) {
    return strcmp(name, "PATH") == 0 ? path_var : strcmp(name, "MODPATH") == 0 ? modpath_var : NULL;
}

const char *shell_env(const char *name) {
    const char *var = var_of(name);

    return var != NULL && var[0] != '\0' ? var + strlen(name) + 1 : NULL;
}

void shell_env_take(unsigned envc, const char *const *envv) {
    for (unsigned i = 0; i < envc; i++) {
        const char *eq = strchr(envv[i], '=');
        char name[8];
        char *var;

        if (eq != NULL && (size_t)(eq - envv[i]) < sizeof name) {
            memcpy(name, envv[i], (size_t)(eq - envv[i]));
            name[eq - envv[i]] = '\0';
            if ((var = var_of(name)) != NULL && strlen(envv[i]) < sizeof path_var) {
                strcpy(var, envv[i]);
            }
        }
    }
}

__attribute__((noreturn)) void shell_run(void) {
    const char *argv[] = { "-tsh", BOOT_CONF, NULL };

    for (unsigned argc = 2;; argc = 1) {
        const char *env[] = { path_var, modpath_var };
        unsigned envc = path_var[0] != '\0' ? 1 : 0;

        if (modpath_var[0] != '\0') {
            env[envc++] = modpath_var;
        }

        int code = program_start(TSH, argc, argv, envc, env);

        if (code < 0) {
            kprintf("%s: %s\n", TSH, code == PROGRAM_EINVAL ? "not a program" : fs_error(code));
            halt_forever();
        }
    }
}
