#!/usr/bin/env python3
"""Which instruction sets does a staged title actually EXECUTE?

Run it over the staged library and read the result WITH SCHEMA.md's
"cpu_features - how to tell a real floor from a fast path" section beside
you. A raw count is the START of the answer and never the end: objdump
disassembles a PE's .text linearly, so padding, tables and any packed region
produce plausible-looking instructions that are not there. Check the cpuid
count, look for dispatch symbols (GIsMMX, Sys_GetProcessorFeatures), and READ
THE ACTUAL LINES before declaring a floor - refusing a title on a machine
that runs it is as damaging as approving one that crashes.


A Pentium 1 has FPU and, on a P55C, MMX. It has NO CMOV (Pentium Pro and
later), no SSE, no SSE2. Any of those in ordinary compiler-generated code is
#UD - an immediate hard crash, not a slow frame rate - and min_cpu_mhz will
never catch it. UT99 469e is the worked example: 1083 SSE2-class instructions
in Core.dll, misread as a broken GAMESYNC about thirty times.

Discriminating MMX from SSE matters and is done on the OPERANDS, not the
mnemonic: pxor/paddd/movq are MMX with mm0-7 and SSE2 with xmm0-15.
"""
import os
import re
import subprocess
import sys

GV = os.environ.get("LIB",
                    "/run/user/1000/gvfs/smb-share:server=192.168.1.122,"
                    "share=files,user=voidsstr/Files/Games-Library")

# Mnemonics that are unambiguous on their own.
CMOV = re.compile(r"\bcmov[a-z]{1,4}\b")
# SSE1 scalar/packed single + the SSE1 integer ops that DO take mm registers
SSE_ONLY = re.compile(r"\b(movaps|movups|movss|addps|addss|subps|subss|mulps|"
                      r"mulss|divps|divss|sqrtps|sqrtss|rsqrtps|rcpps|maxps|"
                      r"minps|maxss|minss|cmpps|cmpss|shufps|unpcklps|"
                      r"unpckhps|cvtsi2ss|cvtss2si|cvttss2si|cvtps2pi|"
                      r"cvtpi2ps|andps|orps|xorps|andnps|ldmxcsr|stmxcsr|"
                      r"movmskps|movhlps|movlhps|prefetchnta|prefetcht[012]|"
                      r"sfence|maskmovq)\b")
SSE2_ONLY = re.compile(r"\b(movapd|movupd|movsd|addpd|addsd|subpd|subsd|"
                       r"mulpd|mulsd|divpd|divsd|sqrtpd|sqrtsd|maxpd|minpd|"
                       r"maxsd|minsd|cmppd|cmpsd|shufpd|unpcklpd|unpckhpd|"
                       r"cvtsi2sd|cvtsd2si|cvttsd2si|cvtsd2ss|cvtss2sd|"
                       r"cvtdq2pd|cvtpd2dq|cvttpd2dq|cvtdq2ps|cvtps2dq|"
                       r"cvttps2dq|andpd|orpd|xorpd|andnpd|movdqa|movdqu|"
                       r"movdq2q|movq2dq|punpcklqdq|punpckhqdq|paddq|psubq|"
                       r"pmuludq|lfence|mfence|clflush|movntdq|movnti|"
                       r"movntpd|maskmovdqu|pshufd|pshuflw|pshufhw)\b")
SSE3 = re.compile(r"\b(addsubps|addsubpd|haddps|haddpd|hsubps|hsubpd|"
                  r"movsldup|movshdup|movddup|lddqu|fisttp|monitor|mwait)\b")
# The shared MMX/SSE2 integer mnemonics - classified by operand below.
SHARED_P = re.compile(r"\b(movq|movd|p(?:add|sub|cmp|unpck|and|or|xor|madd|"
                      r"mull|mulh|sll|srl|sra|ack|avg|max|min|sad|shuf|"
                      r"extr|insr|movmsk|abs)[a-z]*)\b")
XMM = re.compile(r"\bxmm\d")
MM = re.compile(r"\bmm[0-7]\b")
# MMX-only mnemonics (no SSE2 form)
MMX_ONLY = re.compile(r"\b(emms|femms|pmulhrw|pf[a-z]+|pi2fd|pf2id|pswapd)\b")


def scan(path):
    try:
        out = subprocess.run(
            ["objdump", "-d", "-M", "intel", "--no-show-raw-insn", path],
            capture_output=True, text=True, timeout=600, errors="replace")
    except Exception as exc:
        return {"error": str(exc)}
    if out.returncode != 0:
        return {"error": out.stderr.strip().split("\n")[0][:120]}
    c = {"cmov": 0, "mmx": 0, "sse": 0, "sse2": 0, "sse3": 0}
    for line in out.stdout.splitlines():
        if "\t" not in line:
            continue
        ins = line.split("\t", 1)[1]
        semi = ins.find("#")
        if semi >= 0:
            ins = ins[:semi]
        if CMOV.search(ins):
            c["cmov"] += 1
        if SSE3.search(ins):
            c["sse3"] += 1
        elif SSE2_ONLY.search(ins):
            c["sse2"] += 1
        elif SSE_ONLY.search(ins):
            # the SSE1 integer ops take mm registers on an MMX-capable CPU but
            # are still SSE1 instructions; either way a P54C cannot run them.
            c["sse"] += 1
        elif MMX_ONLY.search(ins):
            c["mmx"] += 1
        elif SHARED_P.search(ins):
            if XMM.search(ins):
                c["sse2"] += 1
            elif MM.search(ins):
                c["mmx"] += 1
        elif XMM.search(ins):
            c["sse"] += 1
    return c


def main():
    titles = sys.argv[1:]
    if not titles:
        titles = sorted(d for d in os.listdir(GV)
                        if not d.startswith("_")
                        and os.path.isdir(os.path.join(GV, d)))
    for t in titles:
        root = os.path.join(GV, t)
        # Everything the engine plausibly loads: exes and DLLs in the tree
        # root, plus a System/Bin subdirectory where those exist.
        cands = []
        for d in (root, os.path.join(root, "System"), os.path.join(root, "Bin"),
                  os.path.join(root, "bin")):
            if not os.path.isdir(d):
                continue
            for f in sorted(os.listdir(d)):
                if f.lower().endswith((".exe", ".dll")):
                    p = os.path.join(d, f)
                    try:
                        cands.append((os.path.getsize(p), p))
                    except OSError:
                        pass
        cands.sort(reverse=True)
        agg = {"cmov": 0, "mmx": 0, "sse": 0, "sse2": 0, "sse3": 0}
        worst = {}
        for _sz, p in cands[:14]:
            c = scan(p)
            if "error" in c:
                continue
            for k in agg:
                agg[k] += c[k]
                if c[k] and c[k] > worst.get(k, (0, ""))[0]:
                    worst[k] = (c[k], os.path.basename(p))
        detail = " ".join(f"{k}={agg[k]}({worst[k][1]})" if agg[k] else f"{k}=0"
                          for k in ("cmov", "mmx", "sse", "sse2", "sse3"))
        print(f"{t:<22} {detail}", flush=True)


main()
