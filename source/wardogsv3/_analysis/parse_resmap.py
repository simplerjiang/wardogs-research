import subprocess, struct, json, re

def reg_hex(path):
    out = subprocess.run(['reg','query',path], capture_output=True, text=True, encoding='mbcs', errors='replace').stdout
    for line in out.splitlines():
        if '.Translated' in line:
            return bytes.fromhex(line.split('REG_RESOURCE_LIST')[1].strip())
    return None

data = reg_hex(r"HKLM\HARDWARE\RESOURCEMAP\System Resources\Physical Memory")
print('raw bytes:', len(data))
count = struct.unpack_from('<I', data, 0)[0]
iface = struct.unpack_from('<I', data, 4)[0]
bus   = struct.unpack_from('<I', data, 8)[0]
print('Count=%d Interface=%d Bus=%d' % (count, iface, bus))
off = 12
ver, rev, n = struct.unpack_from('<HHI', data, off); off += 8
print('PartialResourceList version=%d revision=%d count=%d' % (ver, rev, n))
ranges = []
for i in range(n):
    typ, share, flags = struct.unpack_from('<BBH', data, off)
    start, length = struct.unpack_from('<QI', data, off + 4)
    off += 16
    print('  desc[%d] type=%d share=%d flags=0x%04x start=0x%X length=0x%X end=0x%X' %
          (i, typ, share, flags, start, length, start + length - 1))
    if typ == 3:
        ranges.append((start, length))

total = sum(l for _, l in ranges)
print('RAM total = %.2f GiB in %d ranges' % (total / 2**30, len(ranges)))
for s, l in sorted(ranges):
    print('  RAM 0x%010X - 0x%010X  (%.2f GiB)' % (s, s + l - 1, l / 2**30))
