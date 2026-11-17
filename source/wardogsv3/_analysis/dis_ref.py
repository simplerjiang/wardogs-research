import pefile, capstone, sys

path = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\06_来源与分析输入\FGTRACK-stage1.analysis.pe"
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.Name.rstrip(b'\0').decode('latin1'), base + s.VirtualAddress,
         s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
print('image base 0x%X' % base)
for s in secs:
    print('  %-8s va=0x%X vsize=0x%X raw=0x%X rawsize=0x%X' % s)

def read_va(va, n):
    for name, sva, vsize, raw, rawsize in secs:
        if sva <= va < sva + max(vsize, rawsize):
            off = va - sva
            if off >= rawsize:
                return None
            return pe.get_data(raw + off, n)
    return None

def dis(va, count, limit_bytes=0x200):
    code = read_va(va, limit_bytes)
    if not code:
        print('  <no bytes at 0x%X>' % va)
        return
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    print('=== disasm 0x%X (%d bytes) ===' % (va, len(code)))
    for i, ins in enumerate(md.disasm(code, va)):
        if i >= count:
            break
        print('  0x%X: %-30s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str))

for target in [0x14000BDA0, 0x14000C520, 0x1400108D0, 0x140011170]:
    dis(target, 40)
    print()
