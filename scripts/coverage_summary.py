#!/usr/bin/env python3
"""Summarise gcov JSON output (one JSON document per line) into per-file coverage.

usage: coverage_summary.py <repo-root> <gcov-jsonl> <output-json>

Only files under src/ and tools/ count. A line counts as covered if any object
file executed it; a function counts as covered if any object file called it.
Lines gcov reports with count 0 but marked "unexecuted_block" only are still
counted as not covered: gcov already excludes lines with no code.
"""

import json
import os
import sys


def main() -> int:
    root, jsonl, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    root = os.path.realpath(root)
    lines: dict[str, dict[int, bool]] = {}
    functions: dict[str, dict[str, bool]] = {}

    with open(jsonl, encoding="utf-8") as stream:
        for raw in stream:
            raw = raw.strip()
            if not raw:
                continue
            document = json.loads(raw)
            cwd = document.get("current_working_directory", "")
            for entry in document.get("files", []):
                path = entry["file"]
                if not os.path.isabs(path):
                    path = os.path.join(cwd, path)
                path = os.path.realpath(path)
                if not path.startswith(root + os.sep):
                    continue
                relative = os.path.relpath(path, root)
                if not relative.startswith(("src" + os.sep, "tools" + os.sep)):
                    continue
                file_lines = lines.setdefault(relative, {})
                for line in entry.get("lines", []):
                    number = line["line_number"]
                    file_lines[number] = file_lines.get(number, False) or line["count"] > 0
                file_functions = functions.setdefault(relative, {})
                for function in entry.get("functions", []):
                    name = function["demangled_name"]
                    executed = function["execution_count"] > 0
                    file_functions[name] = file_functions.get(name, False) or executed

    rows = []
    for relative in sorted(lines):
        total = len(lines[relative])
        covered = sum(lines[relative].values())
        fn_total = len(functions.get(relative, {}))
        fn_covered = sum(functions.get(relative, {}).values())
        uncovered = sorted(n for n, hit in lines[relative].items() if not hit)
        rows.append({"file": relative, "lines": total, "lines_covered": covered,
                     "functions": fn_total, "functions_covered": fn_covered,
                     "uncovered_lines": uncovered})

    def percent(part: int, whole: int) -> str:
        return f"{100.0 * part / whole:5.1f} %" if whole else "    –  "

    width = max((len(r["file"]) for r in rows), default=10)
    print(f"{'File':<{width}}  {'Lines':>15}  {'Functions':>15}")
    for r in rows:
        print(f"{r['file']:<{width}}  {percent(r['lines_covered'], r['lines']):>8} of {r['lines']:>4}"
              f"  {percent(r['functions_covered'], r['functions']):>8} of {r['functions']:>4}")
    total_lines = sum(r["lines"] for r in rows)
    total_covered = sum(r["lines_covered"] for r in rows)
    total_fn = sum(r["functions"] for r in rows)
    total_fn_covered = sum(r["functions_covered"] for r in rows)
    print(f"{'TOTAL':<{width}}  {percent(total_covered, total_lines):>8} of {total_lines:>4}"
          f"  {percent(total_fn_covered, total_fn):>8} of {total_fn:>4}")

    with open(out_path, "w", encoding="utf-8") as out:
        json.dump(rows, out, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
