#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <types.h>

#define CRASHLOG_OBJ_UI_PREFS 1

#define CRASHLOG_MAX_RECORDS 16
#define CRASHLOG_MAX_DATA 128

typedef struct crashlog_record_info_t {
    uint32 obj_type;
    uint32 obj_id;
    uint32 epoch;
    uint32 data_len;
    uint32 checksum;
} crashlog_record_info_t;

int crashlog_init(uint8 drive);
int crashlog_checkpoint(uint32 obj_type, uint32 obj_id, uint32 epoch,
                        const void* data, uint32 len);
int crashlog_recover_latest(uint32 obj_type, uint32 obj_id,
                            void* out, uint32 out_cap, uint32* out_epoch);
int crashlog_get_record_count(void);
int crashlog_get_record_info(uint32 index, crashlog_record_info_t* out);
int crashlog_get_record_data(uint32 index, void* out, uint32 out_cap);
int crashlog_clear(void);

#endif
