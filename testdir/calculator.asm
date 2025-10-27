; Simple interactive calculator: reads "A+B" (single-digit or multi-digit), prints result
; Uses syscalls: read(0, buf, len), write(1, buf, len), exit(code)
; Comments kept brief per style request

section .data
    prompt db "Enter expr (e.g., 12+34): ", 0
    outbuf  resb 48
    inbuf   resb 48

section .text
    global _start

_start:
    ; write prompt
    mov eax, 4        ; sys_write
    mov ebx, 1        ; fd=1
    lea ecx, [prompt]
    mov edx, 27
    int 0x80

    ; read line
    mov eax, 3        ; sys_read
    mov ebx, 0        ; fd=0
    lea ecx, [inbuf]
    mov edx, 48
    int 0x80          ; eax = nbytes

    ; parse: <int><op><int> ; skip spaces
    lea esi, [inbuf]
    xor eax, eax      ; val1
    xor ebx, ebx      ; val2
    xor edi, edi      ; 0 means parsing first, 1 means second

.skip_ws1:
    mov dl, byte [esi]
    cmp dl, ' '
    jne .parse_int1
    inc esi
    jmp .skip_ws1

.parse_int1:
    mov dl, byte [esi]
    cmp dl, '0'
    jb .check_op
    cmp dl, '9'
    ja .check_op
    ; eax = eax*10 + (dl-'0')
    imul eax, eax, 10
    movzx edx, dl
    sub edx, '0'
    add eax, edx
    inc esi
    jmp .parse_int1

.check_op:
    mov dl, byte [esi]
    cmp dl, '+'
    je .op_add
    cmp dl, '-'
    je .op_sub
    cmp dl, '*'
    je .op_mul
    cmp dl, '/'
    je .op_div
    jmp .done        ; unknown op → done

.op_add:
    mov edi, 1
    inc esi
    jmp .parse_int2
.op_sub:
    mov edi, 2
    inc esi
    jmp .parse_int2
.op_mul:
    mov edi, 3
    inc esi
    jmp .parse_int2
.op_div:
    mov edi, 4
    inc esi

.parse_int2:
.skip_ws2:
    mov dl, byte [esi]
    cmp dl, ' '
    jne .pi2_loop
    inc esi
    jmp .skip_ws2
.pi2_loop:
    mov dl, byte [esi]
    cmp dl, '0'
    jb .compute
    cmp dl, '9'
    ja .compute
    imul ebx, ebx, 10
    movzx edx, dl
    sub edx, '0'
    add ebx, edx
    inc esi
    jmp .pi2_loop

.compute:
    ; perform based on edi
    cmp edi, 1
    je .do_add
    cmp edi, 2
    je .do_sub
    cmp edi, 3
    je .do_mul
    cmp edi, 4
    je .do_div
    jmp .done

.do_add:
    add eax, ebx
    jmp .print
.do_sub:
    sub eax, ebx
    jmp .print
.do_mul:
    imul eax, ebx
    jmp .print
.do_div:
    cmp ebx, 0
    je .print
    xor edx, edx
    div ebx           ; eax=quotient

.print:
    ; convert eax to string into outbuf
    lea edi, [outbuf]
    push eax
    mov ecx, 0        ; sign flag
    cmp eax, 0
    jge .conv_start
    neg eax
    mov ecx, 1
.conv_start:
    xor ebx, ebx
    mov esi, edi      ; save start
    ; handle zero
    cmp eax, 0
    jne .conv_loop
    mov byte [edi], '0'
    inc edi
    jmp .conv_done
.conv_loop:
    xor edx, edx
    mov ebx, 10
    div ebx           ; eax/=10, edx=remainder
    add dl, '0'
    mov byte [edi], dl
    inc edi
    cmp eax, 0
    jne .conv_loop
.conv_done:
    ; add sign if needed
    cmp ecx, 1
    jne .rev
    mov byte [edi], '-'
    inc edi
.rev:
    ; reverse digits in-place
    mov ebx, esi
    dec edi
.rev_loop:
    cmp ebx, edi
    jge .rev_done
    mov al, [ebx]
    mov dl, [edi]
    mov [ebx], dl
    mov [edi], al
    inc ebx
    dec edi
    jmp .rev_loop
.rev_done:
    inc edi
    mov byte [edi], 10  ; newline
    inc edi
    mov byte [edi], 0
    ; write out
    mov eax, 1
    mov ebx, 1
    mov ecx, esi
    mov edx, 48
    int 0x80
    pop eax

.done:
    mov eax, 1
    xor ebx, ebx
    int 0x80
