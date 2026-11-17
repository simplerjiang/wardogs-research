import pefile, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

def exports(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    out = {}
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if s.name:
                out[s.name.decode()] = s.address
    return pe, out

if __name__ == '__main__':
    path = sys.argv[1]
    pe, exp = exports(path)
    pat = sys.argv[2] if len(sys.argv) > 2 else None
    for n, a in sorted(exp.items(), key=lambda kv: kv[1]):
        if not pat or pat.lower() in n.lower():
            print("%-50s 0x%X" % (n, a))
    print("total exports:", len(exp), "imagebase 0x%X" % pe.OPTIONAL_HEADER.ImageBase)
