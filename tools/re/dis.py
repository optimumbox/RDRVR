# dis.py <exe> <va_hex> <bytes_hex> : disassemble RDR.exe at a VA (image base 0x140000000)
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
va, n = int(sys.argv[2], 16), int(sys.argv[3], 16)
rva = va - pe.OPTIONAL_HEADER.ImageBase
data = pe.get_data(rva, n)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i in md.disasm(data, va):
    print(f"{i.address:x}: {i.mnemonic} {i.op_str}")
