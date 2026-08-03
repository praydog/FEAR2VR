"""Do the printf format strings match their argument lists?

WHY THIS EXISTS
---------------
The host's periodic log line broke three times in one session, each time silently:

  1. arguments appended where they read well in the source rather than where the format wanted
     them, so every field after the insertion point showed a different variable;
  2. a double passed to %llu, printing 4607992913809357429 -- the bit pattern of 1.18 -- as a count;
  3. a format field left behind when its argument was deleted, which shifted everything after it and
     handed a float to %s. That one CRASHED THE HOST inside strnlen.

MSVC does not warn at this project's warning level, and a 36-argument call cannot be checked by
eye -- I tried, and signed off on it twice while it was wrong.

The real fix was to split that call into four short ones. This is the guard that says they stay
short and stay balanced, and it catches the same mistake anywhere else in the tree.

WHAT IT DOES NOT DO
-------------------
It checks the COUNT, not the types: %llu against an int is still undefined and still invisible here.
Counting is what actually went wrong three times out of three.
"""

import pathlib
import re
import sys

# %s is deliberately included -- it is the specifier that turns a miscount into a crash rather than
# a wrong number, because it dereferences whatever it is handed.
SPEC = re.compile(
    r"%[-+ 0#]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|z|j|t|L|I64|I32|I)?[diuoxXeEfgGaAcspn%]")


def split_top_level(text):
    """Comma-separated pieces at depth zero, skipping string and char literals."""
    out, depth, start, i, n = [], 0, 0, 0, len(text)
    while i < n:
        c = text[i]
        if c == "\\":
            i += 2
            continue
        if c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(text[start:i])
            start = i + 1
        i += 1
    out.append(text[start:])
    return out


def strip_comments(text):
    """Blank out comments, preserving line structure and never touching string literals.

    A comment between two halves of a concatenated format is legal and common in this tree -- and a
    comma or a quote inside one derailed the scan, which is how this checker reported two calls as
    broken when they were fine.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "\\":
            out.append(text[i:i + 2])
            i += 2
            continue
        if c in '"\'':
            quote = c
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def literals(text):
    """Concatenated contents of adjacent string literals, which is how C spells one format."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text))


def check(path):
    src = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    problems = []
    for m in re.finditer(r"\b(?:std::)?(?:printf|fprintf|sprintf|snprintf)\s*\(", src):
        i, depth, j = m.end(), 1, m.end()
        while depth and j < len(src):
            c = src[j]
            if c == "\\":
                j += 2
                continue
            if c in '"\'':
                quote = c
                j += 1
                while j < len(src) and src[j] != quote:
                    j += 2 if src[j] == "\\" else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            j += 1
        parts = split_top_level(src[i:j - 1])
        # fprintf/snprintf take a sink before the format; the format is the first piece with a literal
        fmt_at = next((k for k, p in enumerate(parts) if '"' in p), None)
        if fmt_at is None:
            continue
        fmt = literals(parts[fmt_at])
        # A format continued by a PRI* macro is not readable as text -- the literal stops and the
        # width lives in a macro this cannot expand. Skipped rather than guessed at.
        if re.search(r"\bPRI[a-zA-Z]+\b", parts[fmt_at]):
            continue
        specs = [s for s in SPEC.findall(fmt) if s != "%%"]
        # Each '*' takes an argument of its own, so "%.*f" is two.
        specs_n = len(specs) + sum(s.count("*") for s in specs)
        args = [p for p in parts[fmt_at + 1:] if p.strip()]
        if specs_n != len(args):
            line = src[:m.start()].count("\n") + 1
            problems.append((line, fmt.replace("\\n", "")[:52], specs_n, len(args)))
    return problems


def main():
    roots = [pathlib.Path(a) for a in sys.argv[1:]] or [
        pathlib.Path("tools/xr64/main.cpp"),
        pathlib.Path("src"),
        pathlib.Path("test"),
    ]
    files = []
    for r in roots:
        files.extend([r] if r.is_file() else sorted(r.rglob("*.cpp")))

    bad = 0
    for f in files:
        for line, fmt, ns, na in check(f):
            bad += 1
            print("%s:%d  %d specifiers / %d arguments  |  %s" % (f, line, ns, na, fmt))
    if bad:
        print("\n%d format/argument mismatch(es)." % bad)
        return 1
    print("format strings balanced across %d file(s)" % len(files))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
