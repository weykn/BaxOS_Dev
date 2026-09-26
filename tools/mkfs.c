/* Host tool: builds the Tuxlet OS filesystem, or updates one in place.
 *
 *   mkfs IMAGE SECTORS ROOT KERNEL_BIN [PATH...]
 *
 * Formats the filesystem if the image has none yet, then stores the kernel
 * and every PATH in it. A PATH under ROOT keeps its position relative to
 * ROOT, folders and all - so src/disk/home/ed becomes home/ed, and the
 * folders it needs are made on the way. A PATH that is itself a folder is
 * made and left empty, which is the only way one with nothing in it yet can
 * reach the disk. A PATH that is a symbolic link becomes one on the disk,
 * pointing where it points - which is how /bin comes to be usr/bin. Files already there are left alone, so what was saved from
 * inside Tuxlet OS survives a rebuild.
 *
 * The kernel goes to boot/kernel.bin rather than into the root, which is
 * kept for the folders a Linux system has there.
 *
 * What comes out is the contents of one partition, which the Makefile writes
 * into the disk image beside the EFI system partition. The kernel that
 * actually boots is the copy inside the loader; the one here is the same
 * bytes, kept so that it can be seen and read like any other file.
 *
 * It links the kernel's own fs.c, so the two always agree on the format. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ata.h"
#include "fs.h"

#define SECTOR_SIZE 512

static int image;

/* fs.c talks to the disk through these; here the disk is the image file. */
int ata_read(uint32_t lba, void *buffer) {
    return pread(image, buffer, SECTOR_SIZE, (off_t)lba * SECTOR_SIZE) == SECTOR_SIZE ? 0 : -1;
}

int ata_read_many(uint32_t lba, unsigned count, void *buffer) {
    size_t bytes = (size_t)count * SECTOR_SIZE;

    return pread(image, buffer, bytes, (off_t)lba * SECTOR_SIZE) == (ssize_t)bytes ? 0 : -1;
}

int ata_write(uint32_t lba, const void *buffer) {
    return pwrite(image, buffer, SECTOR_SIZE, (off_t)lba * SECTOR_SIZE) == SECTOR_SIZE ? 0 : -1;
}

int ata_write_many(uint32_t lba, unsigned count, const void *buffer) {
    size_t bytes = (size_t)count * SECTOR_SIZE;

    return pwrite(image, buffer, bytes, (off_t)lba * SECTOR_SIZE) == (ssize_t)bytes ? 0 : -1;
}

void ata_sync(void) {
    /* The image is an ordinary file; the host writes it back when it pleases. */
}

static void *load(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    struct stat st;

    if (fd < 0 || fstat(fd, &st) < 0) {
        perror(path);
        exit(1);
    }
    void *data = malloc(st.st_size + 1);
    if (data == NULL || read(fd, data, st.st_size) != st.st_size) {
        perror(path);
        exit(1);
    }
    close(fd);
    *size = (size_t)st.st_size;
    return data;
}

static const char *base_name(const char *path) {
    const char *name = path;

    for (; *path != '\0'; path++) {
        if (*path == '/') {
            name = path + 1;
        }
    }
    return name;
}

/* The name to store path under: what follows root, if it is under root, and
   otherwise just the last component. */
static const char *stored_name(const char *root, const char *path) {
    size_t len = strlen(root);

    while (len > 0 && root[len - 1] == '/') {
        len--;                          /* a trailing slash is not part of it */
    }
    if (strncmp(path, root, len) == 0 && path[len] == '/') {
        return path + len + 1;
    }
    return base_name(path);
}

/* Where the kernel is kept. */
#define KERNEL_NAME "boot/kernel.bin"

static int is_folder(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Creates every folder the name needs, outermost first. */
static void make_folder(const char *name) {
    int err = fs_mkdir(name);

    if (err < 0 && err != FS_EEXIST) {
        fprintf(stderr, "%s: %s\n", name, fs_error(err));
        exit(1);
    }
}

static void make_folders(const char *name) {
    char folder[FS_NAME_LEN];

    for (size_t i = 0; name[i] != '\0'; i++) {
        if (name[i] != '/' || i == 0 || i + 1 >= sizeof folder) {
            continue;
        }
        memcpy(folder, name, i);
        folder[i] = '\0';
        make_folder(folder);
    }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s IMAGE SECTORS ROOT KERNEL_BIN [FILE...]\n", argv[0]);
        return 1;
    }
    uint32_t sectors = (uint32_t)strtoul(argv[2], NULL, 0);
    const char *root = argv[3];

    image = open(argv[1], O_RDWR | O_CREAT, 0644);
    if (image < 0 || ftruncate(image, (off_t)sectors * SECTOR_SIZE) < 0) {
        perror(argv[1]);
        return 1;
    }

    size_t size;

    if (fs_init() != 0 && fs_format(sectors) != 0) {
        fprintf(stderr, "%s: cannot format\n", argv[1]);
        return 1;
    }

    /* The kernel first, into the folder the disk keeps it in. */
    void *kernel = load(argv[4], &size);
    make_folders(KERNEL_NAME);
    int err = fs_write(KERNEL_NAME, kernel, size);
    if (err < 0) {
        fprintf(stderr, "%s: %s\n", argv[4], fs_error(err));
        return 1;
    }
    free(kernel);

    for (int i = 5; i < argc; i++) {
        /* Either a path under ROOT, which keeps the name it has there, or
           "name=path", which puts a file built elsewhere - a utility out of
           the build directory - at a name of its own on the disk. */
        const char *path = argv[i];
        const char *mark = strchr(argv[i], '=');
        char given[FS_NAME_LEN];
        const char *name;

        if (mark != NULL && (size_t)(mark - argv[i]) < sizeof given) {
            memcpy(given, argv[i], (size_t)(mark - argv[i]));
            given[mark - argv[i]] = '\0';
            name = given;
            path = mark + 1;
        } else {
            name = stored_name(root, path);
        }

        if (strlen(name) >= FS_NAME_LEN) {
            fprintf(stderr, "%s: path is over %d characters\n", name, FS_NAME_LEN - 1);
            return 1;
        }
        make_folders(name);

        char target[FS_LINK_LEN];
        ssize_t length = readlink(path, target, sizeof target - 1);

        if (length > 0) {
            target[length] = '\0';
            err = fs_symlink(target, name);
            if (err < 0 && err != FS_EEXIST) {
                fprintf(stderr, "%s: %s\n", path, fs_error(err));
                return 1;
            }
            continue;               /* a link, however it resolves on the host */
        }
        if (is_folder(path)) {
            make_folder(name);      /* it may have nothing in it yet */
            continue;
        }
        void *data = load(path, &size);
        err = fs_write(name, data, size);
        if (err < 0) {
            fprintf(stderr, "%s: %s\n", path, fs_error(err));
            return 1;
        }
        free(data);
    }
    return 0;
}
