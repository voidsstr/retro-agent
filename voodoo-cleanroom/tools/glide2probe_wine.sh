#!/usr/bin/env bash
# glide2probe_wine.sh - check glide2probe's HARNESS off the box: under Wine 8
# (the local retro-wine:bookworm image) against tools/glide2stub.c, a fake
# glide2x.dll. It proves the call sequence, the re-open inside WM_KILLFOCUS,
# the pace gate and the watchdog (stack dump, hard timeout) - nothing about a
# driver or about XP's DirectDraw.
#
#   bash voodoo-cleanroom/tools/glide2probe_wine.sh [outdir]
#
# Builds out/glide2probe.exe and out/glide2stub/glide2x.dll first. Exit 0 =
# every scenario's RESULT matched what it must say.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/out/glide2probe_wine}"
CROSS=i686-w64-mingw32-
IMAGE=retro-wine:bookworm
mkdir -p "$OUT" "$HERE/out/glide2stub"
${CROSS}gcc -O1 -march=i686 -Wall -Wno-unused-function -o "$HERE/out/glide2probe.exe" \
    "$HERE/tools/glide2probe.c" -lgdi32 -luser32
${CROSS}gcc -shared -O1 -Wall -o "$HERE/out/glide2stub/glide2x.dll" "$HERE/tools/glide2stub.c" \
    -Wl,--add-stdcall-alias
cp "$HERE/out/glide2probe.exe" "$HERE/out/glide2stub/glide2x.dll" "$OUT/"
rm -f "$OUT"/*.log "$OUT"/*.stub

cat > "$OUT/run.sh" <<'EOS'
#!/bin/sh
# inside the container: /w is the scenario directory
export WINEDEBUG=-all WINEPREFIX=/tmp/wp WINEDLLOVERRIDES="mscoree,mshtml="
Xvfb :7 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
export DISPLAY=:7
sleep 2
timeout 180 wineboot -i >/dev/null 2>&1 || echo "wineboot rc=$?"
cd /w
run() {  # name, env..., -- args...
  name=$1; shift
  envs=""
  while [ "$1" != "--" ]; do envs="$envs $1"; shift; done
  shift
  env $envs GLIDE2STUB_LOG="Z:\\w\\$name.stub" \
    timeout 120 wine glide2probe.exe --dll glide2x.dll --log "Z:\\w\\$name.log" "$@" \
    >/dev/null 2>&1
  echo "$name rc=$?"
}
run plain -- --frames 5
run seq -- --reopen 8:0:2:1 --hold 1 --frames 5
run ue1_same -- --ue1 --killfocus same --hold 3 --frames 5
run ue1_thread -- --ue1 --killfocus thread --hold 5 --frames 5
run ue1_natural -- --ue1 --hold 2 --frames 5
run hang GLIDE2STUB_HANG_OPEN=2 -- --ue1 --killfocus same --hold 3 --frames 5 --timeout 4 --hard-timeout 25
run spin GLIDE2STUB_SPIN_OPEN=2 -- --reopen 8:0:2:1 --hold 1 --frames 5 --timeout 4 --hard-timeout 25
EOS
chmod +x "$OUT/run.sh"
timeout 900 docker run --rm --init -v "$OUT:/w" "$IMAGE" /w/run.sh | tee "$OUT/rc.txt"

fail=0
check() {  # name, python expression over r (the RESULT dict) and log (the text)
  python3 - "$OUT/$1.log" "$1" "$2" <<'PY' || fail=1
import json, sys
path, name, expr = sys.argv[1:4]
try:
    log = open(path, encoding="latin-1").read()
except OSError:
    print(f"FAIL {name}: no log"); sys.exit(1)
res = [l.split("RESULT ", 1)[1] for l in log.splitlines() if "RESULT " in l]
r = json.loads(res[-1]) if res else {}
ok = eval(expr)
print(("ok   " if ok else "FAIL ") + name + ": " + (res[-1] if res else "(no RESULT)"))
sys.exit(0 if ok else 1)
PY
}
check plain       'r.get("opens_ok") == 1 and not r.get("hung")'
check seq         'r.get("opens_ok") == 2 and "UE1 re-Init" in log'
check ue1_same    'r.get("reopen_in_killfocus") is True and r.get("opens_ok") == 2 and "inside WM_KILLFOCUS" in log and log.index("step: ShowWindow + SetFocus(thief)") < log.index("step: WM_KILLFOCUS handler")'
check ue1_thread  'r.get("opens_ok") == 2'
check ue1_natural 'r.get("opens_ok") == 2 and r.get("killfocus") == "natural"'
check hang        'r.get("hung") is True and r.get("killed") is True and "EIP" in log and "grSstWinOpen" in log'
check spin        'r.get("hung") is True and r.get("killed") is True and "glide2x" in log.split("HUNG",1)[-1]'
exit $fail
