import pefile, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, x86_const

PATH = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(PATH, fast_load=True)
img = open(PATH, "rb").read()
base = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
         s.SizeOfRawData, s.Name.rstrip(b"\0").decode(errors="replace")) for s in pe.sections]

def rva2off(rva):
    for va, ve, raw, rawsz, nm in secs:
        if va <= rva < ve:
            d = rva - va
            if d < rawsz:
                return raw + d
    return None

def secname(rva):
    for va, ve, raw, rawsz, nm in secs:
        if va <= rva < ve:
            return nm
    return "?"

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

def dis(rva, n):
    off = rva2off(rva)
    out = []
    for ins in md.disasm(img[off:off + n], base + rva):
        out.append(ins)
    return out

if __name__ == "__main__":
    rva = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x549C0
    n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x120
    print("section", secname(rva), "off", hex(rva2off(rva)))
    for ins in dis(rva, n):
        extra = ""
        if ins.mnemonic in ("mov", "test", "cmp", "and", "or", "movzx", "lea") and "[rip" in ins.op_str:
            disp = 0
            for op in ins.operands:
                if op.type == x86_const.X86_OP_MEM and op.mem.base == x86_const.X86_REG_RIP:
                    disp = op.mem.disp
            tgt = ins.address + ins.size + disp
            extra = "   -> rva 0x%X (%s)" % (tgt - base, secname(tgt - base))
        print("%016X %-24s %s %s%s" % (ins.address, ins.bytes.hex().upper(), ins.mnemonic, ins.op_str, extra))
