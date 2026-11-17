import json
import struct
import sys
from pathlib import Path

root = Path(__file__).parent
image = bytearray(Path(sys.argv[1]).read_bytes())
for slot in json.loads((root / 'original/slots.json').read_text(encoding='utf-8')):
    html = (root / (slot['name'] + '.html')).read_text(encoding='utf-8')
    payload = html.encode(slot['encoding'])
    start = slot['file_offset']
    size = slot['byte_length']
    image[start:start + size] = payload + bytes(size - len(payload))

# The original launch window is 630 x 680. These three immediates
# set the new 500 x 300 dimensions and its vertical centering.
for offset, value in [(139005, 300), (139030, 500), (139091, 300)]:
    struct.pack_into('<I', image, offset, value)

for old in ['暴龙王助手   ', '暴龙王助手 / STORM']:
    new = 'Kong助手'.ljust(len(old))
    image = image.replace((old + '\0').encode('utf-16le'), (new + '\0').encode('utf-16le'))

for offset, size, text in [(0x51ef0, 122, '%s'), (0x535e0, 134, '启动失败。')]:
    payload = (text + '\0').encode('utf-16le')
    image[offset:offset + size] = payload + bytes(size - len(payload))

output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(image)
print(output)
