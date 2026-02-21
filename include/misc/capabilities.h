#ifndef CAPABILITIES_H
#define CAPABILITIES_H

#include <misc/types.h>

// Capability object types
enum {
    CAP_OBJ_NONE = 0,
    CAP_OBJ_USER_FD = 1,
    CAP_OBJ_USER_GUI = 2,
    CAP_OBJ_THREAD = 3,
    CAP_OBJ_IPC = 4,
    CAP_OBJ_MEMORY = 5,
};

// Capability rights
enum {
    CAP_R_READ  = 1u << 0,
    CAP_R_WRITE = 1u << 1,
    CAP_R_EXEC  = 1u << 2,
    CAP_R_SIGNAL = 1u << 3,
    CAP_R_GRANT = 1u << 4,
    CAP_R_CLOSE = 1u << 5,
    CAP_R_SEEK  = 1u << 6,
};

typedef struct cap_t {
    uint32 obj;
    uint32 type;
    uint32 rights;
    uint32 epoch;
    uint32 tag_lo;
    uint32 tag_hi;
} cap_t;

void cap_init(void);
int cap_register_object(void* obj, uint32 type);
int cap_revoke_object(void* obj, uint32 type);
int cap_mint(cap_t* out, void* obj, uint32 type, uint32 rights);
int cap_validate(const cap_t* cap, void* obj, uint32 type, uint32 required_rights);

#endif
