# opcodes.py <exe>: ReplayChunk's packet cases (opcode = table index + 1): each case's ctx vtable calls, other calls,
# and the packet size it leaves in eax (the tail at 0x140ece51f adds it to the read offset)
import sys, struct, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
def rd(va, n): return pe.get_data(va - base, n)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = False
TABLE, TAIL, END = 0x140ece54c, 0x140ece51f, 0x140ece51c
cases = {}
for i in range(0x50):
    off = struct.unpack('<i', rd(TABLE + 4 * i, 4))[0]
    cases.setdefault(base + off, []).append(i + 1)
starts = sorted(cases)
for k, st in enumerate(starts):
    stop = starts[k + 1] if k + 1 < len(starts) else TAIL
    lines = []
    for ins in md.disasm(rd(st, max(16, min(stop - st, 0x400))), st):
        if ins.address >= stop: break
        s = f"{ins.mnemonic} {ins.op_str}"
        if ins.mnemonic == 'call' or ('eax' in ins.op_str and ins.mnemonic == 'mov' and ins.op_str.startswith('eax, 0x')) or ins.mnemonic == 'jmp':
            lines.append(s)
    ops = ",".join(f"{o:#x}" for o in cases[st])
    print(f"op {ops} @ {st:x} ({stop-st:#x} bytes): " + " | ".join(lines))
