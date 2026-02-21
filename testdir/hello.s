  .file 1 "hello_c_uelf.c"
  .file 2 "/include/stdio.h"
  .file 3 "/include/stddef.h"
  .file 4 "/include/stdarg.h"
  .file 5 "/include/unistd.h"
  .local .L..3
  .data
  .type .L..3, @object
  .size .L..3, 18
  .align 1
.L..3:
  .byte 80
  .byte 114
  .byte 101
  .byte 115
  .byte 115
  .byte 32
  .byte 113
  .byte 32
  .byte 116
  .byte 111
  .byte 32
  .byte 113
  .byte 117
  .byte 105
  .byte 116
  .byte 46
  .byte 10
  .byte 0
  .local .L..2
  .data
  .type .L..2, @object
  .size .L..2, 23
  .align 1
.L..2:
  .byte 72
  .byte 101
  .byte 108
  .byte 108
  .byte 111
  .byte 32
  .byte 102
  .byte 114
  .byte 111
  .byte 109
  .byte 32
  .byte 67
  .byte 32
  .byte 40
  .byte 46
  .byte 117
  .byte 101
  .byte 108
  .byte 102
  .byte 41
  .byte 33
  .byte 10
  .byte 0
  .local .L..1
  .data
  .type .L..1, @object
  .size .L..1, 5
  .align 1
.L..1:
  .byte 109
  .byte 97
  .byte 105
  .byte 110
  .byte 0
  .local .L..0
  .data
  .type .L..0, @object
  .size .L..0, 5
  .align 1
.L..0:
  .byte 109
  .byte 97
  .byte 105
  .byte 110
  .byte 0
  .globl main
  .text
  .type main, @function
main:
  push %ebp
  mov %esp, %ebp
  sub $16, %esp
  mov $.L..2, %eax
  push %eax
  call printf
  add $4, %esp
  mov $.L..3, %eax
  push %eax
  call printf
  add $4, %esp
.L.begin.1:
  mov $4, %ecx
  lea -4(%ebp), %edi
  mov $0, %al
  rep stosb
  lea -4(%ebp), %eax
  push %eax
  call getkey
  pop %ecx
  mov %eax, (%ecx)
  mov $113, %eax
  push %eax
  lea -4(%ebp), %eax
  mov (%eax), %eax
  pop %ecx
  cmp %ecx, %eax
  sete %al
  movzbl %al, %eax
  cmp $0, %eax
  je  .L.else.2
  jmp .L..4
  jmp .L.end.2
.L.else.2:
.L.end.2:
.L..5:
  jmp .L.begin.1
.L..4:
  mov $0, %eax
  push %eax
  call _exit
  add $4, %esp
  mov $0, %eax
.L.return.main:
  mov %ebp, %esp
  pop %ebp
  ret
