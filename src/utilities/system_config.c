#include <system_config.h>

#include <ata.h>
#include <fs/vfs.h>
#include <vga.h>
#include <string.h>
#include <util.h>

#define SYS_CFG_PATH_PRIMARY "/config/system.cfg"
#define SYS_CFG_PATH_FALLBACK "/system.cfg"

static uint8 g_save_drive = 0;
static uint8 g_install_drive_logical = 0;
static char g_install_bin_path[64] = "/binaries";
static char g_drive_labels[ATA_MAX_DRIVES][SYS_CFG_LABEL_MAX + 1];

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return s;
}

static int starts_with(const char* s, const char* pfx) {
    while (*pfx) {
        if (*s++ != *pfx++) return 0;
    }
    return 1;
}

static int parse_u8(const char* s, int* out_v) {
    int v = 0;
    int any = 0;
    s = skip_ws(s);
    while (*s >= '0' && *s <= '9') {
        any = 1;
        v = (v * 10) + (*s - '0');
        if (v > 255) v = 255;
        s++;
    }
    if (!any) return -1;
    if (out_v) *out_v = v;
    return 0;
}

static int read_cfg(uint8 drive, const char* path, char* out, int out_cap) {
    int n;
    if (!out || out_cap <= 1) return -1;
    n = vfs_read_file(drive, path, out, out_cap - 1);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
}

static int is_label_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '-' || c == '_') return 1;
    return 0;
}

void system_config_init_defaults(void) {
    int i;
    g_install_drive_logical = 0;
    strncpy(g_install_bin_path, "/binaries", sizeof(g_install_bin_path) - 1);
    g_install_bin_path[sizeof(g_install_bin_path) - 1] = '\0';
    for (i = 0; i < ATA_MAX_DRIVES; ++i) {
        g_drive_labels[i][0] = '\0';
    }
}

void system_config_set_save_drive(uint8 drive) {
    g_save_drive = drive;
}

int system_config_validate_label(const char* label) {
    int i = 0;
    if (!label || !label[0]) return -1;
    while (label[i]) {
        if (i >= SYS_CFG_LABEL_MAX) return -1;
        if (!is_label_char(label[i])) return -1;
        i++;
    }
    return 0;
}

int system_config_set_drive_label(uint8 logical_drive, const char* label) {
    if (logical_drive >= ATA_MAX_DRIVES) return -1;
    if (!label || !label[0]) {
        g_drive_labels[logical_drive][0] = '\0';
        return 0;
    }
    if (system_config_validate_label(label) != 0) return -1;
    strncpy(g_drive_labels[logical_drive], label, SYS_CFG_LABEL_MAX);
    g_drive_labels[logical_drive][SYS_CFG_LABEL_MAX] = '\0';
    return 0;
}

const char* system_config_get_drive_label_ptr(uint8 logical_drive) {
    if (logical_drive >= ATA_MAX_DRIVES) return NULL;
    if (!g_drive_labels[logical_drive][0]) return NULL;
    return g_drive_labels[logical_drive];
}

int system_config_get_drive_label(uint8 logical_drive, char* out, int out_cap) {
    const char* src = system_config_get_drive_label_ptr(logical_drive);
    if (!out || out_cap <= 0) return -1;
    out[0] = '\0';
    if (!src) return -1;
    strncpy(out, src, (size_t)out_cap - 1u);
    out[out_cap - 1] = '\0';
    return 0;
}

uint8 system_config_get_install_drive_logical(void) {
    return g_install_drive_logical;
}

int system_config_set_install_drive_logical(uint8 logical_drive) {
    if (!ata_logical_drive_present(logical_drive)) return -1;
    g_install_drive_logical = logical_drive;
    return 0;
}

const char* system_config_get_install_bin_path(void) {
    return g_install_bin_path;
}

int system_config_set_install_bin_path(const char* path) {
    if (!path || path[0] != '/') return -1;
    if (strlen(path) >= sizeof(g_install_bin_path)) return -1;
    strncpy(g_install_bin_path, path, sizeof(g_install_bin_path) - 1);
    g_install_bin_path[sizeof(g_install_bin_path) - 1] = '\0';
    return 0;
}

uint8 system_config_get_install_drive_physical(void) {
    uint8 physical = ata_logical_to_physical(g_install_drive_logical);
    if (physical != 0xFFu) return physical;

    if (ata_logical_drive_present(0)) {
        physical = ata_logical_to_physical(0);
        if (physical != 0xFFu) return physical;
    }

    {
        uint8 count = ata_get_num_logical_drives();
        uint8 logical;
        for (logical = 0; logical < count; ++logical) {
            if (!ata_logical_drive_present(logical)) continue;
            physical = ata_logical_to_physical(logical);
            if (physical != 0xFFu) return physical;
        }
    }

    return 0;
}

int system_config_load(uint8 drive) {
    char buf[1024];
    char* line;

    system_config_init_defaults();
    g_save_drive = drive;

    if (read_cfg(drive, SYS_CFG_PATH_PRIMARY, buf, (int)sizeof(buf)) != 0) {
        if (read_cfg(drive, SYS_CFG_PATH_FALLBACK, buf, (int)sizeof(buf)) != 0) {
            return -1;
        }
    }

    line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        const char* key;
        const char* val;
        char* eq;
        if (next) {
            *next = '\0';
            next++;
        }

        key = skip_ws(line);
        if (!*key || *key == '#') {
            line = next;
            continue;
        }

        eq = strchr((char*)key, '=');
        if (!eq) {
            line = next;
            continue;
        }
        *eq = '\0';
        val = skip_ws(eq + 1);

        if (starts_with(key, "install_drive")) {
            int v = 0;
            if (parse_u8(val, &v) == 0 && ata_logical_drive_present((uint8)v)) {
                g_install_drive_logical = (uint8)v;
            }
        } else if (starts_with(key, "install_bin_path")) {
            (void)system_config_set_install_bin_path(val);
        } else if (starts_with(key, "label.")) {
            int logical = -1;
            if (parse_u8(key + 6, &logical) == 0 && logical >= 0 && logical < ATA_MAX_DRIVES) {
                (void)system_config_set_drive_label((uint8)logical, val);
            }
        }

        line = next;
    }

    return 0;
}

int system_config_save(void) {
    char out[1024];
    int n;
    int i;

    n = snprintf(out, sizeof(out),
        "# EYN-OS System Config\n"
        "install_drive=%u\n"
        "install_bin_path=%s\n",
        (unsigned)g_install_drive_logical,
        g_install_bin_path);
    if (n <= 0) return -1;

    for (i = 0; i < ATA_MAX_DRIVES; ++i) {
        int wrote;
        if (!g_drive_labels[i][0]) continue;
        wrote = snprintf(out + n, sizeof(out) - (size_t)n, "label.%d=%s\n", i, g_drive_labels[i]);
        if (wrote <= 0 || n + wrote >= (int)sizeof(out)) break;
        n += wrote;
    }

    (void)vfs_mkdir(g_save_drive, "/config");
    if (vfs_write_file(g_save_drive, SYS_CFG_PATH_PRIMARY, out, (uint32)n) >= 0) return 0;
    if (vfs_write_file(g_save_drive, SYS_CFG_PATH_FALLBACK, out, (uint32)n) >= 0) return 0;
    return -1;
}
