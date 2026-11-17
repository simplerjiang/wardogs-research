import pefile, capstone, struct

path = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\02_软件原样本\驱动及对照\linklook.original.sys"
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
print('linklook.original.sys  SizeOfImage=0x%X  entry=0x%X  checksum=0x%X  TimeDateStamp=0x%X' % (
    pe.OPTIONAL_HEADER.SizeOfImage, entry, pe.OPTIONAL_HEADER.CheckSum, pe.FILE_HEADER.TimeDateStamp))

def rva2off(rva):
    for s in pe.sections:
        span = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            d = rva - s.VirtualAddress
            if d < s.SizeOfRawData:
                return s.PointerToRawData + d
    return None

data = pe.__data__

# strings: wide + ascii
import re
wide = set(m.decode('utf-16-le', 'ignore') for m in re.findall(rb'(?:[\x20-\x7e]\x00){4,}', data))
ascii_ = set(m.decode('latin1') for m in re.findall(rb'[\x20-\x7e]{5,}', data))
print('\n-- candidate device / path strings --')
for s in sorted(wide | ascii_):
    t = s.strip()
    if any(k in t.lower() for k in ('device', 'dosdevices', '\\\\.\\', 'linklook', 'symlink', 'physical')):
        print('   %r' % t[:80])

print('\n-- imports --')
try:
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    for dll in pe.DIRECTORY_ENTRY_IMPORT:
        names = [imp.name.decode() if imp.name else 'ord%d' % imp.ordinal for imp in dll.imports]
        print('   %s: %s' % (dll.dll.decode(), ', '.join(names)))
except Exception as e:
    print('   (import parse failed: %s)' % e)

print('\n-- DriverEntry disassembly --')
off = rva2off(pe.OPTIONAL_HEADER.AddressOfEntryPoint)
code = data[off:off + 0x300]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i, ins in enumerate(md.disasm(code, entry)):
    if i >= 70:
        break
    print('  0x%X: %-22s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str))
