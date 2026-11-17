import json, re, collections

path = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\01_调查原始归档\reference-flow-20261026-03\loader-memory-flow-rea.json"
data = json.load(open(path, encoding='utf-8'))
recs = data['records']

for i, r in enumerate(recs):
    nr = r.get('normalized_result') or {}
    subj = r.get('subject') or {}
    print('--- record %d ---' % i)
    print('  subject :', {k: subj[k] for k in list(subj)[:6]})
    print('  params  :', json.dumps(r.get('parameters'))[:200])
    print('  result keys:', list(nr.keys()))
    rr = r.get('raw_result') or {}
    print('  raw keys   :', list(rr.keys()))
    for k, v in nr.items():
        s = json.dumps(v)
        print('    %-24s %s' % (k, s[:160]))
