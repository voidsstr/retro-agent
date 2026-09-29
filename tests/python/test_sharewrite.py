"""scripts/fleet/sharewrite.py - the path handling that decides WHERE a write lands.

The network half (smbclient put + md5 read-back through /mnt) is exercised by
running the tool; these pin the pure parts, because a wrong split here writes a
file into the wrong directory of the live library.
"""
import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "sharewrite", os.path.join(HERE, "..", "..", "scripts", "fleet", "sharewrite.py"))
sw = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sw)


class SharePaths(unittest.TestCase):
    def test_forward_and_back_slashes_split_the_same(self):
        a = sw.share_parts("Files/Games-Library/Quake3-TeamArena/baseq3/zz.pk3")
        b = sw.share_parts("\\Files\\Games-Library\\Quake3-TeamArena\\baseq3\\zz.pk3")
        self.assertEqual(a, b)
        self.assertEqual(a, (["Files", "Games-Library", "Quake3-TeamArena", "baseq3"], "zz.pk3"))

    def test_parent_traversal_is_refused(self):
        with self.assertRaises(ValueError):
            sw.share_parts("Files/Games-Library/../Utility/x")

    def test_empty_is_refused(self):
        with self.assertRaises(ValueError):
            sw.share_parts("/")

    def test_quote_refuses_what_smbclient_would_split_on(self):
        self.assertEqual(sw.smb_quote("Play Quake.bat"), '"Play Quake.bat"')
        for bad in ('a"b', "a;b"):
            with self.assertRaises(ValueError):
                sw.smb_quote(bad)


if __name__ == "__main__":
    unittest.main()
