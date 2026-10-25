#!/usr/bin/env python3
"""Check project instruction budgets, local links, and Git tracking (stdlib only)."""

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


LINK = re.compile(r"!?\[[^\]\n]*\]\(([^)\n]+)\)")
FENCE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")


def local_links(source):
    """Read inline Markdown links, excluding examples inside fenced code blocks."""
    fence = None
    for number, line in enumerate(source.splitlines(), 1):
        match = FENCE.match(line)
        if match:
            marker = match[1]
            if fence is None:
                fence = marker
            elif marker[0] == fence[0] and len(marker) >= len(fence):
                fence = None
            continue
        if fence:
            continue
        # Inline C++ lambdas such as `Update([](T& value) {...})` are not links.
        prose = re.sub(r"(`+).*?\1", "", line)
        for match in LINK.finditer(prose):
            target = match[1].strip()
            if target.startswith("<"):
                target = target[1:target.index(">")]
            else:
                target = target.split(' "', 1)[0]
            parsed = urlsplit(target)
            if not parsed.scheme and not parsed.netloc and parsed.path:
                yield number, unquote(parsed.path)


def check(root):
    tracked = set(subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "--cached", "-z"], text=True
    ).split("\0"))
    errors = []
    root_guide = root / "AGENTS.md"
    skills = sorted((root / "project-skills").rglob("SKILL.md"))
    queue = [root_guide, root / "CLAUDE.md", *skills]
    queue.extend((root / "project-skills").rglob("*.md"))
    checked = set()

    def tracked_file(path):
        return path.relative_to(root).as_posix() in tracked

    for name in sorted(tracked):
        if Path(name).name == "SKILL.md" and not name.startswith("project-skills/"):
            errors.append(f"{name}: project skills must live under project-skills/")

    for path, max_lines, max_chars in [(root_guide, 150, 6000),
                                        *((p, 100, None) for p in skills)]:
        if not path.is_file():
            errors.append(f"{path.relative_to(root)}: missing file")
            continue
        source = path.read_bytes().decode("utf-8")
        lines, chars = len(source.splitlines()), len(source)
        label = path.relative_to(root)
        print(f"{label}: {lines} lines, {chars} characters")
        if lines > max_lines:
            errors.append(f"{label}: {lines} lines exceeds {max_lines}")
        if max_chars is not None and chars > max_chars:
            errors.append(f"{label}: {chars} characters exceeds {max_chars}")

    # Check every skill resource, including non-Markdown helpers and metadata.
    for path in (root / "project-skills").rglob("*"):
        if path.is_file() and not tracked_file(path):
            errors.append(f"{path.relative_to(root)}: not tracked; stage project resources")

    while queue:
        path = queue.pop().resolve()
        if path in checked:
            continue
        checked.add(path)
        if not path.is_relative_to(root):
            errors.append(f"{path}: project references must stay inside the repository")
            continue
        label = path.relative_to(root)
        if not path.is_file():
            errors.append(f"{label}: missing local reference")
            continue
        if not tracked_file(path):
            errors.append(f"{label}: not tracked; stage project references")
        if path.suffix != ".md":
            continue
        for number, target in local_links(path.read_text(encoding="utf-8")):
            destination = (path.parent / target).resolve()
            if not destination.is_relative_to(root):
                errors.append(f"{label}:{number}: reference escapes repository: {target}")
            elif not destination.is_file():
                errors.append(f"{label}:{number}: missing local reference: {target}")
            else:
                queue.append(destination)

    if errors:
        for error in sorted(set(errors)):
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"OK: budgets and Git-tracked local references ({len(checked)} files)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    return check(args.root.resolve())


if __name__ == "__main__":
    sys.exit(main())
