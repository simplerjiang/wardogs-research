import pefile, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, x86_const

PATH = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(PATH, fast_load=True)
img = open(PATH, "rb").read()
BASE = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
         s.SizeOfRawData, s.Name.rstrip(b"\0").decode(errors="replace"), s.Characteristics)
        for s in pe.sections]

code_secs = [s for s in secs if s[5] & 0x20000000]  # CNT_CODE
data_ranges = [(0x45000, 0x45000 + 0x18C8, '.data'), (0x52000, 0x53000, 'CiPolicy'), (0xEC000, 0xED000, 'PAGEDATA')]

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
hits = collections.defaultdict(list)
for va, ve, raw, rawsz, nm, ch in code_secs:
    code = img[raw: raw + rawsz]
    for ins in md.disasm(code, BASE + va):
        for i, op in enumerate(ins.operands):
            if op.type == x86_const.X86_OP_MEM and op.mem.base == x86_const.X86_REG_RIP:
                tgt = (ins.address + ins.size + op.mem.disp) - BASE
                for lo, hi, dn in data_ranges:
                    if lo <= tgt < hi:
                        hits[tgt].append((ins.address - BASE, ins.mnemonic + " " + ins.op_str, dn))

print("distinct data addresses referenced:", len(hits))
for tgt, lst in sorted(hits.items(), key=lambda kv: -len(kv[1]))[:25]:
    print("\n0x%X refs=%d" % (tgt, len(lst)))
    for a, t, dn in lst[:8]:
        print("   0x%06X  %s" % (a, t))
