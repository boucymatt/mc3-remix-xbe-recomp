#!/usr/bin/env python3
"""Refuse files that could carry the original game, or something made from it, into this repository.

The repository holds source code only. The game's files, the recompiled game (generated/), captures of the game,
saves and binaries must never be committed. This script is run by the pre-commit hook (see CONTRIBUTING.md) and can
be run by hand:

    python tools/check_repo_clean.py            every tracked file and every untracked file git would add
    python tools/check_repo_clean.py --staged   only what is staged for the next commit (the hook)
    python tools/check_repo_clean.py --history  also every file in every commit reachable from HEAD

Exit status 0: clean. 1: findings, each printed as  path: reason.
A finding that is a conscious exception goes in tools/repo_guard_allow.txt with a reason, and a reviewer must agree.
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOW_FILE = ROOT / "tools" / "repo_guard_allow.txt"

# folders at the top of the repository that only ever hold the game, or things made from it
FORBIDDEN_TOP = {"assets", "generated", "video", "hdd", "saves", "captures", "gpu_captures", "logs", "out", "build",
                 "CrashDumps", ".local"}

# game files, binaries, archives, captures: none belongs in a source repository
FORBIDDEN_SUFFIX = {
    ".xbe", ".xbck", ".xbx", ".xtf", ".bik", ".pss", ".iso", ".dat", ".bin", ".pck", ".tex", ".xwb", ".xsb", ".xmv",
    ".exe", ".dll", ".sys", ".pdb", ".obj", ".o", ".lib", ".a", ".so", ".dmp",
    ".zip", ".7z", ".rar", ".gz", ".xz", ".tar",
    ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".gif", ".tif", ".tiff", ".psd", ".webp",
    ".ogg", ".wav", ".wma", ".mp3", ".mp4", ".avi", ".mkv", ".webm",
}
FORBIDDEN_NAME = re.compile(r"\.dat_\d+$|^mc2_career_progress", re.IGNORECASE)

# the start of a file says what it is, whatever its name
MAGIC = [
    (b"XBEH", "an Xbox executable (XBE)"),
    (b"DAVE", "a game archive (DAVE)"),
    (b"Dave", "a game archive (Dave)"),
    (b"MZ", "a Windows executable"),
    (b"\x7fELF", "an ELF binary"),
    (b"PK\x03\x04", "a zip archive"),
    (b"7z\xbc\xaf\x27\x1c", "a 7z archive"),
    (b"BIKi", "a Bink movie"),
    (b"BIKb", "a Bink movie"),
    (b"BIKf", "a Bink movie"),
    (b"RIFF", "a RIFF media file"),
    (b"OggS", "an Ogg media file"),
    (b"\x89PNG", "a PNG image"),
    (b"\xff\xd8\xff", "a JPEG image"),
    (b"GIF8", "a GIF image"),
    (b"DDS ", "a DDS texture"),
]

TEXT_SUFFIX = {".cpp", ".h", ".hpp", ".c", ".inl", ".py", ".md", ".toml", ".txt", ".cmd", ".bat", ".cmake", ".json",
               ".sh", ".hlsl", ".yml", ".yaml", ".gitignore", ".gitattributes", ".example", ""}

MAX_BYTES = 1 << 20  # a source file this large is almost certainly data

# a run of byte literals is an embedded blob; real source has a few tables, a game asset has thousands of bytes
BYTE_RUN = re.compile(r"(?:0x[0-9a-fA-F]{2}\s*,\s*){256,}")
LONG_LINE = 6000

PATTERNS = [
    (re.compile(r"gh[pousr]_[A-Za-z0-9]{20,}"), "looks like a GitHub token"),
    (re.compile(r"github_pat_[A-Za-z0-9_]{20,}"), "looks like a GitHub token"),
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"), "a private key"),
    (re.compile(r"AKIA[0-9A-Z]{16}"), "looks like an AWS key"),
    (re.compile(r"[A-Za-z]:\\Users\\[^\\\s]+", re.IGNORECASE), "a personal absolute path"),
    # any drive letter path: a letter, a colon, then a backslash (or two, as C++ escapes it) or a slash and a name.
    # The look-behind keeps URL schemes (https://) and words out of it.
    (re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:(?:\\{1,2}|/)[A-Za-z0-9_.$]"), "a local absolute path (a drive letter)"),
    (re.compile(r"[\w.+-]+@(?:gmail|outlook|hotmail|yahoo|icloud|proton)\.[a-z.]+", re.IGNORECASE),
     "a personal e-mail address"),
]


def git(*args: str, binary: bool = False):
    out = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, check=False)
    if out.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed: {out.stderr.decode(errors='replace').strip()}")
    return out.stdout if binary else out.stdout.decode("utf-8", errors="replace")


def allowed() -> list[str]:
    """The lines of the allow file, comments stripped. `glob` exempts a whole file; `glob :: regex` exempts only the
    lines of that file the regex matches, so one legitimate string does not switch the guard off for its file."""
    if not ALLOW_FILE.exists():
        return []
    rules = []
    for line in ALLOW_FILE.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            rules.append(line)
    return rules


def split_rule(rule: str) -> tuple[str, str | None]:
    glob, sep, regex = rule.partition("::")
    return glob.strip(), (regex.strip() if sep else None)


def is_allowed(path: str, rules: list[str]) -> bool:
    """The whole file is exempt (a rule with no regex)."""
    return any(fnmatch.fnmatch(path, glob) for glob, regex in map(split_rule, rules) if regex is None)


def line_allowed(path: str, line: str, rules: list[str]) -> bool:
    """This one line of the file is exempt (a rule with a regex that matches it)."""
    return any(fnmatch.fnmatch(path, glob) and re.search(regex, line)
               for glob, regex in map(split_rule, rules) if regex)


def check(path: str, data: bytes | None, rules: list[str]) -> list[str]:
    """Every reason `path` (with its bytes, if readable) must not be committed."""
    if is_allowed(path, rules):
        return []
    found: list[str] = []
    parts = path.split("/")
    name = parts[-1]
    suffix = Path(name).suffix.lower()
    if parts[0] in FORBIDDEN_TOP:
        found.append(f"lives under {parts[0]}/, which only ever holds the game or what is made from it")
    if suffix in FORBIDDEN_SUFFIX:
        found.append(f"a {suffix} file: game files, binaries, archives and captures are not source")
    if FORBIDDEN_NAME.search(name):
        found.append("a game data or save file name")
    if data is None:
        return found
    if len(data) > MAX_BYTES:
        found.append(f"{len(data) / 1048576:.1f} MiB: larger than a source file should be")
    head = data[:16]
    for magic, what in MAGIC:
        if head.startswith(magic):
            found.append(f"starts like {what}")
            break
    if suffix in TEXT_SUFFIX and b"\x00" in data[:65536]:
        found.append("has NUL bytes but a text name: a binary file renamed")
    if suffix in TEXT_SUFFIX and b"\x00" not in data[:65536]:
        text = data.decode("utf-8", errors="replace")
        if BYTE_RUN.search(text):
            found.append("a run of 256+ byte literals: an embedded blob, not source")
        for number, line in enumerate(text.splitlines(), 1):
            if len(line) > LONG_LINE:
                found.append(f"line {number} is {len(line)} characters: embedded data?")
                break
        lines = text.splitlines()
        for pattern, what in PATTERNS:
            for match in pattern.finditer(text):
                line_no = text.count("\n", 0, match.start()) + 1
                if line_allowed(path, lines[line_no - 1] if line_no <= len(lines) else "", rules):
                    continue
                found.append(f"line {line_no}: {what}")
                break  # one finding per rule and file is enough to act on
    return found


def files(mode: str) -> list[tuple[str, bytes | None]]:
    if mode == "staged":
        names = git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split("\0")
        return [(n, git("show", f":{n}", binary=True)) for n in names if n]
    names = git("ls-files", "--cached", "--others", "--exclude-standard", "-z").split("\0")
    result = []
    for n in names:
        p = ROOT / n
        result.append((n, p.read_bytes() if p.is_file() else None))
    return [(n, d) for n, d in result if n and d is not None]


def history() -> list[tuple[str, bytes | None]]:
    """Every distinct blob reachable from HEAD, named by the first path it appeared under."""
    listing = git("rev-list", "--objects", "--all")
    result = []
    for line in listing.splitlines():
        sha, _, name = line.partition(" ")
        if not name:
            continue
        if git("cat-file", "-t", sha).strip() == "blob":
            result.append((name, git("cat-file", "blob", sha, binary=True)))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staged", action="store_true", help="only what is staged for the next commit")
    parser.add_argument("--history", action="store_true", help="also every file in every reachable commit")
    args = parser.parse_args()

    rules = allowed()
    todo = files("staged" if args.staged else "tree")
    if args.history:
        todo += history()
    bad = 0
    seen = set()
    for path, data in todo:
        for reason in check(path, data, rules):
            if (path, reason) in seen:
                continue
            seen.add((path, reason))
            print(f"{path}: {reason}")
            bad += 1
    if bad:
        print(f"\n{bad} problem(s). Nothing derived from the game may be committed.\n"
              "If a finding is a real exception, list the path and the reason in tools/repo_guard_allow.txt\n"
              "and say so in the pull request. If the file is already committed, do not just delete it: see\n"
              "'If a game file was committed' in CONTRIBUTING.md.", file=sys.stderr)
        return 1
    print(f"repo guard: {len(todo)} file(s) checked, clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
