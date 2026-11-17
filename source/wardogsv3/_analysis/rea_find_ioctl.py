import json, re, sys

base = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\01_调查原始归档"
paths = [base + r"\PGR-FIRST-LOAD-snapshot.json", base + r"\FUNCTION-REVIEW-snapshot.json"]

def walk(o, out):
    if isinstance(o, dict):
        for k, v in o.items():
            walk(v, out)
    elif isinstance(o, list):
        for v in o:
            walk(v, out)
    elif isinstance(o, str):
        out.append(o)

p = paths[0]
print('loading', p)
data = json.load(open(p, encoding='utf-8'))
print('top keys:', list(data.keys()))

funcs = {}
# find procedure records anywhere
def scan(o):
    if isinstance(o, dict):
        proc = o.get('procedure')
        if isinstance(proc, dict) and 'address' in proc and 'pseudocode' in o:
            funcs[proc['address']] = o
        for v in o.values():
            scan(v)
    elif isinstance(o, list):
        for v in o:
            scan(v)

scan(data)
print('functions with pseudocode:', len(funcs))
hits = []
for addr, rec in funcs.items():
    ps = rec.get('pseudocode') or ''
    if '0x8010204' in ps or '80102040' in ps or '0x22218c' in ps or '22218c' in ps:
        hits.append((addr, ps))
print('IOCTL users:', [h[0] for h in hits])

for addr, ps in hits:
    print('=== %s ===' % addr)
    for line in ps.splitlines():
        if any(t in line for t in ('0x80102', '0x22218', '0x28', 'local_', 'param_')):
            pass
    print(ps[:1500])
    print()
