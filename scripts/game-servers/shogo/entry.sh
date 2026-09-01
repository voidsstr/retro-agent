#!/bin/bash
# ONE-TIME: drive the Shogo Server Wizard so ShogoSrv.exe -go has settings to
# start from.  ShogoSrv.txt: "-go ... immediately run the game server using the
# PREVIOUS settings.  If there aren't any previous settings to use, the dialogs
# will still come up."  With none, the wizard sits there forever and the
# container looks perfectly healthy while nothing is hosting.
#
# Coordinates are screen coordinates on a 1024x768 Xvfb with the 473x276 wizard
# at 275,255 - i.e. window-relative x+275, y+255.
export WINEPREFIX=${WINEPREFIX:-/wp} WINEDEBUG=-all
wineboot -i >/dev/null 2>&1
cd /game || exit 1
wine ShogoSrv.exe -go &
for i in $(seq 1 30); do
  W=$(xdotool search --name "Shogo Server Wizard" 2>/dev/null | head -1)
  [ -n "$W" ] && break
  sleep 2
done
[ -z "$W" ] && { echo "[setup] FAILED: wizard never appeared"; exit 1; }
echo "[setup] wizard window=$W"
sleep 3
xdotool windowfocus "$W" 2>/dev/null; sleep 1

# --- page 1: networking service + broadcast options ---------------------
# *** DO NOT UNTICK "Communicate with GameSpy". ***
# It reads like a dead master uplink and it is not: it is the switch for the
# server's own GameSpy QUERY RESPONDER. With it off, ShogoSrv binds UDP 27888
# and answers nothing at all -- a listening socket on a server no browser can
# see, which is the same shape as the Red Faction trap. Measured 2026-09-01:
# unticked, every query form timed out; ticked, `\status\` answers on 27888.
# Only the Shogo web-site registry is unticked - that host really is gone.
xdotool mousemove 386 451 click 1; sleep 1     # [ ] Register with Shogo web site
xdotool mousemove 554 508 click 1; sleep 3     # Next >

# --- page 2: server name, frag limit, players, port ---------------------
xdotool mousemove 547 321 click 1; sleep 1
# Clear the field the only way that works here. A triple-click does NOT select
# the text in this classic Win32 EDIT control under Wine, and neither does
# ctrl+a (that is a modern shell affordance the control never implemented) --
# both leave the default in place and the typed name is APPENDED, so the
# server advertises itself as "Shogo ServerNSC Retro Fleet Arena - Shogo".
# End, then backspace over it.
xdotool key --clearmodifiers End; sleep 1
xdotool key --clearmodifiers --repeat 40 --repeat-delay 30 BackSpace; sleep 1
xdotool type --delay 40 "NSC Retro Fleet Arena - Shogo"; sleep 1
xdotool mousemove 554 508 click 1; sleep 3     # Next >

# --- page 3: gameplay toggles - defaults are fine -----------------------
xdotool mousemove 554 508 click 1; sleep 3     # Next >

# --- page 4: level list.  Finish stays GREYED until a level is added. ---
for i in 1 2 3 4 5 6 7 8; do
  xdotool mousemove 400 320 click 1; sleep 1   # first row of Retail Levels
  xdotool mousemove 554 346 click 1; sleep 1   # Add >
  # After an Add the list keeps focus; step down one and repeat.
  xdotool key Down; sleep 1
done
xdotool mousemove 601 463 click 1; sleep 1     # [x] Save levels
xwd -root -silent > /game/_run/before-finish.xwd 2>/dev/null
xdotool mousemove 554 508 click 1; sleep 5     # Finish

# ONE MORE MODAL, and it is the reason the wizard's answers never persisted:
# after Finish ShogoSrv puts up an info box ("Power users can specify the -go
# command-line parameter...") and does nothing further until it is dismissed.
# The window list at that moment reads "Shogo Server", which looks exactly
# like a running server -- while UDP 27888 is unbound.
for i in $(seq 1 15); do
  OK=$(xdotool search --name "Shogo Server" 2>/dev/null | tail -1)
  [ -n "$OK" ] && break
  sleep 1
done
xdotool mousemove 516 435 click 1; sleep 3     # OK
xwd -root -silent > /game/_run/after-finish.xwd 2>/dev/null
echo "[setup] windows after Finish:"
for w in $(xdotool search --name "." 2>/dev/null); do n=$(xdotool getwindowname $w 2>/dev/null); [ -n "$n" ] && echo "   $n"; done
sleep 25
echo "[setup] done"
echo "[setup] listening sockets inside the container:"
ss -lnu 2>/dev/null | grep -E '2788|:[0-9]+' | head -10 || netstat -lnu 2>/dev/null | head -10

# The wizard's answers are NOT persisted: ShogoSrv commits them only on a
# clean exit, and a service is killed rather than closed.  So the wizard is
# driven on EVERY start.  That is ugly and it is also the difference between
# "a person has to click through a dialog after every reboot" and a server
# that comes back on its own.
echo "[entry] wizard driven; blocking on wineserver"
wineserver -w
echo "[entry] wineserver -w returned $?"
