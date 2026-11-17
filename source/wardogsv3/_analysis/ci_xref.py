import pefile, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, x86_const

PATH = r"C:\Windows\System32\ci.dll"
pe = pefile.PE(PATH, fast_load=True)
img = open(PATH, "rb").read()
BASE = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
         s.SizeOfRawData, s.Name.rstrip(b"\0").decode(errors="replace")) for s in pe.sections]

def secname(rva):
    for va, ve, raw, rawsz, nm in secs:
        if va <= rva < ve:
            return nm
    return "?"

def find_e8(target_rva):
    hits = []
    for va, ve, raw, rawsz, nm in secs:
        if nm not in (".text", "PAGE", "PAGEKD"):
            continue
        end = min(ve, va + rawsz)
        for rva in range(va, end - 5):
            if img[raw + (rva - va)] != 0xE8:
                continue
            rel = int.from_bytes(img[raw + (rva - va) + 1: raw + (rva - va) + 5], "little", signed=True)
            if rva + 5 + rel == target_rva:
                hits.append(rva)
    return hits

if __name__ == "__main__":
    tgt = int(sys.argv[1], 16)
    print("calls to rva 0x%X:" % tgt)
    for h in find_e8(tgt):
        print("  call site rva 0x%X  (%s)" % (h, secname(h)))
