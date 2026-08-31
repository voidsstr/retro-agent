#!/usr/bin/env python3
"""
Generate a title's resilient disc-mount launcher from the ONE canonical template.

WHY THIS EXISTS
---------------
"Play Red Faction.bat" is the fleet's mount launcher ("template v2") and it is
~300 lines of hard-won cmd.exe: it finds Daemon Tools OR WinCDEmu without a
hard-coded path, issues exactly ONE mount switch (spraying switches at Daemon
Tools 3.x raises a MODAL dialog that then blocks every later daemon.exe call),
waits for the drive letter, suppresses AutoPlay, kills the disc's autorun, and
reports "no mounter installed" and "a mounter ran but no drive appeared" as
DIFFERENT failures because they are different calls to action.

By 2026-08-31 that file had been hand-copied into three titles. A fourth and
fifth copy is how a fix lands in one launcher and not the others - so the
template is read from the source title and only the per-title parts are
substituted. Diff a generated file against the template and the ONLY changes
should be the ones this script makes.

The per-title parts are exactly five things:
  * the header title line                (cosmetic, but it names the file)
  * the FLEETRES block                   (per-engine resolution handling)
  * the six set "..." variables          (GTITLE IMAGE VOLID MARKER GAME GAMEARGS)
  * the autorun taskkill list            (what this disc's AUTORUN.INF starts)
  * an optional pre-launch step

USAGE
    make-mount-launcher.py --spec <spec.json> --out <Play Title.bat>
    make-mount-launcher.py --check <existing.bat> --spec <spec.json>

MARKER: pick a file that exists ONLY on this disc. AUTORUN.INF and INSTALL.EXE
are on essentially every game CD ever pressed; using one made the Descent II
launcher "find" a mounted StarCraft disc and start the game against it.
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPLATE = os.path.join(
    HERE, '..', '..', 'provisioning', 'discmount', 'mount-launcher-template.bat')

VAR_NAMES = ('GTITLE', 'IMAGE', 'VOLID', 'MARKER', 'GAME', 'GAMEARGS',
             'REQUIREDISC')


def load_template(path):
    with open(path, 'r', encoding='latin-1', newline='') as f:
        return f.read()


BANNER_END = 'rem @@TEMPLATE_BANNER_END@@'


def substitute(tpl, spec):
    # The template file carries a banner explaining that it IS the template.
    # That note belongs in the repo, not in every game tree, so it is stripped
    # here - which is also why a generated launcher round-trips byte-identical
    # against the hand-written one it was derived from.
    i = tpl.find(BANNER_END)
    if i < 0:
        raise SystemExit('template is missing its %s sentinel' % BANNER_END)
    out = tpl[i + len(BANNER_END):].lstrip('\r\n')

    # 1. header title line
    out = re.sub(r'(?m)^(rem  )@@TITLE@@( - resilient disc-image mount.*)$',
                 lambda m: m.group(1) + spec['title'] + m.group(2), out)

    # 2. FLEETRES block
    fr = spec.get('fleetres_block', '').rstrip('\r\n')
    if fr:
        out = out.replace('@@FLEETRES@@', fr)
    else:
        # Three staged launchers have no FLEETRES block at all. Substituting an
        # empty string would leave the blank lines that framed it, so the whole
        # framed region goes instead - a generated file must be byte-identical
        # to the hand-written one it replaces, or the round-trip test that is
        # the only real assurance here becomes untrustworthy.
        out = re.sub(r'\r?\n@@FLEETRES@@\r?\n\r?\n', '\r\n', out, count=1)
        out = out.replace('@@FLEETRES@@', '')

    # 3. the six variables
    for name in VAR_NAMES:
        # REQUIREDISC defaults to 1 (refuse rather than launch discless) - the
        # conservative half. A title that genuinely runs without its disc says
        # so explicitly in its spec.
        default = '1' if name == 'REQUIREDISC' else ''
        val = spec['vars'].get(name, default)
        out = out.replace('@@VAR_%s@@' % name, val)

    # 4. autorun kill list
    out = out.replace('@@AUTOKILL@@', spec.get('autokill', 'autorun.exe setup.exe'))

    # 5. optional pre-launch step
    pre = spec.get('prelaunch', 'rem (none)').rstrip('\r\n')
    out = out.replace('@@PRELAUNCH@@', pre)

    leftover = re.findall(r'@@[A-Z_]+@@', out)
    if leftover:
        raise SystemExit('template placeholders left unsubstituted: %s' % sorted(set(leftover)))
    return out


def crlf(text):
    """Windows .bat files: normalise to CRLF, exactly once."""
    return text.replace('\r\n', '\n').replace('\r', '\n').replace('\n', '\r\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spec', required=True)
    ap.add_argument('--template', default=DEFAULT_TEMPLATE)
    ap.add_argument('--out')
    ap.add_argument('--check', metavar='BAT',
                    help='compare an existing launcher against what the spec would generate')
    args = ap.parse_args()

    with open(args.spec) as f:
        spec = json.load(f)
    text = crlf(substitute(load_template(args.template), spec))

    if args.check:
        with open(args.check, 'r', encoding='latin-1', newline='') as f:
            have = f.read()
        if crlf(have) == text:
            print('OK: %s matches the template + spec' % args.check)
            return 0
        print('DIFFERS: %s is not what the template + spec generates' % args.check)
        return 1

    if not args.out:
        sys.stdout.write(text)
        return 0
    if '(' in os.path.basename(args.out) or ')' in os.path.basename(args.out):
        raise SystemExit('refusing: a generated filename must not contain ( or ) - '
                         'the agent cannot launch it (CLAUDE.md)')
    with open(args.out, 'w', encoding='latin-1', newline='') as f:
        f.write(text)
    print('wrote %s (%d bytes)' % (args.out, len(text)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
