import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PE = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\01_调查原始归档\FGTRACK-business.analysis.pe"
pe = pefile.PE(PE, fast_load=True)
img = open(PE, "rb").read()
base = pe.OPTIONAL_HEADER.ImageBase

secs = []
for s in pe.sections:
    secs.append((s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
                 s.PointerToRawData, s.SizeOfRawData, s.Name.rstrip(b"\x00").decode(errors="replace")))

def rva2off(rva):
    for va, vend, raw, rawsz, name in secs:
        if va <= rva < vend:
            d = rva - va
            if d < rawsz:
                return raw + d
    return None

def sec_of(rva):
    for va, vend, raw, rawsz, name in secs:
        if va <= rva < vend:
            return name
    return "?"

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def dis(rva, length):
    off = rva2off(rva)
    code = img[off:off + length]
    addr = base + rva
    out = []
    for ins in md.disasm(code, addr):
        out.append("%016X  %-24s %s %s" % (ins.address, ins.bytes.hex().upper(), ins.mnemonic, ins.op_str))
    return out

if __name__ == "__main__":
    rva = int(sys.argv[1], 0) - base if sys.argv[1].lower().startswith("0x14") else int(sys.argv[1], 0)
    if rva > 0x1000000:
        rva -= base
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x200
    print("section:", sec_of(rva), "fileoff:", hex(rva2off(rva) or 0))
    for line in dis(rva, n):
        print(line)
