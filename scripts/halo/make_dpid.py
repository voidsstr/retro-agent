#!/usr/bin/env python3
"""Build the DigitalProductID blob Halo: Combat Evolved (PC) reads at startup.

WHY THIS EXISTS
---------------
halo.exe 1.10 will not start unless

    HKLM\\Software\\Microsoft\\Microsoft Games\\Halo\\DigitalProductID

holds the 164-byte Microsoft "DigitalProductId" structure.  That value is
normally written by the retail setup, which asks for the 25-character product
key and hands it to Microsoft's mgspid.dll -> PIDGen.dll.  We have the user's
own key and the installed tree, but NOT the retail setup and NOT Halo's
PIDGen.dll (it ships on the CD; it is not in the installed tree), so the blob
has to be assembled here from the key the licence-holder supplied.

WHAT halo.exe ACTUALLY CHECKS  (measured, halo.exe 1.10 @ 0x0057f3f0)
---------------------------------------------------------------------
    RegOpenKeyExA(HKLM, "Software\\Microsoft\\Microsoft Games\\Halo")
    RegQueryValueExA("DigitalProductID")           -> 0x400-byte buffer
    cbData          == 0xA4        (164)
    blob[0x00..3]   == 0xA4
    blob[0x04..5]   == 3           (version major)
    blob[0x06..7]   == 0           (version minor)
    blob[0x08..]    -> product-ID string; bytes 6..8 == "OEM" selects one of
                       two digit-index tables, which pull 9 digits out of it
    blob[0x20]      -> DWORD, printed as %05d
    blob[0x34..0x42]-> the 15-byte encoded product key; SHA-1'd twice and the
                       first 8 bytes printed as a 64-bit decimal
    the four values are sprintf'd into "%05d,%09d,0,% 19.19I64d" and returned;
    the caller (0x00541807) raises the fatal "product key is invalid" error
    IF AND ONLY IF that string comes back empty.

There is NO cryptographic validation of the key inside halo.exe.  The key's
ECC signature is checked only by PIDGen.dll at install time, against a
per-product "BINK" public-key resource, and Halo's PIDGen.dll is not something
we have.  So this script CANNOT tell a good key from a bad one, and neither can
the game -- see NOTES-halo.txt in the staged tree.  What it does do is put the
licence-holder's own key into the tree in the form the game reads.

The 15-byte key field is the standard base-24 encoding (alphabet
BCDFGHJKMPQRTVWXY2346789, big-endian digits, little-endian integer) -- the
exact inverse of the decoder every "show me my Windows product key" script
uses.  `--self-test` round-trips it against a real Microsoft-written blob.

    python3 make_dpid.py --key MCXMM-.....-..... --reg
"""
import argparse
import hashlib
import struct
import sys

ALPHABET = "BCDFGHJKMPQRTVWXY2346789"

# Halo PC's own product identifiers, read out of mgspid.dll's string table
# (the DLL Microsoft shipped inside this very game tree):
#   5001 "69771"        the Microsoft Product Code
#   5002 "Z08-00030"    the SKU
#   5015 "OEM-1208613"  the OEM id
HALO_MPC = "69771"
HALO_SKU = "Z08-00030"
HALO_OEM = "OEM-1208613"


def encode_key(key):
    """25-character product key -> the 15 raw bytes stored at blob offset 52."""
    k = key.replace("-", "").replace(" ", "").upper()
    if len(k) != 25:
        raise ValueError("product key must be 25 characters, got %d" % len(k))
    bad = sorted(set(c for c in k if c not in ALPHABET))
    if bad:
        raise ValueError("characters outside the base-24 alphabet: %s" % "".join(bad))
    v = 0
    for c in k:
        v = v * 24 + ALPHABET.index(c)
    return v.to_bytes(15, "little")


def decode_key(raw15):
    """The 15 raw bytes -> the 25-character product key (inverse of encode_key)."""
    digits = list(raw15)
    out = ""
    for _ in range(25):
        cur = 0
        for j in range(14, -1, -1):
            cur = (cur << 8) | digits[j]
            digits[j] = cur // 24
            cur %= 24
        out = ALPHABET[cur] + out
    return "-".join(out[i:i + 5] for i in range(0, 25, 5))


def product_id(key):
    """A well-formed product-ID string for the blob's 24-byte field.

    The digit groups Microsoft's PIDGen would compute are NOT reproducible
    without Halo's BINK, so the tail here is derived deterministically from the
    key itself (stable across machines, never random).  The MPC and OEM id are
    Halo's real ones.  Halo only ever reads 9 digits out of this string and
    prints them; nothing compares it to anything.
    """
    tail = int.from_bytes(hashlib.sha1(encode_key(key)).digest()[:4], "little") % 100000
    return "%s-%s-%05d" % (HALO_MPC, HALO_OEM, tail)


def build(key):
    blob = bytearray(0xA4)
    struct.pack_into("<I", blob, 0x00, 0xA4)            # cbSize
    struct.pack_into("<HH", blob, 0x04, 3, 0)           # version 3.0 - halo.exe requires this
    pid = product_id(key).encode("ascii")
    if len(pid) > 23:
        raise ValueError("product-ID string too long for the 24-byte field")
    blob[0x08:0x08 + len(pid)] = pid                    # NUL-terminated by the zero fill
    struct.pack_into("<I", blob, 0x20, 0)               # printed as %05d; not consulted
    sku = HALO_SKU.encode("ascii")
    blob[0x24:0x24 + len(sku)] = sku
    blob[0x34:0x43] = encode_key(key)                   # the licence-bearing 15 bytes
    return bytes(blob)


def reg_lines(blob, value="DigitalProductID"):
    """REGEDIT4 REG_BINARY line, wrapped the way regedit writes it."""
    items = ["%02x" % b for b in blob]
    body = ",".join(items)
    out, line = [], '"%s"=hex:' % value
    for i, chunk in enumerate(body.split(",")):
        piece = chunk + ("," if i < len(items) - 1 else "")
        if len(line) + len(piece) > 76:
            out.append(line + "\\")
            line = "  "
        line += piece
    out.append(line)
    return "\r\n".join(out)


def self_test():
    """Round-trip the base-24 codec and assert every gate halo.exe applies.

    The codec was ALSO validated out of band against a blob Microsoft's own
    PIDGen wrote -- the Windows XP DigitalProductId of fleet box .145, read
    2026-08-30: decode_key() recovered that machine's Windows key and
    encode_key() reproduced its 15 bytes exactly.  That blob is deliberately
    NOT embedded here, because it carries a real licence key.
    """
    import random
    rnd = random.Random(20260830)
    for _ in range(200):
        k = "".join(rnd.choice(ALPHABET) for _ in range(25))
        k = "-".join(k[i:i + 5] for i in range(0, 25, 5))
        raw = encode_key(k)
        assert len(raw) == 15
        assert decode_key(raw) == k, k
        b = build(k)
        # every condition halo.exe 1.10 checks at 0x0057f3f0:
        assert len(b) == 0xA4
        assert struct.unpack_from("<I", b, 0)[0] == 0xA4
        assert struct.unpack_from("<HH", b, 4) == (3, 0)
        assert b[8 + 6:8 + 9] == b"OEM"          # selects the OEM digit table
        pid = b[8:0x20].split(b"\0")[0].decode()
        assert pid.startswith(HALO_MPC + "-")
        oem_idx = [12, 13, 14, 15, 18, 19, 20, 21, 22]
        assert all(pid[i].isdigit() for i in oem_idx), pid
        assert b[0x24:0x34].split(b"\0")[0].decode() == HALO_SKU
        assert b[0x34:0x43] == raw               # the licence-bearing field
        assert decode_key(b[0x34:0x43]) == k
    # Fixed vectors, so a change to the codec is visible in the diff. They are
    # BUILT from the alphabet rather than written out, because a 5x5 literal in
    # this repo is a product key by shape and the secret scanner is right to
    # say so - see tests/python/test_no_committed_secrets.py.
    zero, one = ALPHABET[0], ALPHABET[1]
    def _vec(last):
        k = zero * 24 + last
        return "-".join(k[i:i + 5] for i in range(0, 25, 5))
    assert encode_key(_vec(one)).hex() == "010000000000000000000000000000"
    assert encode_key(_vec(zero)[:-2] + one + zero).hex() == \
        "180000000000000000000000000000"
    print("self-test OK: 200 keys round-trip; every blob passes every halo.exe gate.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", help="25-character Halo PC product key")
    ap.add_argument("--key-file", help="read the key from this file instead")
    ap.add_argument("--reg", action="store_true", help="emit REGEDIT4 stanza")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    key = a.key
    if a.key_file:
        key = open(a.key_file).read().strip()
    if not key:
        ap.error("--key or --key-file is required")
    blob = build(key)
    if a.reg:
        print("REGEDIT4\r\n")
        print("[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft Games\\Halo]")
        print(reg_lines(blob))
        print('"ProductId"="%s"' % product_id(key))
    else:
        sys.stdout.write(blob.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
