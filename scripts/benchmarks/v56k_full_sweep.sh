#!/bin/bash
# The other-games sweep on host 2 (.124): every title the runner can drive, at
# 16 and 32 bit, one driver setting per clean boot, cfg 5 first (the default),
# then cfg 2 (the fastest), then cfg 0 (the floor). Detached and durable:
#
#   setsid nohup bash scripts/benchmarks/v56k_full_sweep.sh > scripts/benchmarks/results/v56k_titles_192.168.1.124/full_sweep.log 2>&1 < /dev/null &
#
# Titles and why:
#   quake3           - 32-bit is the open cell; 16-bit already published
#   quake2           - game-local 3dfxgl.dll (the runner labels what it really is)
#   glquake          - the oldest MiniGL path; refuses >1280x960 (declared)
#   ut99:glide       - native Glide; FullscreenColorBits=32 expected to be ignored (record it)
#   ut99:opengl      - OpenGLDrv over the AmigaMerlin ICD; the 32-bit route for UT
#   ut99:d3d         - D3DDrv over the AmigaMerlin D3D HAL
#   rtcw:openglv5    - WolfMP, wolfbench.dm_60; this GOG build ALWAYS loads its bundled
#                      Wicked3D gl/openglv5.dll (r_glDriver only latches; removing it
#                      wedges the box), so RtCW is measured on that 3dfx ICD, not AmigaMerlin's.
#                      No 1280x960 mode (declared).
#   serioussam, serioussam2 - Serious Engine, own demo
#   unrealgold:glide, deusex:glide - UE1 -benchmark route (may not exit; kept last)
#
# AA settings are NOT swept: they do not engage on this driver (dossier, retraction 0).
set -u
cd "$(dirname "$0")/../.."
OUT=scripts/benchmarks/results/v56k_titles_192.168.1.124
mkdir -p "$OUT"
# Measured on .124 2026-09-16 - only titles that CAN produce a number are swept.
#   ut99:opengl   GPF in UOpenGlRenderDevice::SetRes on AmigaMerlin (UE1 Critical Error modal)
#   serioussam(2) CD-locked: "CD check - Please insert the game CD", a library fix not a driver one
# ut99:d3d is kept: it is the other 32-bit candidate and now fails in seconds if it modals.
TITLES="quake3,quake2,glquake,ut99:glide,ut99:d3d,rtcw:openglv5"
RES="1600x1200,1280x960,1024x768,800x600,640x480"   # high to low: the CPU-bound cell last, well after boot
echo "[$(date +%H:%M:%S)] full sweep start: titles=$TITLES res=$RES depths=16,32 configs=5,2,0"
python3 scripts/benchmarks/v56k_sweep.py --host 192.168.1.124 --configs 5,2,0 \
  --titles "$TITLES" --resolutions "$RES" --depths 16,32 --attempts 3 --max-run 420 \
  --outdir "$OUT"
echo "[$(date +%H:%M:%S)] full sweep finished (rc=$?)"
# UE1 titles without a demo route go last, in their own pass, so a non-exiting
# -benchmark cannot hold the main matrix hostage.
python3 scripts/benchmarks/v56k_sweep.py --host 192.168.1.124 --configs 5 \
  --titles "unrealgold:glide,deusex:glide" --resolutions "1024x768,640x480" --depths 16,32 --attempts 1 --max-run 300 \
  --outdir "$OUT/ue1-extra"
echo "[$(date +%H:%M:%S)] ue1 extra pass finished (rc=$?)"
