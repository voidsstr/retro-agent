#!/usr/bin/env bash
# abswitch.sh on|off <evidence dir> <tag> : set/delete Diag\FlipDeadline on the 86Box guest, read it back, force a mode set
set -u
D=$(dirname "$0"); O=$2; T=$3
if [ "$1" = on ]; then
  python3 $D/ag.py 'REGWRITE HKLM SYSTEM\CurrentControlSet\Services\vcrmp\Diag FlipDeadline REG_DWORD 1' || exit 1
else
  python3 $D/ag.py 'EXEC reg delete "HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag" /v FlipDeadline /f' || exit 1
fi
python3 $D/ag.py 'REGREAD HKLM SYSTEM\CurrentControlSet\Services\vcrmp\Diag' | python3 -c 'import json,sys; j=json.load(sys.stdin); print("FlipDeadline:", [v for v in j["values"] if v["name"]=="FlipDeadline"] or "absent")' | tee $O/regread_$T.txt
python3 $D/ag.py 'EXEC C:\vcr\vcrctl.exe setmode 640 480 16' | tee $O/modeset_$T.txt
