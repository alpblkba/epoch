"""Reports internal work-package labels and private-document citations in files that may become public.

Scans every file under the repository root, including Git-ignored ones, except the private layer, the agent entrypoints, Git storage, build output, virtual environments and binaries. A match is a development-tracking label, meaning a milestone code (series letters, optional separator, two digits) or a review finding code (F, dash, digit), or a citation of the private planning archive: a path into its self-docs, tracelog or review directories, the private protocol notes file, a numbered proposal version, a numbered handbook section, a numbered note of the charter, a TBD pointing at a Markdown file, or a lettered owner decision. Fault checkpoints such as F1 and F2a carry no dash and are not matched, and ordinary words such as protocol, proposal or handbook match only in those citation forms. The patterns are a bounded family, so a clean result does not prove that no semantic reference to private material remains.

Prints one "path:line: text" per match, including matches in file and directory names, and exits 1 when any exist, 0 otherwise. Read only.

Usage: python3 scripts/check_public_terms.py [repository_root]
"""

import os
import re
import sys

PATTERNS = [
    # Only letters and digits count as neighbors, so a code joined to a name by an underscore still matches, while a digit run inside a word does not.
    re.compile(r"(?<![a-z0-9])(?:hw|dev|doc|core|sim|rtl|gov)[-_ ]?\d{2}[a-d]?(?![a-z0-9])", re.IGNORECASE),  # milestone codes
    re.compile(r"(?<![A-Za-z0-9])F-\d(?![A-Za-z0-9])"),  # review finding codes
    re.compile(r"\bprivate/(?:self-docs|tracelog|review)\b|\bprotocol\.md\b|\bproposal v\d|\bhandbook (?:section )?\d|\bcharter note\b"
               r"|\bTBD in \S+\.md\b|\bowner decision [A-Z0-9]\b", re.IGNORECASE),  # private-document citations
]
# Private by design or never published: the private layer, the agent entrypoints and their settings, Git storage, local backup.
SKIP_TOP = {".git", "private", "backup", ".claude", ".dist", "AGENTS.md", "CLAUDE.md", ".gitignore"}
SKIP_DIRS = {"__pycache__", ".venv", "venv", "node_modules"}
SKIP_SUFFIXES = (".bin", ".elf", ".hex", ".o", ".a", ".pyc", ".map")


def is_build_dir(name):
    return name == "build" or name.startswith("build-") or name.startswith("cmake-build")


def matches(text):
    return any(p.search(text) for p in PATTERNS)


def scan(root):
    """Returns a sorted list of (relative path, line number or 0 for the name itself, text)."""
    found = []
    for top in sorted(os.listdir(root)):
        if top in SKIP_TOP or top in SKIP_DIRS or is_build_dir(top):
            continue
        path = os.path.join(root, top)
        walk = os.walk(path) if os.path.isdir(path) else [(root, [], [top])]
        for d, dirs, files in walk:
            dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS and not is_build_dir(x))
            for name in sorted(files):
                full = os.path.join(d, name)
                rel = os.path.relpath(full, root)
                if matches(rel):
                    found.append((rel, 0, rel))
                if name.endswith(SKIP_SUFFIXES) or name == ".DS_Store":
                    continue
                try:
                    with open(full, "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                if b"\0" in data[:8192]:
                    continue  # binary
                for i, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
                    if matches(line):
                        found.append((rel, i, line.strip()))
    return found


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    found = scan(os.path.abspath(root))
    for rel, line, text in found:
        print(f"{rel}:{line}: {text[:200]}")
    print(f"{len(found)} internal label(s) or private citation(s) found")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
