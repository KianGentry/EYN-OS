; 80386-safe memcpy/memset implementations for the freestanding kernel.
; - No CPUID/RDTSC/CMOV/etc.
; - Uses REP MOVSD/MOVSB and REP STOSD/STOSB only.
; - cdecl ABI: arguments on stack, return in EAX.

BITS 32

SECTION .text

GLOBAL memcpy
GLOBAL memset
GLOBAL bzero

; void* memcpy(void* dest, const void* src, size_t n)
memcpy:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi

    cld

    mov  edi, [ebp+8]     ; dest
    mov  esi, [ebp+12]    ; src
    mov  ecx, [ebp+16]    ; n
    mov  eax, edi         ; return dest

    test ecx, ecx
    jz   .mc_done

    ; Align destination to 4-byte boundary (copy bytes until aligned)
    mov  edx, edi
    and  edx, 3
    jz   .mc_dwords

    mov  ebx, 4
    sub  ebx, edx         ; bytes needed to reach 4-byte alignment
    cmp  ebx, ecx
    jbe  .mc_align_count_ok
    mov  ebx, ecx
.mc_align_count_ok:
    mov  edx, ecx         ; save remaining
    mov  ecx, ebx
    rep  movsb
    mov  ecx, edx
    sub  ecx, ebx

.mc_dwords:
    mov  edx, ecx         ; save remaining
    shr  ecx, 2
    rep  movsd
    mov  ecx, edx
    and  ecx, 3
    rep  movsb

.mc_done:
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret

; void* memset(void* s, int c, size_t n)
memset:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi

    cld

    mov  edi, [ebp+8]     ; s
    mov  eax, [ebp+12]    ; c (int)
    mov  ecx, [ebp+16]    ; n

    mov  edx, edi         ; return value

    test ecx, ecx
    jz   .ms_done

    ; Build 32-bit pattern in EAX from low byte
    and  eax, 0xFF
    mov  ebx, eax
    shl  ebx, 8
    or   eax, ebx
    mov  ebx, eax
    shl  ebx, 16
    or   eax, ebx

    ; Align destination to 4-byte boundary
    mov  ebx, edi
    and  ebx, 3
    jz   .ms_dwords

    mov  esi, 4
    sub  esi, ebx
    cmp  esi, ecx
    jbe  .ms_align_count_ok
    mov  esi, ecx
.ms_align_count_ok:
    mov  ebx, ecx         ; save remaining
    mov  ecx, esi
    rep  stosb
    mov  ecx, ebx
    sub  ecx, esi

.ms_dwords:
    mov  ebx, ecx         ; save remaining
    shr  ecx, 2
    rep  stosd
    mov  ecx, ebx
    and  ecx, 3
    rep  stosb

.ms_done:
    mov  eax, edx         ; return s
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret

; void bzero(void* s, size_t n)
; Implemented directly to avoid call overhead.
bzero:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi

    cld

    mov  edi, [ebp+8]
    mov  ecx, [ebp+12]
    xor  eax, eax

    test ecx, ecx
    jz   .bz_done

    mov  ebx, edi
    and  ebx, 3
    jz   .bz_dwords

    mov  esi, 4
    sub  esi, ebx
    cmp  esi, ecx
    jbe  .bz_align_ok
    mov  esi, ecx
.bz_align_ok:
    mov  ebx, ecx
    mov  ecx, esi
    rep  stosb
    mov  ecx, ebx
    sub  ecx, esi

.bz_dwords:
    mov  ebx, ecx
    shr  ecx, 2
    rep  stosd
    mov  ecx, ebx
    and  ecx, 3
    rep  stosb

.bz_done:
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
