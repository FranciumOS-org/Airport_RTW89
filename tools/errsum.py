#!/usr/bin/env python3
"""Group clang errors from build/log/*.log into a shim work list.

Usage: python3 tools/errsum.py build/log build/log/compile-status.md

Errors are grouped by what has to be done about them (a missing identifier, a
missing struct member, a type mismatch, ...) so one shim fix can be matched to
every error it clears. Run after `make -k compile`.
"""
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

DIAG_RE = re.compile(r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|fatal error): (?P<msg>.+)$")

PATTERNS = [
    ("missing header", re.compile(r"'([^']+)' file not found")),
    ("undeclared function", re.compile(r"call to undeclared function '(\w+)'")),
    ("undeclared identifier", re.compile(r"use of undeclared identifier '(\w+)'")),
    ("unknown type", re.compile(r"unknown type name '(\w+)'")),
    ("missing struct member", re.compile(r"no member named '(\w+)' in '(?:struct |union )?(\w+)'")),
    ("incomplete type", re.compile(r"incomplete (?:definition of )?type '(?:struct )?(\w+)'")),
    ("redefinition", re.compile(r"redefinition of '(\w+)'")),
    ("conflicting types", re.compile(r"conflicting types for '(\w+)'")),
    ("pointer type mismatch", re.compile(r"incompatible pointer types")),
    ("int/pointer conversion", re.compile(r"incompatible (?:integer to pointer|pointer to integer) conversion")),
]


def classify(msg):
    for name, rx in PATTERNS:
        m = rx.search(msg)
        if m:
            key = ".".join(reversed(m.groups())) if len(m.groups()) > 1 else (m.group(1) if m.groups() else msg)
            return name, key
    return "other", re.sub(r"'[^']*'", "'…'", msg)


def main():
    log_dir, out = Path(sys.argv[1]), Path(sys.argv[2])
    groups = defaultdict(lambda: defaultdict(lambda: {"n": 0, "files": set(), "where": None}))
    per_file = Counter()
    failed = []
    for log in sorted(log_dir.glob("*.log")):
        errs = 0
        for line in log.read_text(errors="replace").splitlines():
            m = DIAG_RE.match(line)
            if not m:
                continue
            errs += 1
            cat, key = classify(m["msg"])
            g = groups[cat][key]
            g["n"] += 1
            g["files"].add(Path(m["file"]).name)
            g["where"] = g["where"] or f"{Path(m['file']).name}:{m['line']}"
        if errs:
            per_file[log.stem] = errs
            failed.append(log.stem)

    total = sum(per_file.values())
    lines = ["# Compile status", "",
             f"**{len(failed)}** files with errors, **{total}** errors total.", "",
             "| File | Errors |", "|---|---|"]
    lines += [f"| {f} | {n} |" for f, n in per_file.most_common()]
    order = [p[0] for p in PATTERNS] + ["other"]
    for cat in order:
        if cat not in groups:
            continue
        items = sorted(groups[cat].items(), key=lambda kv: -kv[1]["n"])
        lines += ["", f"## {cat} ({len(items)})", "", "| What | Errors | First seen | Files |", "|---|---|---|---|"]
        for key, g in items:
            files = ", ".join(sorted(g["files"])[:4]) + (" …" if len(g["files"]) > 4 else "")
            lines.append(f"| `{key}` | {g['n']} | {g['where']} | {files} |")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"{len(failed)} files, {total} errors -> {out}")
    for cat in order:
        if cat in groups:
            print(f"  {cat:24s} {len(groups[cat]):4d} distinct, {sum(g['n'] for g in groups[cat].values()):5d} errors")


if __name__ == "__main__":
    main()
