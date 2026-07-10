#![no_std]

use core::panic::PanicInfo;

#[repr(C)]
pub struct RustEynfsCacheEntry {
    pub block_num: u32,
    pub last_use: u32,
    pub data: *mut u8,
    pub dirty: u8,
    pub valid: u8,
}

#[repr(C)]
#[derive(Copy, Clone, Eq, PartialEq)]
pub enum RustEynfsCacheValidationResult {
    Valid = 0,
    BadTablePtr = 1,
    BadDataPtr = 2,
    BadCapacity = 3,
    BadEntryPtr = 4,
    BadPhysmap = 5,
}

#[repr(C)]
#[derive(Copy, Clone, Eq, PartialEq)]
pub enum RustEynfsVictimResult {
    None = 0,
    FoundClean = 1,
    FoundDirty = 2,
    FoundInvalid = 3,
}

#[panic_handler]
fn panic_handler(_: &PanicInfo<'_>) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_eynfs_validate_block_cache(
    entries: *const RustEynfsCacheEntry,
    data_base: *const u8,
    capacity: u32,
    kernel_base: usize,
    physmap_end: usize,
    block_size: u32,
) -> RustEynfsCacheValidationResult {
    if physmap_end <= kernel_base {
        return RustEynfsCacheValidationResult::BadPhysmap;
    }
    if entries.is_null() {
        return RustEynfsCacheValidationResult::BadTablePtr;
    }
    if data_base.is_null() {
        return RustEynfsCacheValidationResult::BadDataPtr;
    }
    if capacity == 0 || capacity > 1024 {
        return RustEynfsCacheValidationResult::BadCapacity;
    }
    if block_size == 0 {
        return RustEynfsCacheValidationResult::BadCapacity;
    }

    let table_addr = entries as usize;
    let data_addr = data_base as usize;
    if table_addr < kernel_base || table_addr >= physmap_end {
        return RustEynfsCacheValidationResult::BadTablePtr;
    }
    if data_addr < kernel_base || data_addr >= physmap_end {
        return RustEynfsCacheValidationResult::BadDataPtr;
    }

    let slice = unsafe { core::slice::from_raw_parts(entries, capacity as usize) };
    for (index, entry) in slice.iter().enumerate() {
        let expected = match data_addr.checked_add(index * block_size as usize) {
            Some(v) => v,
            None => return RustEynfsCacheValidationResult::BadEntryPtr,
        };
        if entry.data as usize != expected {
            return RustEynfsCacheValidationResult::BadEntryPtr;
        }
    }

    RustEynfsCacheValidationResult::Valid
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_eynfs_choose_victim_index(
    entries: *const RustEynfsCacheEntry,
    capacity: u32,
    out_index: *mut u32,
) -> RustEynfsVictimResult {
    if entries.is_null() || out_index.is_null() || capacity == 0 {
        return RustEynfsVictimResult::None;
    }

    let slice = unsafe { core::slice::from_raw_parts(entries, capacity as usize) };

    let mut found_clean = None::<(u32, u32)>;
    let mut found_dirty = None::<(u32, u32)>;

    for (index, entry) in slice.iter().enumerate() {
        let idx = index as u32;
        if entry.valid == 0 {
            unsafe { *out_index = idx; }
            return RustEynfsVictimResult::FoundInvalid;
        }

        if entry.dirty != 0 {
            match found_dirty {
                None => found_dirty = Some((idx, entry.last_use)),
                Some((_, best_use)) if entry.last_use < best_use => {
                    found_dirty = Some((idx, entry.last_use));
                }
                _ => {}
            }
            continue;
        }

        match found_clean {
            None => found_clean = Some((idx, entry.last_use)),
            Some((_, best_use)) if entry.last_use < best_use => {
                found_clean = Some((idx, entry.last_use));
            }
            _ => {}
        }
    }

    if let Some((idx, _)) = found_clean {
        unsafe { *out_index = idx; }
        return RustEynfsVictimResult::FoundClean;
    }

    if let Some((idx, _)) = found_dirty {
        unsafe { *out_index = idx; }
        return RustEynfsVictimResult::FoundDirty;
    }

    RustEynfsVictimResult::None
}