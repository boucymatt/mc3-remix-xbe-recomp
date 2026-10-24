#!/usr/bin/env python3
"""Self-test of tools/check_repo_clean.py: it must refuse what it exists to refuse, and accept real source.

    python tools/test_repo_guard.py

Run it whenever the guard changes. A guard that has never been seen to fail proves nothing.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_repo_clean as guard  # noqa: E402


def reasons(path: str, data: bytes | None = b"int main() { return 0; }\n", allow: list[str] | None = None) -> str:
    return " | ".join(guard.check(path, data, allow or []))


class RefusesTheGame(unittest.TestCase):
    def test_game_folders(self):
        for top in ("assets", "generated", "video", "hdd", "captures", "gpu_captures", "logs", "out", "CrashDumps"):
            self.assertIn(f"{top}/", reasons(f"{top}/anything.txt"), top)

    def test_game_and_binary_suffixes(self):
        for name in ("default.xbe", "x.xbck", "intro.bik", "movie.pss", "disc.iso", "assets_x.dat", "a.bin",
                     "game.exe", "lib.dll", "game.pdb", "a.obj", "b.zip", "c.7z", "shot.png", "shot.JPG", "t.dds",
                     "s.wav", "crash.dmp"):
            self.assertTrue(guard.check(f"src/{name}", b"x", []), name)

    def test_numbered_data_archives_and_saves(self):
        self.assertTrue(guard.check("src/assets_x.dat_2", b"x", []))
        self.assertTrue(guard.check("src/mc2_career_progress.dat", b"x", []))

    def test_magic_bytes_beat_a_harmless_name(self):
        self.assertIn("XBE", reasons("src/notes.txt", b"XBEH" + b"\x00" * 64))
        self.assertIn("archive", reasons("src/notes.txt", b"DAVE" + b"\x00" * 64))
        self.assertIn("executable", reasons("src/notes.txt", b"MZ\x90\x00" + b"\x00" * 64))
        self.assertIn("Bink", reasons("src/notes.txt", b"BIKi" + b"\x00" * 64))
        self.assertIn("PNG", reasons("src/notes.txt", b"\x89PNG\r\n\x1a\n" + b"x" * 32))

    def test_binary_renamed_as_source(self):
        self.assertIn("NUL", reasons("src/game/a.cpp", b"int x;\x00\x01\x02" * 8))

    def test_embedded_blob(self):
        blob = ("static const unsigned char kData[] = {" + ", ".join(f"0x{i % 256:02x}" for i in range(300)) +
                "};\n").encode()
        self.assertIn("256+ byte literals", reasons("src/game/blob.h", blob))
        # a normal table is fine
        table = ("static const unsigned char kT[] = {" + ", ".join(f"0x{i:02x}" for i in range(16)) + "};\n").encode()
        self.assertEqual(reasons("src/game/table.h", table), "")

    def test_oversized_and_very_long_lines(self):
        self.assertIn("MiB", reasons("src/game/big.cpp", b"a;\n" * 500000))
        self.assertIn("characters", reasons("src/game/long.cpp", b"x" * (guard.LONG_LINE + 10)))

    def test_secrets_and_personal_data(self):
        self.assertIn("token", reasons("docs/a.md", b"key ghp_" + b"A" * 36))
        # built at run time: the literal would trip the guard on this very file
        self.assertIn("private key", reasons("docs/a.md", ("-----BEGIN " + "RSA PRIVATE KEY-----\n").encode()))
        self.assertIn("absolute path", reasons("docs/a.md", ("see C:" + "\\Users\\someone\\work\\x").encode()))
        self.assertIn("absolute path", reasons("docs/a.md", ("see X:" + "\\mc3-alpha-decomp\\research").encode()))

    def test_drive_paths_in_every_spelling(self):
        # the form that slipped through once: a C++ string literal, with escaped backslashes, in a default value
        escaped = ('static std::string dir = "' + "N:" + "\\\\" + "Some Folder" + "\\\\" + 'assets";').encode()
        self.assertIn("drive letter", reasons("src/game/a.cpp", escaped))
        self.assertIn("drive letter", reasons("src/game/a.cpp", ("N:" + "\\" + "Some Folder").encode()))
        self.assertIn("drive letter", reasons("src/game/a.cpp", ("D:" + "/" + "stuff/x").encode()))

    def test_urls_and_ordinary_colons_are_not_paths(self):
        for text in (b"see https://github.com/TGP482/XboxXBE-Recomp", b"ratio a:b and key: value",
                     b"std::string x; // note: see below", b"e.g. 16:9 and 12:30"):
            self.assertEqual(reasons("docs/a.md", text), "", text)
        self.assertIn("e-mail", reasons("docs/a.md", ("mail someone" + "@" + "gmail.com").encode()))


class AcceptsSource(unittest.TestCase):
    def test_ordinary_files(self):
        for name in ("src/game/hooks.cpp", "src/game/x.h", "tools/a.py", "docs/a.md", "config/game.toml", "build.bat",
                     "CMakeLists.txt", "sdk/xdk/5849/signatures.toml", ".gitignore"):
            self.assertEqual(reasons(name), "", name)

    def test_noreply_address_is_fine(self):
        self.assertEqual(reasons("docs/a.md", ("1+someone" + "@" + "users.noreply.github.com").encode()), "")


class AllowList(unittest.TestCase):
    def test_a_listed_path_is_skipped_and_only_that_path(self):
        rules = ["sdk/THIRD_PARTY.txt"]
        mail = ("a" + "@" + "gmail.com").encode()  # built at run time, see test_secrets_and_personal_data
        self.assertEqual(reasons("sdk/THIRD_PARTY.txt", mail, rules), "")
        self.assertNotEqual(reasons("docs/other.md", mail, rules), "")

    def test_a_line_rule_exempts_only_the_lines_it_matches(self):
        xbox = ("see D:" + "\\\\default.xbe").encode()  # an Xbox path: D: is the console's disc drive
        local = ("see N:" + "\\\\Some Folder").encode()
        rules = ["sdk/a.h :: D:.{1,2}default"]
        self.assertEqual(reasons("sdk/a.h", xbox, rules), "")
        # a different line in the same file is still refused, and so is the same line in another file
        self.assertNotEqual(reasons("sdk/a.h", local, rules), "")
        self.assertNotEqual(reasons("sdk/b.h", xbox, rules), "")
        # a line rule never exempts a game folder or a binary
        self.assertNotEqual(reasons("assets/x.txt", xbox, ["assets/x.txt :: default"]), "")

    def test_the_real_allow_file_never_lists_game_folders(self):
        for rule in guard.allowed():
            self.assertNotIn(rule.split("/")[0], guard.FORBIDDEN_TOP, rule)


if __name__ == "__main__":
    unittest.main(verbosity=1)
