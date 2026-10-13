"""sym.py <module.dll> <rva> [...]: the function (and offset) at each RVA of a module of ours, from its PDB beside it
(the Windows Kits dbghelp). For the crash log's "DINPUT8.dll+0x21818" frames:

    python tools/re/sym.py build/dinput8.dll 0x21818 0x207d3
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

DBGHELP = r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll'
dbg = ctypes.WinDLL(DBGHELP)


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', wt.ULONG),
                ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', wt.ULONG), ('Scope', wt.ULONG), ('Tag', wt.ULONG),
                ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG), ('Name', ctypes.c_char * 512)]


class LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD), ('FileName', ctypes.c_char_p),
                ('Address', ctypes.c_uint64)]


def main():
    path = os.path.abspath(sys.argv[1])
    h = wt.HANDLE(0x1234)
    dbg.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES
    if not dbg.SymInitialize(h, os.path.dirname(path).encode(), False):
        raise SystemExit('SymInitialize failed')
    dbg.SymLoadModuleEx.restype = ctypes.c_uint64
    dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
    base = dbg.SymLoadModuleEx(h, None, path.encode(), None, 0x10000000, 0, None, 0)
    if not base:
        raise SystemExit('SymLoadModuleEx failed')
    for a in sys.argv[2:]:
        addr = base + int(a, 16)
        si = SYMBOL_INFO()
        si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512 + 1
        si.MaxNameLen = 511
        disp = ctypes.c_uint64(0)
        ok = dbg.SymFromAddr(h, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si))
        line = LINE64()
        line.SizeOfStruct = ctypes.sizeof(LINE64)
        d32 = wt.DWORD(0)
        lok = dbg.SymGetLineFromAddr64(h, ctypes.c_uint64(addr), ctypes.byref(d32), ctypes.byref(line))
        where = f'{line.FileName.decode(errors="replace")}:{line.LineNumber}' if lok else ''
        print(f'{a}: {si.Name.decode() if ok else "?"}+{disp.value:#x} {where}')


if __name__ == '__main__':
    main()
