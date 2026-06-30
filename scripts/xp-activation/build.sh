#!/usr/bin/env bash
# Build the offline XP Confirmation ID generator (Linux/macOS, GCC or Clang).
set -euo pipefail
cd "$(dirname "$0")"
CC="${CC:-cc}"
"$CC" -O2 -o xpcid xpcid.c
echo "Built ./xpcid"
echo "Usage: ./xpcid <installation-id>   # 54 digits, spaces/dashes ignored"
