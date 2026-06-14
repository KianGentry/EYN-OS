#![no_std]

use core::ffi::c_void;
use core::num::NonZeroU32;
use core::ptr;
use core::ptr::NonNull;

#[derive(Copy, Clone, Eq, PartialEq)]
enum BufferError {
    InvalidArg,
    OutOfRange,
    NoBuffer,
}

impl BufferError {
    fn as_errno(self) -> i32 {
        match self {
            BufferError::InvalidArg => -1,
            BufferError::OutOfRange => -1,
            BufferError::NoBuffer => -1,
        }
    }
}

#[repr(C)]
pub struct ZeroCopyBuffer {
    pub buffer: *mut c_void,
    pub size: u32,
    pub offset: u32,
    pub drive: u8,
    pub block_start: u32,
    pub dirty: u8,
}

#[repr(C)]
pub struct RustHeapBlockHeader {
    pub size: u32,
    pub used: u32,
    pub next: u32,
    pub magic: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Eq, PartialEq)]
pub enum RustHeapValidationResult {
    Ok = 0,
    NullBlock = 1,
    BadHeapBase = 2,
    HeapOverflow = 3,
    BlockOutsideHeap = 4,
    OffsetMismatch = 5,
    BlockRangeInvalid = 6,
    BlockSizeInvalid = 7,
}

unsafe extern "C" {
    fn malloc(nbytes: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
}

#[panic_handler]
fn panic_handler(_: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}

fn checked_end(offset: u32, size: u32, cap: u32) -> Result<usize, BufferError> {
    if offset > cap {
        return Err(BufferError::OutOfRange);
    }
    let end = offset.checked_add(size).ok_or(BufferError::OutOfRange)?;
    if end > cap {
        return Err(BufferError::OutOfRange);
    }
    Ok(end as usize)
}

unsafe fn alloc_zeroed(bytes: usize) -> *mut c_void {
    let p = unsafe { malloc(bytes) };
    if p.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        ptr::write_bytes(p.cast::<u8>(), 0, bytes);
    }
    p
}

fn buffer_ptr(buffer: &ZeroCopyBuffer) -> Result<NonNull<u8>, BufferError> {
    NonNull::new(buffer.buffer.cast::<u8>()).ok_or(BufferError::NoBuffer)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_zero_copy_buffer_create(size: u32) -> *mut ZeroCopyBuffer {
    let meta_ptr = unsafe { alloc_zeroed(core::mem::size_of::<ZeroCopyBuffer>()) as *mut ZeroCopyBuffer };
    if meta_ptr.is_null() {
        return ptr::null_mut();
    }

    let data_ptr = if let Some(non_zero) = NonZeroU32::new(size) {
        unsafe { alloc_zeroed(non_zero.get() as usize) }
    } else {
        ptr::null_mut()
    };

    if size != 0 && data_ptr.is_null() {
        unsafe { free(meta_ptr as *mut c_void) };
        return ptr::null_mut();
    }

    unsafe {
        (*meta_ptr).buffer = data_ptr;
        (*meta_ptr).size = size;
        (*meta_ptr).offset = 0;
        (*meta_ptr).drive = 0;
        (*meta_ptr).block_start = 0;
        (*meta_ptr).dirty = 0;
    }

    meta_ptr
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_zero_copy_buffer_destroy(buffer: *mut ZeroCopyBuffer) {
    if buffer.is_null() {
        return;
    }

    unsafe {
        if !(*buffer).buffer.is_null() {
            free((*buffer).buffer);
        }
        free(buffer as *mut c_void);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_zero_copy_buffer_read(
    buffer: *const ZeroCopyBuffer,
    data: *mut c_void,
    offset: u32,
    size: u32,
) -> i32 {
    if buffer.is_null() || data.is_null() {
        return BufferError::InvalidArg.as_errno();
    }

    let buf = unsafe { &*buffer };
    if checked_end(offset, size, buf.size).is_err() {
        return BufferError::OutOfRange.as_errno();
    }
    if size == 0 {
        return 0;
    }

    let src_base = match buffer_ptr(buf) {
        Ok(p) => p,
        Err(e) => return e.as_errno(),
    };

    let src = unsafe { src_base.as_ptr().add(offset as usize) };
    let dst = data.cast::<u8>();

    unsafe {
        ptr::copy_nonoverlapping(src, dst, size as usize);
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_zero_copy_buffer_write(
    buffer: *mut ZeroCopyBuffer,
    data: *const c_void,
    offset: u32,
    size: u32,
) -> i32 {
    if buffer.is_null() {
        return BufferError::InvalidArg.as_errno();
    }
    if size != 0 && data.is_null() {
        return BufferError::InvalidArg.as_errno();
    }

    let buf = unsafe { &mut *buffer };
    if checked_end(offset, size, buf.size).is_err() {
        return BufferError::OutOfRange.as_errno();
    }
    if size == 0 {
        return 0;
    }

    let dst_base = match buffer_ptr(buf) {
        Ok(p) => p,
        Err(e) => return e.as_errno(),
    };

    let dest = unsafe { dst_base.as_ptr().add(offset as usize) };
    let src = data.cast::<u8>();

    unsafe {
        ptr::copy_nonoverlapping(src, dest, size as usize);
    }
    buf.dirty = 1;
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_validate_heap_block(
    block: *const RustHeapBlockHeader,
    offset: u32,
    heap_start: *const u8,
    heap_size: u32,
    heap_size_min: u32,
    block_header_size: u32,
) -> RustHeapValidationResult {
    if block.is_null() {
        return RustHeapValidationResult::NullBlock;
    }

    let heap_begin = heap_start as usize;
    if heap_begin == 0 || heap_begin < 0x100000usize || heap_size < heap_size_min {
        return RustHeapValidationResult::BadHeapBase;
    }

    let heap_end = match heap_begin.checked_add(heap_size as usize) {
        Some(v) if v > heap_begin => v,
        _ => return RustHeapValidationResult::HeapOverflow,
    };

    let block_addr = block as usize;
    let header_end = match block_addr.checked_add(core::mem::size_of::<RustHeapBlockHeader>()) {
        Some(v) => v,
        None => return RustHeapValidationResult::BlockOutsideHeap,
    };

    if block_addr < heap_begin || header_end > heap_end {
        return RustHeapValidationResult::BlockOutsideHeap;
    }

    let expected_addr = match heap_begin.checked_add(offset as usize) {
        Some(v) => v,
        None => return RustHeapValidationResult::OffsetMismatch,
    };
    if block_addr != expected_addr {
        return RustHeapValidationResult::OffsetMismatch;
    }

    let block_ref = unsafe { &*block };

    let off_plus_hdr = match offset.checked_add(block_header_size) {
        Some(v) => v,
        None => return RustHeapValidationResult::BlockRangeInvalid,
    };
    let off_plus_size = match offset.checked_add(block_ref.size) {
        Some(v) => v,
        None => return RustHeapValidationResult::BlockRangeInvalid,
    };

    if offset >= heap_size || off_plus_hdr > heap_size || off_plus_size > heap_size {
        return RustHeapValidationResult::BlockRangeInvalid;
    }

    if block_ref.size < block_header_size || block_ref.size > heap_size || block_ref.size == 0 {
        return RustHeapValidationResult::BlockSizeInvalid;
    }

    RustHeapValidationResult::Ok
}
