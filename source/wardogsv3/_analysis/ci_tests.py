import pefile, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, x86_const

PATH = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(PATH, fast_load=True)
img = open(PATH, "rb").read()
BASE = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
         s.SizeOfRawData, s.Name.rstrip(b"\0").decode(errors="replace"), s.Characteristics)
        for s in pe.sections]
code_secs = [s for s in secs if s[5] & 0x20000000]
data_ranges = [(0x45000, 0x45000 + 0x18C8, '.data'), (0x52000, 0x53000, 'CiPolicy'), (0xEC000, 0xED000, 'PAGEDATA')]

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
tests = collections.defaultdict(list)
for va, ve, raw, rawsz, nm, ch in code_secs:
    code = img[raw: raw + rawsz]
    for ins in md.disasm(code, BASE + va):
        if ins.mnemonic not in ('test', 'cmp', 'and', 'or', 'bts', 'btr', 'mov', 'bt'):
            continue
        ops = ins.operands
        if len(ops) < 2:
            continue
        mem = None; imm = None
        for op in ops:
            if op.type == x86_const.X86_OP_MEM and op.mem.base == x86_const.X86_REG_RIP:
                mem = (ins.address + ins.size + op.mem.disp) - BASE
            if op.type == x86_const.X86_OP_IMM:
                imm = op.imm
        if mem is None:
            continue
        for lo, hi, dn in data_ranges:
            if lo <= mem < hi:
                tests[mem].append((ins.address - BASE, ins.mnemonic + " " + ins.op_str, imm, dn))

for tgt, lst in sorted(tests.items(), key=lambda kv: -len(kv[1])):
    imms = sorted({l[2] for l in lst if l[2] is not None})
    print("0x%X(%s) n=%d imms=%s" % (tgt, lst[0][3], len(lst), [hex(i) for i in imms][:8]))
