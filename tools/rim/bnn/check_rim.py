#!/usr/bin/env python3
"""Run tools/rim/rim_dump.py verification on a .rim that uses the `bin` dtype.

FORMAT.md declares `bin` as a tensor dtype, but rim_common.DTYPES does not
define it yet, so the stock rim_dump CLI rejects it. Importing bnn_common
registers `bin` (as packed u8 bytes) in the shared DTYPES dict; this wrapper
then runs rim_dump's own dump/verify code unchanged.

    python3 check_rim.py [out/bnn-cifar10.rim]
"""
import sys
from pathlib import Path

import bnn_common as C  # registers 'bin' in rim_common.DTYPES
import rim_dump

path = sys.argv[1] if len(sys.argv) > 1 else str(C.OUT_DIR / "bnn-cifar10.rim")
ok = rim_dump.dump(path)
sys.exit(0 if ok else 1)
