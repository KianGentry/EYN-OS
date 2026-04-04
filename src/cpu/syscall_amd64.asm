; Minimal amd64 syscall entry stub for Milestone A build bring-up.
; Full syscall ABI entry/return handling is implemented in later milestones.

bits 64

global syscall_entry

section .text
syscall_entry:
    iretq
