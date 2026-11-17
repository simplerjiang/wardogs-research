import json, os

p = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\01_调查原始归档\reference-flow-20261026-03\loader-memory-flow-snapshot.json"
data = json.load(open(p, encoding='utf-8'))
bundle = data['evidence_bundle']
recs = bundle['records']
print('records:', len(recs))

index = {}
for r in recs:
    nr = r.get('normalized_result')
    if not isinstance(nr, dict):
        continue
    proc = nr.get('procedure')
    if isinstance(proc, dict) and 'address' in proc:
        index[proc['address'].lower()] = nr

print('indexed functions:', len(index))
want = ['0x14000f080', '0x1400100d0', '0x14000aa10', '0x1400108d0', '0x140011170', '0x14000b120']
out = r"E:\wardogsv3\_analysis\ref_ctx_dump.txt"
with open(out, 'w', encoding='utf-8') as f:
    for w in want:
        nr = index.get(w)
        f.write('\n\n########## %s ##########\n' % w)
        if not nr:
            f.write('(not in snapshot)\n')
            continue
        f.write('--- PSEUDOCODE ---\n')
        f.write((nr.get('pseudocode') or '')[:8000])
        f.write('\n--- ASSEMBLY (first 200) ---\n')
        asm = nr.get('assembly') or []
        f.write('\n'.join(asm[:200]))
        f.write('\n--- NAMES ---\n')
        f.write(json.dumps(nr.get('referenced_names'))[:1500])
        f.write('\n--- STRINGS ---\n')
        f.write(json.dumps(nr.get('referenced_strings'))[:1500])
print('wrote', out, os.path.getsize(out))
