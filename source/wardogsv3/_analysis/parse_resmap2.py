import subprocess, struct

def reg_hex(path):
    out = subprocess.run(['reg','query',path], capture_output=True, text=True, encoding='mbcs', errors='replace').stdout
    for line in out.splitlines():
        if '.Translated' in line:
            return bytes.fromhex(line.split('REG_RESOURCE_LIST')[1].strip())
    return None

data = reg_hex(r"HKLM\HARDWARE\RESOURCEMAP\System Resources\Physical Memory")
print('len', len(data))
for i in range(0, len(data), 4):
    print('%3d: %s' % (i, data[i:i+4].hex(' ')), end='   ')
    if (i // 4) % 4 == 3:
        print()
print()

# CM_RESOURCE_LIST
count = struct.unpack_from('<I', data, 0)[0]
iface, bus = struct.unpack_from('<II', data, 4)
ver, rev, n = struct.unpack_from('<HHI', data, 12)
print('Count=%d iface=%d bus=%d ver=%d rev=%d partial_count=%d' % (count, iface, bus, ver, rev, n))
off = 20
for i in range(n):
    if off + 16 > len(data):
        print('  truncated at', i); break
    typ, share, flags = struct.unpack_from('<BBH', data, off)
    start, length = struct.unpack_from('<QI', data, off + 4)
    off += 16
    print('  [%d] type=%d share=%d flags=0x%04X start=0x%010X len=0x%X end=0x%010X' %
          (i, typ, share, flags, start, length, start + length - 1))
