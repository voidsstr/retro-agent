#!/usr/bin/env python3
"""86box-key.py KEY [KEY...] [--display :21] - type keys into the 86Box window via XTEST.

KEY is an X keysym name (F1, Return, Down, Escape, a, ...) or 'text:<string>'.
The pointer is parked over the emulated screen first (no WM: focus follows it).
"""
import ctypes, sys, time
disp = ':21'
keys = []
it = iter(sys.argv[1:])
for a in it:
    if a == '--display':
        disp = next(it)
    else:
        keys.append(a)
X = ctypes.CDLL('libX11.so.6')
T = ctypes.CDLL('libXtst.so.6')
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XStringToKeysym.restype = ctypes.c_ulong
X.XStringToKeysym.argtypes = [ctypes.c_char_p]
X.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XFlush.argtypes = [ctypes.c_void_p]
T.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
d = X.XOpenDisplay(disp.encode())
if not d:
    sys.exit('cannot open display ' + disp)
T.XTestFakeMotionEvent(d, -1, 250, 250, 0)
X.XFlush(d)
time.sleep(0.2)

def tap(sym, shift=False):
    kc = X.XKeysymToKeycode(d, X.XStringToKeysym(sym.encode()))
    if not kc:
        sys.exit('no keycode for ' + sym)
    sk = X.XKeysymToKeycode(d, X.XStringToKeysym(b'Shift_L'))
    if shift:
        T.XTestFakeKeyEvent(d, sk, 1, 0)
    T.XTestFakeKeyEvent(d, kc, 1, 0)
    X.XFlush(d)
    time.sleep(0.08)
    T.XTestFakeKeyEvent(d, kc, 0, 0)
    if shift:
        T.XTestFakeKeyEvent(d, sk, 0, 0)
    X.XFlush(d)
    time.sleep(0.12)

SYM = {' ': 'space', '\\': 'backslash', ':': ('colon', True), '.': 'period', '-': 'minus',
       '_': ('underscore', True), '/': 'slash', '"': ('quotedbl', True)}
for k in keys:
    if k.startswith('text:'):
        for ch in k[5:]:
            s = SYM.get(ch, ch)
            if isinstance(s, tuple):
                tap(*s)
            else:
                tap(s.lower() if len(s) == 1 else s, len(s) == 1 and s.isupper())
    else:
        tap(k)
print('sent', keys)
