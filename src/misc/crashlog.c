#include <crashlog.h>
#include <fs/vfs.h>
#include <string.h>

#define CRASHLOG_PATH_PRIMARY "/config/kobj.log"
#define CRASHLOG_PATH_FALLBACK "/kobj.log"
#define CRASHLOG_MAGIC 0x4B4F4A31u
#define CRASHLOG_VERSION 1u

typedef struct crashlog_header_t {
    uint32 magic;
    uint32 version;
    uint32 record_count;
    uint32 _pad;
} crashlog_header_t;

typedef struct crashlog_record_t {
    uint32 magic;
    uint32 obj_type;
    uint32 obj_id;
    uint32 epoch;
    uint32 data_len;
    uint32 checksum;
    uint8 data[CRASHLOG_MAX_DATA];
} crashlog_record_t;

typedef struct crashlog_state_t {
    uint8 drive;
    crashlog_header_t header;
    crashlog_record_t records[CRASHLOG_MAX_RECORDS];
} crashlog_state_t;

static crashlog_state_t g_crashlog;

static uint32 crashlog_checksum(const void* data, uint32 len) {
    const uint8* p = (const uint8*)data;
    uint32 h = 2166136261u;
    for (uint32 i = 0; i < len; ++i) {
        h ^= (uint32)p[i];
        h *= 16777619u;
    }
    return h;
}

static int crashlog_write_file(const crashlog_state_t* st) {
    if (!st) return -1;
    int w = vfs_write_file(st->drive, CRASHLOG_PATH_PRIMARY, st, (uint32)sizeof(*st));
    if (w < 0) {
        w = vfs_write_file(st->drive, CRASHLOG_PATH_FALLBACK, st, (uint32)sizeof(*st));
    }
    return (w < 0) ? -1 : 0;
}

static int crashlog_read_file(uint8 drive, crashlog_state_t* out) {
    if (!out) return -1;
    int n = vfs_read_file(drive, CRASHLOG_PATH_PRIMARY, out, (int)sizeof(*out));
    if (n < 0) {
        n = vfs_read_file(drive, CRASHLOG_PATH_FALLBACK, out, (int)sizeof(*out));
    }
    if (n != (int)sizeof(*out)) return -1;
    return 0;
}

static int crashlog_validate(crashlog_state_t* st) {
    if (!st) return -1;
    if (st->header.magic != CRASHLOG_MAGIC) return -1;
    if (st->header.version != CRASHLOG_VERSION) return -1;
    if (st->header.record_count > CRASHLOG_MAX_RECORDS) return -1;

    for (uint32 i = 0; i < st->header.record_count; ++i) {
        crashlog_record_t* r = &st->records[i];
        if (r->magic != CRASHLOG_MAGIC) return -1;
        if (r->data_len > CRASHLOG_MAX_DATA) return -1;
        if (crashlog_checksum(r->data, r->data_len) != r->checksum) return -1;
    }

    return 0;
}

static int crashlog_ensure_config_dir(uint8 drive) {
    vfs_stat_t st;
    if (vfs_stat(drive, "/config", &st) == 0 && st.type == VFS_NODE_DIR) return 0;
    return vfs_mkdir(drive, "/config");
}

int crashlog_init(uint8 drive) {
    memset(&g_crashlog, 0, sizeof(g_crashlog));
    g_crashlog.drive = drive;
    g_crashlog.header.magic = CRASHLOG_MAGIC;
    g_crashlog.header.version = CRASHLOG_VERSION;
    g_crashlog.header.record_count = 0;

    crashlog_state_t tmp;
    if (crashlog_read_file(drive, &tmp) == 0) {
        if (crashlog_validate(&tmp) == 0) {
            g_crashlog = tmp;
            g_crashlog.drive = drive;
            return 0;
        }
    }

    crashlog_ensure_config_dir(drive);
    return crashlog_write_file(&g_crashlog);
}

int crashlog_checkpoint(uint32 obj_type, uint32 obj_id, uint32 epoch,
                        const void* data, uint32 len) {
    if (!data || len == 0 || len > CRASHLOG_MAX_DATA) return -1;

    crashlog_record_t r;
    memset(&r, 0, sizeof(r));
    r.magic = CRASHLOG_MAGIC;
    r.obj_type = obj_type;
    r.obj_id = obj_id;
    r.epoch = epoch;
    r.data_len = len;
    memcpy(r.data, data, len);
    r.checksum = crashlog_checksum(r.data, r.data_len);

    uint32 idx = 0;
    if (g_crashlog.header.record_count < CRASHLOG_MAX_RECORDS) {
        idx = g_crashlog.header.record_count++;
    } else {
        idx = (uint32)(epoch % CRASHLOG_MAX_RECORDS);
    }

    g_crashlog.records[idx] = r;
    return crashlog_write_file(&g_crashlog);
}

int crashlog_recover_latest(uint32 obj_type, uint32 obj_id,
                            void* out, uint32 out_cap, uint32* out_epoch) {
    if (!out || out_cap == 0) return -1;

    uint32 best_epoch = 0;
    int found = 0;
    crashlog_record_t* best = NULL;

    for (uint32 i = 0; i < g_crashlog.header.record_count; ++i) {
        crashlog_record_t* r = &g_crashlog.records[i];
        if (r->magic != CRASHLOG_MAGIC) continue;
        if (r->obj_type != obj_type || r->obj_id != obj_id) continue;
        if (r->data_len == 0 || r->data_len > CRASHLOG_MAX_DATA) continue;
        if (crashlog_checksum(r->data, r->data_len) != r->checksum) continue;
        if (!found || r->epoch > best_epoch) {
            best_epoch = r->epoch;
            best = r;
            found = 1;
        }
    }

    if (!found || !best) return -1;
    uint32 to_copy = (best->data_len < out_cap) ? best->data_len : out_cap;
    memcpy(out, best->data, to_copy);
    if (out_epoch) *out_epoch = best_epoch;
    return (int)to_copy;
}

int crashlog_get_record_count(void) {
    return (int)g_crashlog.header.record_count;
}

int crashlog_get_record_info(uint32 index, crashlog_record_info_t* out) {
    if (!out) return -1;
    if (index >= g_crashlog.header.record_count) return -1;

    crashlog_record_t* r = &g_crashlog.records[index];
    if (r->magic != CRASHLOG_MAGIC) return -1;
    if (r->data_len > CRASHLOG_MAX_DATA) return -1;
    if (crashlog_checksum(r->data, r->data_len) != r->checksum) return -1;

    out->obj_type = r->obj_type;
    out->obj_id = r->obj_id;
    out->epoch = r->epoch;
    out->data_len = r->data_len;
    out->checksum = r->checksum;
    return 0;
}

int crashlog_get_record_data(uint32 index, void* out, uint32 out_cap) {
    if (!out || out_cap == 0) return -1;
    if (index >= g_crashlog.header.record_count) return -1;

    crashlog_record_t* r = &g_crashlog.records[index];
    if (r->magic != CRASHLOG_MAGIC) return -1;
    if (r->data_len > CRASHLOG_MAX_DATA) return -1;
    if (crashlog_checksum(r->data, r->data_len) != r->checksum) return -1;

    uint32 to_copy = (r->data_len < out_cap) ? r->data_len : out_cap;
    memcpy(out, r->data, to_copy);
    return (int)to_copy;
}

int crashlog_clear(void) {
    g_crashlog.header.record_count = 0;
    memset(g_crashlog.records, 0, sizeof(g_crashlog.records));
    return crashlog_write_file(&g_crashlog);
}
