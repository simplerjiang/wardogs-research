import json, os, sys, glob

BASE = r"E:\CodexData\exports\FGTRACK-完整资料-20261026-01"
OUT = r"E:\wardogsv3\_analysis\funcs"
os.makedirs(OUT, exist_ok=True)

files = glob.glob(os.path.join(BASE, "**", "*.json"), recursive=True)
want = {
    "0x1400108d0", "0x1400127e0", "0x14000f080", "0x1400100d0",
    "0x14000af30", "0x14000af80", "0x14000a7d0", "0x14000aa10",
    "0x140007aa0", "0x14000ed30", "0x14000ec90", "0x14001a58c",
    "0x140014fe0", "0x140010a00",
}
found = {}
for f in files:
    if os.path.getsize(f) > 60 * 1024 * 1024:
        continue
    try:
        with open(f, "r", encoding="utf-8", errors="replace") as fh:
            j = json.load(fh)
    except Exception:
        continue
    recs = j.get("records") if isinstance(j, dict) else None
    if not recs:
        continue
    for r in recs:
        if not isinstance(r, dict):
            continue
        p = (r.get("parameters") or {}).get("procedure")
        if p in want:
            nr = r.get("normalized_result") or {}
            pc = nr.get("pseudocode") or ""
            if p in found and len(found[p][1]) >= len(pc):
                continue
            found[p] = (os.path.basename(f), pc, r.get("evidence_id"))

for p, (src, pc, ev) in sorted(found.items()):
    name = p.replace("0x", "")
    with open(os.path.join(OUT, name + ".c"), "w", encoding="utf-8") as fh:
        fh.write("// procedure %s  source=%s  evidence=%s\n\n" % (p, src, ev))
        fh.write(pc)
    print(name, len(pc), src, ev)
print("missing:", sorted(want - set(found)))
