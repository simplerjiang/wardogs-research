import json, sys, re, collections

path = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\01_调查原始归档\reference-flow-20261026-03\loader-memory-flow-rea.json"
data = json.load(open(path, encoding='utf-8'))
print('top keys:', list(data.keys()))
recs = data.get('records', [])
print('records:', len(recs))
r0 = recs[0]
print('record keys:', list(r0.keys()))
for k, v in r0.items():
    if isinstance(v, (list, dict)):
        print('  %-28s %s len=%d' % (k, type(v).__name__, len(v)))
    else:
        print('  %-28s %r' % (k, v))
