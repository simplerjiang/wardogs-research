"""Assert that no source file contains comments any more."""
import glob
import re
import sys

C_STYLE = ('src/*.cpp', 'src/*.h', 'src/*.c', 'driver/*.c')
PY_STYLE = ('tools/*.py',)

bad = []
for pattern in C_STYLE:
    for path in glob.glob(pattern):
        text = open(path, encoding='utf-8').read()
        for m in re.finditer(r'/\*|//', text):
            line = text.count('\n', 0, m.start()) + 1
            bad.append((path, line, text.split('\n')[line - 1].strip()[:60]))

for pattern in PY_STYLE:
    for path in glob.glob(pattern):
        if path.endswith('strip_comments.py'):
            continue
        text = open(path, encoding='utf-8').read()
        for i, line in enumerate(text.split('\n'), 1):
            if re.match(r'\s*#', line):
                bad.append((path, i, line.strip()[:60]))

if bad:
    for item in bad:
        print('COMMENT', item)
    sys.exit(1)
print('no comments in sources')
