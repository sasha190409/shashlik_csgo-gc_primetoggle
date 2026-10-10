#!/usr/bin/env python3
import os
import re
from pathlib import Path

SKIP_FILES = {"httplib.h"}
SKIP_DIRS  = {".git", "build", "out", "cmake-build-debug", "cmake-build-release", ".vs"}

# lines that are just "// =====" or "// -----" etc -> kill them
SEPARATOR_RE = re.compile(r"^\s*//\s*[=\-_]{3,}\s*$")

# exact stripped-line replacements (russian -> english, and "--- x ---" -> "x")
REPLACEMENTS = {
    # gc_client.cpp - OnClientRequestPrestigeCoin
    "// Already at max level (4878). Show the client a popup,":
        "// already at max level (4878). show the client a popup,",
    "// Reset level and XP on prestige":
        "// reset level and XP on prestige",

    # gc_client.cpp - OnMatchmakingStart continuation
    "//    keeps the Play button enabled and the client never knows the":
        "//    keeps the play button enabled and the client never knows the",

    # inventory.cpp - only if you don't treat ONLY as an emphasis marker
    "// mark as \"new\" ONLY if this item wasn't already in memory before the reload":
        "// mark as \"new\" only if this item wasn't already in memory before the reload",
}


def process_file(path: Path) -> bool:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        print(f"skip (not utf-8): {path}")
        return False

    out_lines = []
    changed = False
    for line in text.splitlines(keepends=True):
        stripped = line.rstrip("\n").rstrip("\r")

        # nuke decorative separator lines
        if SEPARATOR_RE.match(stripped):
            changed = True
            continue

        # exact replacements
        key = stripped.strip()
        if key in REPLACEMENTS:
            indent = stripped[:len(stripped) - len(stripped.lstrip())]
            nl = "\n" if line.endswith("\n") else ""
            out_lines.append(indent + REPLACEMENTS[key] + nl)
            changed = True
            continue

        out_lines.append(line)

    if changed:
        path.write_text("".join(out_lines), encoding="utf-8")
        print(f"updated: {path}")
    return changed


def main():
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            if name in SKIP_FILES:
                continue
            process_file(Path(root) / name)


if __name__ == "__main__":
    main()