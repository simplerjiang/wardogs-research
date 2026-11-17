import re, sys

target = 0x34EB6  # RVA of the faulting instruction
rows = []
sec = None
for line in open(r"E:\wardogsv3\fglauncher\build\WardogLauncher.map", encoding='mbcs', errors='replace'):
    if line.startswith(' '):
        parts = line.split()
        if len(parts) >= 3 and re.match(r'^[0-9A-Fa-f]{8,16}$', parts[0]):
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            rows.append((addr, ' '.join(parts[1:])))
        continue
    m = re.match(r'^ (.*?)\s+([0-9A-Fa-f]{16})\s+([0-9A-Fa-f]{6,})', line)
    if m:
        sec = m.group(1).strip()

base = None
best = None
for addr, name in rows:
    if addr <= 0x140000000:
        continue
    if best is None or (addr <= 0x140000000 + target and addr > best[0]):
        if addr <= 0x140000000 + target:
            best = (addr, name)
if best:
    print('nearest symbol below 0x%X: 0x%X %s (+0x%X)' % (0x140000000 + target, best[0], best[1], 0x140000000 + target - best[0]))
else:
    print('no symbol found')
# also print a window of symbols around
lo = 0x140000000 + target - 0x400
for addr, name in rows:
    if lo <= addr <= 0x140000000 + target + 0x100:
        print('  0x%X  %s' % (addr, name))
