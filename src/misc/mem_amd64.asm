; amd64 SysV memcpy/memset/bzero for freestanding kernel builds.

BITS 64

SECTION .text

GLOBAL memcpy
GLOBAL memset
GLOBAL bzero

; void* memcpy(void* dest, const void* src, size_t n)
;   rdi=dest, rsi=src, rdx=n
memcpy:
    mov rax, rdi
    mov rcx, rdx
    test rcx, rcx
    jz .mc_done
    cld
    rep movsb
.mc_done:
    ret

; void* memset(void* s, int c, size_t n)
;   rdi=s, rsi=c, rdx=n
memset:
    mov r8, rdi
    mov rcx, rdx
    mov al, sil
    test rcx, rcx
    jz .ms_done
    cld
    rep stosb
.ms_done:
    mov rax, r8
    ret

; void bzero(void* s, size_t n)
;   rdi=s, rsi=n
bzero:
    mov rcx, rsi
    xor eax, eax
    test rcx, rcx
    jz .bz_done
    cld
    rep stosb
.bz_done:
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits