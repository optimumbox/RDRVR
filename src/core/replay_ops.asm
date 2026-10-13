; [Stereo] ViSkipPackets (replay_ops.cpp): the playback loop's dispatch (ReplayDispatch 0x140ecdd8a: dec eax; cmp eax,
; 4Fh, entered with eax = the opcode, rdi = the packet, r14 = the read offset's address) through rdrvr_ro_dispatch,
; which counts the packet and returns 0 (run it) or its size (skip it: the common tail adds eax to the read offset).
; Only rax is live among the volatile registers here (each case loads what it uses from rdi/rsi); the loop's rsp is the
; function body's, 16-byte aligned with home space (playback_head.asm makes the same call).

EXTERN rdrvr_ro_dispatch:PROC
EXTERN rdrvr_ro_back:QWORD
EXTERN rdrvr_ro_tail:QWORD

.code
rdrvr_ro_stub PROC
    mov ecx, eax
    mov rdx, rdi
    mov r8, r14
    call rdrvr_ro_dispatch
    test eax, eax
    jnz skip
    mov eax, dword ptr [rdi]
    dec eax
    cmp eax, 4Fh
    jmp qword ptr [rdrvr_ro_back]
skip:
    jmp qword ptr [rdrvr_ro_tail]
rdrvr_ro_stub ENDP
END
