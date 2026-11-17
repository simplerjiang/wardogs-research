"""Strip comments from C/C++ and Python sources, keeping line numbers stable.

C/C++: removes // and /* */ comments outside string/char/raw-string literals.
Python: removes # comments (keeps the shebang and encoding line).
Every removed comment is replaced with spaces, so line and column numbers of the
surviving code do not move.
"""
import io
import re
import sys
import tokenize

RAW_OPEN = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')


def strip_c(text: str) -> str:
    out = []
    i, n = 0, len(text)

    def blank(ch: str) -> str:
        return ch if ch in '\r\n' else ' '

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ''

        if ch == '/' and nxt == '/':
            j = i
            while j < n:
                if text[j] == '\\' and j + 1 < n and text[j + 1] in '\r\n':
                    j += 2
                    continue
                if text[j] in '\r\n':
                    break
                j += 1
            out.append(' ' * (j - i))
            i = j
            continue

        if ch == '/' and nxt == '*':
            j = i + 2
            while j < n and not (text[j] == '*' and j + 1 < n and text[j + 1] == '/'):
                j += 1
            end = min(j + 2, n)
            out.append(''.join(blank(c) for c in text[i:end]))
            i = end
            continue

        if ch == '"' or ch == "'":
            quote = ch
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                if text[j] in '\r\n':
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue

        if ch == 'R' or (ch in 'uUL8' and RAW_OPEN.match(text, i)):
            m = RAW_OPEN.match(text, i)
            if m:
                tail = ')' + m.group(1) + '"'
                start = m.end()
                close = text.find(tail, start)
                close = n if close < 0 else close + len(tail)
                out.append(text[i:close])
                i = close
                continue

        out.append(ch)
        i += 1

    return ''.join(out)


def strip_py(text: str) -> str:
    lines = text.splitlines(keepends=True)
    keep = [False] * len(lines)
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.COMMENT:
                if tok.start[0] == 1 and tok.string.startswith('#!'):
                    keep[tok.start[0] - 1] = True
                if tok.start[0] == 2 and 'coding' in tok.string:
                    keep[tok.start[0] - 1] = True
    except (tokenize.TokenError, IndentationError):
        return text
    out = []
    for idx, line in enumerate(lines):
        if not keep[idx] and line.lstrip().startswith('#'):
            out.append(re.match(r'[ \t]*', line).group(0) + line.lstrip('\r\n')[0:0] + '\n' if line.endswith('\n') else '')
            continue
        out.append(line)
    text2 = ''.join(out)
    out2 = []
    try:
        toks = list(tokenize.generate_tokens(io.StringIO(text2).readline))
    except (tokenize.TokenError, IndentationError):
        return text
    comments = [t for t in toks if t.type == tokenize.COMMENT]
    rows = text2.splitlines(keepends=True)
    for tok in reversed(comments):
        if tok.start[0] == 1 and tok.string.startswith('#!'):
            continue
        r = tok.start[0] - 1
        line = rows[r]
        rows[r] = line[:tok.start[1]] + ' ' * (tok.end[1] - tok.start[1]) + line[tok.end[1]:]
    return ''.join(rows)


def main(paths):
    total = 0
    for path in paths:
        with open(path, 'r', encoding='utf-8', newline='') as fh:
            text = fh.read()
        if path.lower().endswith('.py'):
            new = strip_py(text)
        else:
            new = strip_c(text)
        if new != text:
            with open(path, 'w', encoding='utf-8', newline='') as fh:
                fh.write(new)
            total += 1
            print('stripped', path)
    print('files changed:', total)


if __name__ == '__main__':
    main(sys.argv[1:])
