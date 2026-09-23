#pragma once

#include <stdint.h>

/* The part of UEFI this kernel uses.
 *
 * Firmware is called through function pointers in tables it hands us, using
 * Microsoft's calling convention rather than the SysV one the kernel is
 * built with - hence ms_abi on every one of them, which lets the compiler
 * make the switch at the call site. The loader is built by a compiler where
 * that is already the default, and the attribute changes nothing there.
 *
 * Structures have to match the firmware's byte for byte, so the fields that
 * matter carry a static assertion on their offset: a mistake here would show
 * up as a machine that hangs with a blank screen and nothing to go on. */

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t efi_status;
typedef uint64_t efi_uintn;
typedef void    *efi_handle;

#define EFI_SUCCESS       0
#define EFI_ERROR(s)      (((int64_t)(s)) < 0)
#define EFI_NOT_READY     0x8000000000000006ull
#define EFI_BUFFER_SMALL  0x8000000000000005ull

struct efi_guid {
    uint32_t a;
    uint16_t b, c;
    uint8_t  d[8];
};

#define EFI_GOP_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_FS_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_BLOCK_IO_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_TEXT_INPUT_GUID \
    { 0x387477c1, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_DEVICE_PATH_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* One node of a device path. They follow each other, length bytes apart,
   until one of type 0x7F. */
struct efi_device_path {
    uint8_t type, subtype;
    uint8_t length[2];
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

/* ---- text in and out ---------------------------------------------------- */

struct efi_input_key {
    uint16_t scan_code;
    uint16_t unicode_char;
};

struct efi_text_input {
    efi_status (EFIAPI *reset)(struct efi_text_input *, uint8_t extended);
    efi_status (EFIAPI *read_key)(struct efi_text_input *, struct efi_input_key *);
    void       *wait_for_key;
};

struct efi_text_output {
    void       *reset;
    efi_status (EFIAPI *output_string)(struct efi_text_output *, const uint16_t *);
    /* The rest is unused. */
};

/* ---- graphics ----------------------------------------------------------- */

enum {
    EFI_PIXEL_RGBX = 0,     /* red in the low byte of each 32-bit pixel */
    EFI_PIXEL_BGRX = 1,     /* blue in the low byte, which is what we want */
    EFI_PIXEL_MASK = 2,
    EFI_PIXEL_BLT  = 3,     /* no framebuffer to write at all */
};

struct efi_gop_info {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t mask[4];
    uint32_t pixels_per_scanline;    /* the pitch, in pixels rather than bytes */
};

struct efi_gop_mode {
    uint32_t             max_mode;
    uint32_t             mode;
    struct efi_gop_info *info;
    efi_uintn            info_size;
    uint64_t             framebuffer;
    efi_uintn            framebuffer_size;
};

struct efi_gop {
    efi_status (EFIAPI *query_mode)(struct efi_gop *, uint32_t mode,
                                    efi_uintn *size, struct efi_gop_info **info);
    efi_status (EFIAPI *set_mode)(struct efi_gop *, uint32_t mode);
    void                *blt;
    struct efi_gop_mode *mode;
};

/* ---- block devices ------------------------------------------------------ */

struct efi_block_media {
    uint32_t media_id;
    uint8_t  removable;
    uint8_t  present;
    uint8_t  logical_partition;
    uint8_t  read_only;
    uint8_t  write_caching;
    uint8_t  pad[3];
    uint32_t block_size;
    uint32_t io_align;
    uint32_t pad2;
    uint64_t last_block;
};

struct efi_block_io {
    uint64_t                revision;
    struct efi_block_media *media;
    efi_status (EFIAPI *reset)(struct efi_block_io *, uint8_t extended);
    efi_status (EFIAPI *read_blocks)(struct efi_block_io *, uint32_t media_id,
                                     uint64_t lba, efi_uintn size, void *buffer);
    efi_status (EFIAPI *write_blocks)(struct efi_block_io *, uint32_t media_id,
                                      uint64_t lba, efi_uintn size, const void *buffer);
    efi_status (EFIAPI *flush_blocks)(struct efi_block_io *);
};

/* ---- the image we were loaded as ---------------------------------------- */

struct efi_loaded_image {
    uint32_t    revision;
    uint32_t    pad;
    efi_handle  parent;
    void       *system_table;
    efi_handle  device;         /* what we were loaded from */
    /* The rest is unused. */
};

/* ---- files, for the loader ---------------------------------------------- */

#define EFI_FILE_MODE_READ 0x0000000000000001ull

struct efi_file {
    uint64_t   revision;
    efi_status (EFIAPI *open)(struct efi_file *, struct efi_file **,
                              const uint16_t *name, uint64_t mode, uint64_t attr);
    efi_status (EFIAPI *close)(struct efi_file *);
    void       *delete;
    efi_status (EFIAPI *read)(struct efi_file *, efi_uintn *size, void *buffer);
    void       *write;
    void       *get_position;
    efi_status (EFIAPI *set_position)(struct efi_file *, uint64_t position);
    void       *get_info;
    /* The rest is unused. */
};

struct efi_simple_fs {
    uint64_t   revision;
    efi_status (EFIAPI *open_volume)(struct efi_simple_fs *, struct efi_file **root);
};

/* ---- services ----------------------------------------------------------- */

enum { EFI_ALLOCATE_ANY = 0, EFI_ALLOCATE_MAX = 1, EFI_ALLOCATE_ADDRESS = 2 };
enum { EFI_LOADER_DATA = 2, EFI_CONVENTIONAL_MEMORY = 7 };

struct efi_memory_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical;
    uint64_t virt;
    uint64_t pages;
    uint64_t attribute;
};

struct efi_boot_services {
    struct efi_table_header hdr;
    void *raise_tpl, *restore_tpl;
    efi_status (EFIAPI *allocate_pages)(uint32_t type, uint32_t memory_type,
                                        efi_uintn pages, uint64_t *address);
    efi_status (EFIAPI *free_pages)(uint64_t address, efi_uintn pages);
    efi_status (EFIAPI *get_memory_map)(efi_uintn *size, struct efi_memory_descriptor *map,
                                        efi_uintn *key, efi_uintn *descriptor_size,
                                        uint32_t *version);
    efi_status (EFIAPI *allocate_pool)(uint32_t type, efi_uintn size, void **buffer);
    efi_status (EFIAPI *free_pool)(void *buffer);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event,
         *check_event, *install_protocol, *reinstall_protocol, *uninstall_protocol;
    efi_status (EFIAPI *handle_protocol)(efi_handle, const struct efi_guid *, void **);
    void *reserved, *register_notify, *locate_handle, *locate_device_path,
         *install_configuration_table, *load_image, *start_image, *exit,
         *unload_image, *exit_boot_services, *get_next_monotonic_count;
    efi_status (EFIAPI *stall)(efi_uintn microseconds);
    efi_status (EFIAPI *set_watchdog_timer)(efi_uintn seconds, uint64_t code,
                                            efi_uintn size, uint16_t *data);
    efi_status (EFIAPI *connect_controller)(efi_handle controller, efi_handle *drivers,
                                            void *remaining_path, uint8_t recursive);
    efi_status (EFIAPI *disconnect_controller)(efi_handle controller, efi_handle driver,
                                               efi_handle child);
    void *open_protocol, *close_protocol,
         *open_protocol_information, *protocols_per_handle;
    efi_status (EFIAPI *locate_handle_buffer)(uint32_t search, const struct efi_guid *,
                                              void *key, efi_uintn *count,
                                              efi_handle **handles);
    efi_status (EFIAPI *locate_protocol)(const struct efi_guid *, void *registration,
                                         void **interface);
    /* The rest is unused. */
};

struct efi_time {
    uint16_t year;
    uint8_t  month, day, hour, minute, second, pad;
    uint32_t nanosecond;
    int16_t  time_zone;
    uint8_t  daylight, pad2;
};

struct efi_runtime_services {
    struct efi_table_header hdr;
    efi_status (EFIAPI *get_time)(struct efi_time *, void *capabilities);
    void *set_time, *get_wakeup_time, *set_wakeup_time, *set_virtual_address_map,
         *convert_pointer, *get_variable, *get_next_variable, *set_variable,
         *get_next_high_monotonic_count;
    void (EFIAPI *reset_system)(uint32_t type, efi_status status,
                                efi_uintn size, void *data);
};

#define EFI_RESET_COLD     0
#define EFI_RESET_SHUTDOWN 2

struct efi_system_table {
    struct efi_table_header      hdr;
    uint16_t                    *vendor;
    uint32_t                     revision;
    uint32_t                     pad;
    efi_handle                   console_in_handle;
    struct efi_text_input       *con_in;
    efi_handle                   console_out_handle;
    struct efi_text_output      *con_out;
    efi_handle                   standard_error_handle;
    struct efi_text_output      *std_err;
    struct efi_runtime_services *runtime;
    struct efi_boot_services    *boot;
    efi_uintn                    table_entries;
    void                        *configuration;
};

/* The offsets firmware actually writes these at. */
_Static_assert(sizeof(struct efi_table_header) == 24, "EFI_TABLE_HEADER");
_Static_assert(__builtin_offsetof(struct efi_system_table, con_in) == 48, "ConIn");
_Static_assert(__builtin_offsetof(struct efi_system_table, con_out) == 64, "ConOut");
_Static_assert(__builtin_offsetof(struct efi_system_table, runtime) == 88, "RuntimeServices");
_Static_assert(__builtin_offsetof(struct efi_system_table, boot) == 96, "BootServices");
_Static_assert(__builtin_offsetof(struct efi_boot_services, allocate_pages) == 40, "AllocatePages");
_Static_assert(__builtin_offsetof(struct efi_boot_services, get_memory_map) == 56, "GetMemoryMap");
_Static_assert(__builtin_offsetof(struct efi_boot_services, handle_protocol) == 152, "HandleProtocol");
_Static_assert(__builtin_offsetof(struct efi_boot_services, stall) == 248, "Stall");
_Static_assert(__builtin_offsetof(struct efi_boot_services, set_watchdog_timer) == 256, "SetWatchdogTimer");
_Static_assert(__builtin_offsetof(struct efi_boot_services, connect_controller) == 264, "ConnectController");
_Static_assert(__builtin_offsetof(struct efi_boot_services, locate_handle_buffer) == 312, "LocateHandleBuffer");
_Static_assert(__builtin_offsetof(struct efi_boot_services, locate_protocol) == 320, "LocateProtocol");
_Static_assert(__builtin_offsetof(struct efi_runtime_services, get_time) == 24, "GetTime");
_Static_assert(__builtin_offsetof(struct efi_runtime_services, reset_system) == 104, "ResetSystem");
_Static_assert(__builtin_offsetof(struct efi_gop, mode) == 24, "GOP Mode");
_Static_assert(__builtin_offsetof(struct efi_gop_mode, framebuffer) == 24, "FrameBufferBase");
_Static_assert(__builtin_offsetof(struct efi_gop_info, pixels_per_scanline) == 32, "PixelsPerScanLine");
_Static_assert(__builtin_offsetof(struct efi_block_media, last_block) == 24, "LastBlock");
_Static_assert(__builtin_offsetof(struct efi_block_io, read_blocks) == 24, "ReadBlocks");
_Static_assert(__builtin_offsetof(struct efi_loaded_image, device) == 24, "DeviceHandle");
_Static_assert(__builtin_offsetof(struct efi_file, read) == 32, "File Read");
