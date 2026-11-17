import pefile, struct, capstone

path = r"C:\Windows\System32\ntoskrnl.exe"
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
data = pe.__data__
base = pe.OPTIONAL_HEADER.ImageBase
exports = {e.name.decode() if isinstance(e.name, bytes) else e.name: e.address
           for e in getattr(pe, 'DIRECTORY_ENTRY_EXPORT', []).symbols}

def rva2off(rva):
    for s in pe.sections:
        span = s.Misc_VirtualSize or s.SizeOfRawData
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            d = rva - s.VirtualAddress
            if d < s.SizeOfRawData:
                return s.PointerToRawData + d
    return None

def dis(rva, n=64, count=24):
    off = rva2off(rva)
    code = data[off:off + n]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    print('--- rva 0x%X ---' % rva)
    for i, ins in enumerate(md.disasm(code, base + rva)):
        if i >= count:
            break
        print('  0x%X: %-24s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str))

for name in ('PsSetLoadImageNotifyRoutine', 'PsSetLoadImageNotifyRoutineEx', 'PsRemoveLoadImageNotifyRoutine'):
    r = exports.get(name)
    print('== %s 0x%X ==' % (name, r or 0))
    if r:
        dis(r, 0x100, 20)

print()
print('== all lock bts [rip+disp32] sites ==')
needle = b'\xF0\x0F\xBA\x2D'
for s in pe.sections:
    if not (s.Characteristics & 0x20000000) or not s.SizeOfRawData:
        continue
    blob = data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
    off = 0
    while True:
        i = blob.find(needle, off)
        if i < 0:
            break
        if i + 9 <= len(blob):
            disp = struct.unpack_from('<i', blob, i + 4)[0]
            site = s.VirtualAddress + i
            target = site + 9 + disp
            bit = blob[i + 8] & 0x3F
            mark = ''
            for n2 in ('PsSetLoadImageNotifyRoutine', 'PsSetLoadImageNotifyRoutineEx'):
                r2 = exports.get(n2)
                if r2 and 0 <= site - r2 < 0x300:
                    mark = '  <== inside %s+0x%X' % (n2, site - r2)
            print('  site 0x%X target 0x%X bit %2d%s' % (site, target, bit, mark))
        off = i + 1
