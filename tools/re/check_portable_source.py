#!/usr/bin/env python3
"""Reject author-specific absolute paths in the current distributable source.

No game is required. An optional root argument supports extracted source bundles.
This audits the current tree, not historical Git objects or external local backups.
"""
from pathlib import Path
import re
import sys

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
TEXT = {'.cpp', '.c', '.h', '.hpp', '.def', '.rc', '.manifest', '.ps1', '.py', '.sh', '.md', '.txt', '.yml', '.yaml', '.json'}
SKIP = {'__pycache__', 'build', 'dist', 'out', 'logs', 'tmp', '.git', 'archive'}
# Require a token boundary so https:// links are not mistaken for drive letters.
DRIVE = re.compile(r'''(?:^|[\s"'=(])([A-Za-z]:[\\/][^\r\n"']*)''')
PRIVATE = re.compile(r'(?i)(?:Tencent Files[\\/]|WorkBuddy[\\/]|Claude Sessions[\\/]|Ra3 Bootstrap[\\/])')

def candidates():
    for file in ROOT.iterdir():
        if file.is_file() and (file.suffix.lower() in TEXT or file.name == 'LICENSE'):
            yield file
    for name in ('src', 'tests', 'tools', 'docs', '.github'):
        folder = ROOT / name
        if not folder.exists():
            continue
        for file in folder.rglob('*'):
            if file.is_file() and file.suffix.lower() in TEXT and not any(p in SKIP for p in file.relative_to(folder).parts):
                if file.name != '_symcache.txt':
                    yield file

failed = 0
checked = 0
for file in sorted(set(candidates())):
    checked += 1
    for line_no, line in enumerate(file.read_text(encoding='utf-8-sig').splitlines(), 1):
        if DRIVE.search(line) or PRIVATE.search(line):
            print(f'FAIL {file.relative_to(ROOT)}:{line_no}: machine-specific path')
            failed += 1
print(f'Source portability: {checked} files, {failed} machine-specific paths')
sys.exit(1 if failed else 0)
