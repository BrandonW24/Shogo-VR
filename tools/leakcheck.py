#!/usr/bin/env python3
"""Checks that no code from Monolith's Shogo source has been copied into this
repository.  Usage: python tools/leakcheck.py <path to Shogo source>

Every non-trivial line (30+ characters after collapsing spaces; #include lines
ignored) of the repository's files is compared with the Shogo source release.
Three or more consecutive matching lines - copied code - fail the check.
Single matching lines are listed for information: they are the idioms any
code using the engine's API must write the same way (a struct initialiser, a
virtual method that overrides the engine's class)."""
import os, re, sys

EXTS = ('.cpp', '.h', '.c', '.rc', '.dsp', '.def', '.txt', '.md')

def norm(line):
    return re.sub(r'\s+', ' ', line.strip())

def lines_of(path):
    try:
        with open(path, encoding='latin-1') as f:
            return f.read().splitlines()
    except OSError:
        return []

def main():
    if len(sys.argv) != 2:
        print(__doc__); return 2
    src, repo = sys.argv[1], os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    monolith = set()
    for root, _, files in os.walk(src):
        for name in files:
            if name.lower().endswith(EXTS):
                for l in lines_of(os.path.join(root, name)):
                    n = norm(l)
                    if len(n) >= 30: monolith.add(n)
    singles, runs = [], []
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d not in ('.git', 'openxr')]
        for name in files:
            if not name.lower().endswith(EXTS) or name == 'leakcheck.py': continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, repo)
            run = []
            for i, l in enumerate(lines_of(path) + [''], 1):
                n = norm(l)
                if n.startswith('#include'): continue
                if n in monolith:
                    run.append((i, l.strip()))
                elif n and not re.fullmatch(r'[{}();]*', n):
                    if len(run) >= 3: runs.append((rel, run))
                    else: singles += [(rel, i2, t) for i2, t in run]
                    run = []
    for rel, i, t in singles: print(f"  info  {rel}:{i}: {t}")
    for rel, run in runs:
        print(f"  COPIED {rel}:{run[0][0]}-{run[-1][0]}:"); [print(f"         {t}") for _, t in run]
    print(f"{len(singles)} single API-idiom line(s); {len(runs)} copied block(s).")
    print("FAILED: copied code found." if runs else "OK: no copied code from the Shogo source.")
    return 1 if runs else 0

if __name__ == '__main__':
    sys.exit(main())
