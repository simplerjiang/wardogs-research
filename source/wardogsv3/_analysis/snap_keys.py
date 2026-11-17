import json

p = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01\01_调查原始归档\reference-flow-20261026-03\loader-memory-flow-snapshot.json"
data = json.load(open(p, encoding='utf-8'))
print('top keys:', list(data.keys()))
for k, v in data.items():
    if isinstance(v, dict):
        print('  %s: dict keys=%s' % (k, list(v)[:12]))
    elif isinstance(v, list):
        print('  %s: list len=%d first_type=%s' % (k, len(v), type(v[0]).__name__ if v else None))
    else:
        print('  %s: %r' % (k, v))
