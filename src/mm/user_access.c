#include <mm/user_access.h>

#include <mm/vmm.h>
#include <string.h>

static int user_page_ok(uint32 va, int write) {
    // Reject kernel addresses.
    if (va >= KERNEL_BASE) {
        return 0;
    }

    // Enforce the null-guarded user region.
    if (va < USER_CODE_BASE || va >= USER_STACK_TOP) {
        return 0;
    }

    address_space_t* as = vmm_current_as ? vmm_current_as : &vmm_kernel_as;
    pte_t* pte = vmm_walk_page_tables(as, va, 0);
    if (!pte) {
        return 0;
    }

    uint32 flags = *pte;
    if ((flags & PTE_PRESENT) == 0) {
        return 0;
    }
    if ((flags & PTE_USER) == 0) {
        return 0;
    }
    if (write && ((flags & PTE_RW) == 0)) {
        return 0;
    }

    return 1;
}

int user_access_ok(const void* user_ptr, size_t len, int write) {
    if (len == 0) {
        return 1;
    }
    if (!user_ptr) {
        return 0;
    }

    uint32 start = (uint32)user_ptr;
    uint32 end = start + (uint32)len;
    // Overflow check
    if (end < start) {
        return 0;
    }

    // Must remain in user space.
    if (start < USER_CODE_BASE || end > USER_STACK_TOP) {
        return 0;
    }

    uint32 page = start & PAGE_MASK;
    uint32 end_page = (end - 1) & PAGE_MASK;

    for (;;) {
        if (!user_page_ok(page, write)) {
            return 0;
        }
        if (page == end_page) {
            break;
        }
        page += PAGE_SIZE;
    }

    return 1;
}

int copyin(void* dst, const void* user_src, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (!dst) {
        return -1;
    }
    if (!user_access_ok(user_src, len, 0)) {
        return -1;
    }

    memcpy(dst, user_src, len);
    return 0;
}

int copyout(void* user_dst, const void* src, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (!src) {
        return -1;
    }
    if (!user_access_ok(user_dst, len, 1)) {
        return -1;
    }

    memcpy(user_dst, src, len);
    return 0;
}
