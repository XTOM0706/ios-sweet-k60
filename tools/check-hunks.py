#!/usr/bin/env python3
"""Verify every patch's @@ line counts against its body.

dxb applies the series with `patch`, which silently stops a hunk when the
header promises fewer lines than the body has: the file ends up truncated and
the only symptom is a compile error deep in the tree (2026-10-31, 0248).
Run this before pushing a patch.
"""
import glob
import re
import sys

bad = 0
for f in sorted(glob.glob('xnu/patches/*.patch')):
    lines = open(f, errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        m = re.match(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', lines[i])
        if not m:
            i += 1
            continue
        want_old = int(m.group(2) or 1)
        want_new = int(m.group(4) or 1)
        j = i + 1
        got_old = got_new = 0
        while j < len(lines):
            l = lines[j]
            if l.startswith('@@') or l.startswith('diff --git') or (
                    l.startswith('--- ') and j + 1 < len(lines) and lines[j + 1].startswith('+++ ')):
                break
            c = l[:1]
            if c == ' ':
                got_old += 1
                got_new += 1
            elif c == '-':
                got_old += 1
            elif c == '+':
                got_new += 1
            elif l == '':
                break
            else:
                break
            j += 1
        if (got_old, got_new) != (want_old, want_new):
            print('%s:%d: header says -%d/+%d, body has -%d/+%d'
                  % (f, i + 1, want_old, want_new, got_old, got_new))
            bad += 1
        i = j

print('hunks checked, %d bad' % bad)
sys.exit(1 if bad else 0)
