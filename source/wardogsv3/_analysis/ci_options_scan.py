import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, x86_const

PATH = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(PATH, fast_load=True)
img = open(PATH, "rb").read()
BASE = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
         s.SizeOfRawData, s.Name.rstrip(b"\0").decode(errors="replace"),
         s.Characteristics) for s in pe.sections]
for va, ve, raw, rawsz, nm, ch in secs:
    print("%-10s va=0x%08X vsize=0x%X rawsz=0x%X chars=0x%08X writable=%s" %
          (nm, va, ve - va, rawsz, ch, bool(ch & 0x80000000)))

def rva2off(rva):
    for va, ve, raw, rawsz, nm, ch in secs:
        if va <= rva < ve and rva - va < rawsz:
            return raw + (rva - va)
    return None

VALUE = 0x0000C001
print("\n=== DWORD == 0x%X in writable sections (file image) ===" % VALUE)
cands = []
for va, ve, raw, rawsz, nm, ch in secs:
    if not (ch & 0x80000000):
        continue
    end = min(ve, va + rawsz)
    for rva in range(va, end - 4):
        off = rva2off(rva)
        if off is None:
            continue
        v = int.from_bytes(img[off:off + 4], "little")
        if v == VALUE:
            cands.append(rva)
print("count:", len(cands), ["0x%X" % c for c in cands])

print("\n=== RIP-relative refs to candidates ===")
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
for va, ve, raw, rawsz, nm, ch in secs:
    if ch & 0x80000000 or nm not in ('.text', 'PAGE'):
        continue
    end = min(ve, va + rawsz)
    code = img[raw: raw + rawsz]
    for ins in md.disasm(code, BASE + va):
        for op in ins.operands:
            if op.type == x86_const.X86_OP_MEM and op.mem.base == x86_const.X86_REG_RIP:
                tgt = (ins.address + ins.size + op.mem.disp) - BASE
                if tgt in cands:
                    print("  0x%X: %-30s -> 0x%X  (%s)" % (ins.address - BASE, ins.mnemonic + " " + ins.op_str, tgt, nm))
