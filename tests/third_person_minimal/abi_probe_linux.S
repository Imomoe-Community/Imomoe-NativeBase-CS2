/* Copyright (C) 2026 Mai Ooizumi */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

    .section .text
    .p2align 4
    .globl ThirdPersonAbiTarget
    .type ThirdPersonAbiTarget, @function
ThirdPersonAbiTarget:
    .cfi_startproc
    pushq %rbx
    .cfi_def_cfa_offset 16
    .cfi_offset %rbx, -16
    movq %rdi, %rbx
    movq (%rbx), %rdi

    .globl ThirdPersonAbiSite
ThirdPersonAbiSite:
    testq %rdi, %rdi
    jz .L_native_eye_path
    movq (%rdi), %xmm0
    movss 8(%rdi), %xmm1
    jmp .L_return_eye_result

.L_native_eye_path:
    movq 8(%rbx), %xmm0
    movss 16(%rbx), %xmm1

.L_return_eye_result:
    popq %rbx
    .cfi_def_cfa_offset 8
    .cfi_restore %rbx
    ret
    .cfi_endproc
    .size ThirdPersonAbiTarget, .-ThirdPersonAbiTarget

    .globl ThirdPersonAbiCallWithSentinels
    .type ThirdPersonAbiCallWithSentinels, @function
ThirdPersonAbiCallWithSentinels:
    .cfi_startproc
    pushq %rbx
    .cfi_def_cfa_offset 16
    .cfi_offset %rbx, -16
    subq $16, %rsp
    .cfi_def_cfa_offset 32
    movq %rdi, (%rsp)
    movq %rsi, 8(%rsp)
    movabsq $0x2233445566778899, %rbx
    movq (%rsp), %rdi
    call ThirdPersonAbiTarget

    movq 8(%rsp), %rax
    movq %xmm0, (%rax)
    movss %xmm1, 8(%rax)
    movabsq $0x2233445566778899, %rax
    cmpq %rax, %rbx
    sete %al
    movzbl %al, %eax

    addq $16, %rsp
    .cfi_def_cfa_offset 16
    popq %rbx
    .cfi_def_cfa_offset 8
    .cfi_restore %rbx
    ret
    .cfi_endproc
    .size ThirdPersonAbiCallWithSentinels, .-ThirdPersonAbiCallWithSentinels

    .section .note.GNU-stack,"",@progbits
