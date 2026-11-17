import pefile, struct

path = r"C:\Windows\System32\ntoskrnl.exe"
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
size = pe.OPTIONAL_HEADER.SizeOfImage
print('ImageBase 0x%X SizeOfImage 0x%X TimeDateStamp 0x%X' % (base, size, pe.FILE_HEADER.TimeDateStamp))

exports = {}
for e in getattr(pe, 'DIRECTORY_ENTRY_EXPORT', []).symbols:
    exports[e.name.decode() if isinstance(e.name, bytes) else e.name] = e.address

for n in ('PsSetLoadImageNotifyRoutine', 'PsRemoveLoadImageNotifyRoutine', 'PspNotifyEnableMask', 'PsSetLoadImageNotifyRoutineEx'):
    print('  %-42s 0x%X' % (n, exports.get(n, 0)))

sections = []
for s in pe.sections:
    sections.append((s.Name.rstrip(b'\0').decode('latin1'), s.VirtualAddress, s.Misc_VirtualSize,
                     s.PointerToRawData, s.SizeOfRawData, s.Characteristics))

data = pe.__data__
hits = []
needle = b'\xF0\x0F\xBA\x2D'
for name, va, vsize, raw, rawsize, ch in sections:
    if not (ch & 0x20000000) or rawsize == 0:
        continue
    blob = data[raw:raw + rawsize]
    off = 0
    while True:
        i = blob.find(needle, off)
        if i < 0:
            break
        if i + 9 <= len(blob):
            disp = struct.unpack_from('<i', blob, i + 4)[0]
            site_rva = va + i
            target_rva = site_rva + 9 + disp
            bit = blob[i + 8] & 0x3F
            hits.append((site_rva, target_rva, bit, name))
        off = i + 1
print('lock bts [rip+disp32] sites: %d' % len(hits))
for site, target, bit, name in hits[:12]:
    # which section holds the target?
    sec = '?'
    for sname, sva, svsize, sraw, srawsize, sch in sections:
        span = svsize or srawsize
        if sva <= target < sva + span:
            sec = sname
            break
    set_rva = exports.get('PsSetLoadImageNotifyRoutine', 0)
    near = ''
    if set_rva and 0 <= site - set_rva < 0x400:
        near = ' <-- inside PsSetLoadImageNotifyRoutine+0x%X' % (site - set_rva)
    print('  site 0x%X -> target 0x%X (bit %d, %s)%s' % (site, target, bit, sec, near))
