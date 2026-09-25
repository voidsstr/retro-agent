"""rawfloppy.c invariants - the failure modes are all silent ones.

A raw floppy writer that reports success after a short write is worse than no
writer at all: `dir a:` lists files fine off a disk whose last track never
landed, and the fault surfaces as `Non-System disk` in front of the opened
machine.  These assertions pin the checks that make a partial write loud.
"""
import os
import shutil
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, "scripts", "fleet", "rawfloppy", "rawfloppy.c")


class TestRawFloppySource(unittest.TestCase):
    def setUp(self):
        with open(SRC, "r", encoding="utf-8") as fh:
            self.src = fh.read()

    def test_short_write_is_a_failure(self):
        # WriteFile can succeed having written fewer bytes than asked.
        self.assertIn("did != got", self.src)

    def test_volume_is_locked_and_dismounted_before_writing(self):
        # Without these the filesystem writes its own cached FAT back over us.
        self.assertIn("FSCTL_LOCK_VOLUME", self.src)
        self.assertIn("FSCTL_DISMOUNT_VOLUME", self.src)

    def test_an_empty_image_is_not_reported_as_success(self):
        self.assertIn("image was empty", self.src)

    def test_failures_name_the_byte_offset(self):
        for marker in ("write failed at byte", "read failed at byte"):
            self.assertIn(marker, self.src)

    def test_verify_compares_the_bytes(self):
        self.assertIn("memcmp", self.src)

    def test_it_compiles(self):
        cc = shutil.which("i686-w64-mingw32-gcc")
        if cc is None:
            self.skipTest("SKIPPED LOUDLY: i686-w64-mingw32-gcc not installed "
                          "- rawfloppy.exe was NOT compile-checked")
        out = os.path.join("/tmp", "rawfloppy_test.exe")
        r = subprocess.run([cc, "-O2", "-s", "-o", out, SRC,
                            "-lkernel32", "-luser32"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.getsize(out) > 0)
        os.unlink(out)


if __name__ == "__main__":
    unittest.main()
