; Copyright (C) 2026 Mai Ooizumi
; SPDX-License-Identifier: AGPL-3.0-or-later

option casemap:none

.code

PUBLIC ThirdPersonAbiTarget
PUBLIC ThirdPersonAbiSite
PUBLIC ThirdPersonAbiCallWithSentinels

; Windows x64: RCX=pawn, RDX=output buffer, RAX=output buffer.
ThirdPersonAbiTarget PROC FRAME
    push rdi
    .pushreg rdi
    push rbx
    .pushreg rbx
    sub rsp, 28h
    .allocstack 28h
    .endprolog

    mov rdi, rcx
    mov rbx, rdx
    mov rcx, qword ptr [rdi]

ThirdPersonAbiSite LABEL BYTE
    test rcx, rcx
    jz short NativeEyePath
    movq xmm0, qword ptr [rcx]
    movss xmm1, dword ptr [rcx + 8]
    jmp short StoreEyeResult

NativeEyePath:
    movq xmm0, qword ptr [rdi + 8]
    movss xmm1, dword ptr [rdi + 16]

StoreEyeResult:
    movq qword ptr [rbx], xmm0
    movss dword ptr [rbx + 8], xmm1
    mov rax, rbx
    add rsp, 28h
    pop rbx
    pop rdi
    ret
ThirdPersonAbiTarget ENDP

; Seed and check Windows nonvolatile RDI/RBX around a real call to the target.
; The wrapper also checks the target's required RAX=output-buffer return.
ThirdPersonAbiCallWithSentinels PROC FRAME
    push rdi
    .pushreg rdi
    push rbx
    .pushreg rbx
    sub rsp, 38h
    .allocstack 38h
    .endprolog

    mov qword ptr [rsp + 20h], rcx
    mov qword ptr [rsp + 28h], rdx
    mov rdi, 01122334455667788h
    mov rbx, 02233445566778899h
    mov rcx, qword ptr [rsp + 20h]
    mov rdx, qword ptr [rsp + 28h]
    call ThirdPersonAbiTarget

    cmp rax, qword ptr [rsp + 28h]
    jne SentinelCheckFailed
    mov r10, 01122334455667788h
    cmp rdi, r10
    jne SentinelCheckFailed
    mov r10, 02233445566778899h
    cmp rbx, r10
    jne SentinelCheckFailed
    mov eax, 1
    jmp SentinelCheckDone

SentinelCheckFailed:
    xor eax, eax

SentinelCheckDone:
    add rsp, 38h
    pop rbx
    pop rdi
    ret
ThirdPersonAbiCallWithSentinels ENDP

END
