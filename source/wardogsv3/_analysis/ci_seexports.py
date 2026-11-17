import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, x86_const

P = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(P, fast_load=True)
img = open(P, "rb").read()
BASE = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
         s.SizeOfRawData, s.Name.rstrip(b"\0").decode(errors="replace"), s.Characteristics) for s in pe.sections]
SEEXPORTS_IAT = 0x4F1A0

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
hits = []
for va, ve, raw, rawsz, nm, ch in secs:
    if not (ch & 0x20000000):
        continue
    code = img[raw: raw + rawsz]
    ins_list = list(md.disasm(code, BASE + va))
    for i, ins in enumerate(ins_list):
        # mov rax/rcx/rdx, qword ptr [rip+X]  where X resolves to the SeExports IAT slot
        if ins.mnemonic != "mov" or len(ins.operands) != 2:
            continue
        dst, src = ins.operands
        if dst.type != x86_const.X86_OP_REG or src.type != x86_const.X86_OP_MEM:
            continue
        if src.mem.base != x86_const.X86_REG_RIP:
            continue
        tgt = (ins.address + ins.size + src.mem.disp) - BASE
        if tgt != SEEXPORTS_IAT:
            continue
        # look ahead a few instructions for a store through the loaded register
        reg = ins.reg_name(dst.reg)
        for j in range(i + 1, min(i + 6, len(ins_list))):
            nxt = ins_list[j]
            if nxt.mnemonic == "mov" and len(nxt.operands) == 2:
                d2, s2 = nxt.operands
                if d2.type == x86_const.X86_OP_MEM and d2.mem.base != 0 and \
                        nxt.reg_name(d2.mem.base) == reg:
                    hits.append((ins.address - BASE, nxt.address - BASE, d2.mem.disp,
                                 nxt.reg_name(s2.reg) if s2.type == x86_const.X86_OP_REG else "imm",
                                 nxt.op_str))
                    break
for h in hits:
    print("site=0x%06X store=0x%06X field_off=0x%X src=%s   %s" % h)
