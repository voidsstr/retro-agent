#!/usr/bin/env python3
# Assemble CACHE.COM - a real-mode DOS utility to toggle the CPU cache
# (CR0.CD / NW bits) to slow a fast Pentium down for speed-sensitive DOS
# games. Two-pass tiny assembler; .COM loads at CS:0100 (DS=CS).
import struct

ORG = 0x100
prog = []          # list of ('bytes', b'...') or ('rel8', label) or ('abs16', label)
labels = {}

def emit(b): prog.append(('b', bytes(b)))
def rel8(lbl): prog.append(('rel8', lbl))
def abs16(lbl): prog.append(('abs16', lbl))
def label(name): prog.append(('label', name))

def dollar(s): return s.encode('ascii') + b'\r\n$'

def build():
    global prog
    prog = []
    label('start')
    emit([0xBE, 0x81, 0x00])             # mov si,0x81
    label('skip')
    emit([0xAC])                         # lodsb
    emit([0x3C, 0x20]); je('skip')       # cmp al,' '; je skip
    emit([0x3C, 0x09]); je('skip')       # cmp al,9(TAB); je skip
    emit([0x3C, 0x0D]); je('status')     # cmp al,CR (empty)-> status
    emit([0x24, 0xDF])                   # and al,0xDF
    emit([0x3C, 0x4F]); jne('status')    # cmp al,'O'; jne status
    emit([0xAC])                         # lodsb (2nd char)
    emit([0x24, 0xDF])                   # and al,0xDF
    emit([0x3C, 0x4E]); je('do_on')      # cmp al,'N'; je do_on
    emit([0x3C, 0x46]); je('do_off')     # cmp al,'F'; je do_off
    je_always('status')                  # jmp status

    label('do_on')
    emit([0x0F, 0x20, 0xC0])             # mov eax,cr0
    emit([0x66, 0x25, 0xFF, 0xFF, 0xFF, 0x9F])  # and eax,0x9FFFFFFF (clr CD,NW)
    emit([0x0F, 0x22, 0xC0])             # mov cr0,eax
    emit([0x0F, 0x09])                   # wbinvd
    movdx('m_on'); je_always('print1')

    label('do_off')
    emit([0x0F, 0x20, 0xC0])             # mov eax,cr0
    emit([0x66, 0x0D, 0x00, 0x00, 0x00, 0x60])  # or eax,0x60000000 (set CD,NW)
    emit([0x0F, 0x22, 0xC0])             # mov cr0,eax
    emit([0x0F, 0x09])                   # wbinvd
    movdx('m_off'); je_always('print1')

    label('status')
    emit([0x0F, 0x20, 0xC0])             # mov eax,cr0
    emit([0x66, 0xA9, 0x00, 0x00, 0x00, 0x40])  # test eax,0x40000000 (CD)
    jnz('st_off')                        # if CD set -> currently OFF
    movdx('m_ison'); prnt(); movdx('m_usage'); je_always('print1')
    label('st_off')
    movdx('m_isoff'); prnt(); movdx('m_usage')

    label('print1')
    prnt()                               # print DS:DX ($-string)
    emit([0xB8, 0x00, 0x4C])             # mov ax,0x4C00
    emit([0xCD, 0x21])                   # int 21h  (exit)

    # ---- data ----
    label('m_on');    emit(dollar("L1/L2 cache ENABLED - full speed"))
    label('m_off');   emit(dollar("L1/L2 cache DISABLED - SLOW mode for old games"))
    label('m_ison');  emit(dollar("Cache is currently ON (full speed)"))
    label('m_isoff'); emit(dollar("Cache is currently OFF (slowed down)"))
    label('m_usage'); emit(dollar("Usage: CACHE ON | OFF   (must run in MS-DOS mode)"))

def je(lbl):   emit([0x74]); prog.append(('rel8', lbl))
def jne(lbl):  emit([0x75]); prog.append(('rel8', lbl))
def jnz(lbl):  emit([0x75]); prog.append(('rel8', lbl))
def je_always(lbl): emit([0xEB]); prog.append(('rel8', lbl))
def movdx(lbl): emit([0xBA]); prog.append(('abs16', lbl))
def prnt(): emit([0xB4, 0x09, 0xCD, 0x21])  # mov ah,9; int21h

# assemble: pass 1 = addresses, pass 2 = bytes
def assemble():
    for _ in range(3):  # iterate to stabilize (all refs are fixed-size)
        addr = ORG
        labels.clear()
        for kind, val in prog:
            if kind == 'label': labels[val] = addr
            elif kind == 'b': addr += len(val)
            elif kind == 'rel8': addr += 1
            elif kind == 'abs16': addr += 2
    out = bytearray()
    cur = ORG
    for kind, val in prog:
        if kind == 'label': continue
        if kind == 'b':
            out += val; cur += len(val)
        elif kind == 'rel8':
            tgt = labels[val]; disp = tgt - (cur + 1)
            assert -128 <= disp <= 127, (val, disp)
            out += struct.pack('b', disp); cur += 1
        elif kind == 'abs16':
            out += struct.pack('<H', labels[val]); cur += 2
    return bytes(out)

build()
com = assemble()
open('/tmp/CACHE.COM', 'wb').write(com)
print(f"CACHE.COM = {len(com)} bytes")
print("labels:", {k: hex(v) for k, v in sorted(labels.items(), key=lambda x: x[1])})
