"""Reviewer check: replay the hardware plan's FIXED keystroke counts on both
built QVMs in qvmsim (the builder's gate navigates by label, not by count),
with Texture Filter starting Bilinear AND Trilinear; then the Graphics
Settings presets and the Voodoo clamp, which the builder's gate never drove."""
import os, sys
P = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches/provisioning/patches/q3-baseq3-ui"
sys.path.insert(0, P)
import qvmsim as K, apply
B = os.path.expanduser("~/.retro-fleet/patch-out/q3-baseq3-ui/_build")
IOQ3 = "1920x1080 1680x1050 1600x1200 1400x1050 1280x1024 1280x960 1024x768 800x600 640x480"
LCD = {"r_mode": "-1", "r_customwidth": "1920", "r_customheight": "1080"}

def boot(kind, cvars, driver_type=0):
    d = os.path.join(B, kind, "code", "q3_ui", "vm")
    ui = K.UISim(open(d + "/ui.qvm", "rb").read(), open(d + "/ui.map").read(),
                 dict(apply._BASE_CVARS, **cvars), vid=(1920, 1080), driver_type=driver_type)
    ui.main(K.UI_SET_ACTIVE_MENU, K.UIMENU_MAIN); ui.refresh()
    return ui

def press(ui, k, n, log, name):
    for _ in range(n):
        ui.key(k, redraw=True)
    log.append("%s x%d -> %r" % (name, n, ui.focused()[1]))

def plan(kind, cvars, texmode):
    ui = boot(kind, dict(cvars, r_textureMode=texmode))
    log = ["start %r" % ui.focused()[1]]
    press(ui, K.K_DOWNARROW, 2, log, "DOWN"); press(ui, K.K_ENTER, 1, log, "RETURN")
    press(ui, K.K_DOWNARROW, 2, log, "DOWN"); press(ui, K.K_ENTER, 1, log, "RETURN")
    press(ui, K.K_DOWNARROW, 7, log, "DOWN")
    c, n, labels = ui.spin(ui.focused()[0]); log.append("Video Mode %r (%d of %d)" % (labels[c], c + 1, n))
    press(ui, K.K_DOWNARROW, 7, log, "DOWN")
    f = ui.focused()[0]; log.append("filter %r" % ui.spin(f)[2][ui.spin(f)[0]])
    press(ui, K.K_RIGHTARROW, 1, log, "RIGHT"); log.append("filter %r" % ui.spin(f)[2][ui.spin(f)[0]])
    press(ui, K.K_DOWNARROW, 3, log, "DOWN")
    it = ui.focused()[0]
    on_accept = ui.vm.rd32(it + K.MC_CALLBACK) == ui.syms["GraphicsOptions_ApplyChanges"]
    if on_accept:
        before = len(ui.eng.writes); ui.key(K.K_ENTER); w = dict(ui.eng.writes[before:])
        log.append("ACCEPT: r_mode=%s r_customwidth=%s r_customheight=%s r_glDriver=%s, then %r"
                   % (w.get("r_mode"), w.get("r_customwidth"), w.get("r_customheight"),
                      w.get("r_glDriver"), ui.eng.commands[-1]))
    else:
        log.append("NOT ON ACCEPT - RETURN here would apply NOTHING and no vid_restart would run")
    return log

for kind in ("stock", "patched"):
    for eng, cv in (("retail", LCD), ("ioquake3", dict(LCD, r_availableModes=IOQ3))):
        for tm in ("GL_LINEAR_MIPMAP_NEAREST", "GL_LINEAR_MIPMAP_LINEAR"):
            print("== %s / %s / r_textureMode %s" % (kind, eng, tm))
            for l in plan(kind, cv, tm):
                print("   " + l)

def accept(ui):
    before = len(ui.eng.writes)
    for _ in range(40):
        it = ui.focused()[0]
        if ui.vm.rd32(it + K.MC_CALLBACK) == ui.syms["GraphicsOptions_ApplyChanges"]:
            break
        ui.key(K.K_DOWNARROW)
    ui.key(K.K_ENTER)
    return dict(ui.eng.writes[before:])

def to_system(ui):
    for lab in ("SETUP", "SYSTEM"):
        ui.goto(lab); ui.key(K.K_ENTER, redraw=True)

print("\n== presets (Graphics Settings) and the Voodoo clamp")
for kind in ("stock", "patched"):
    for eng, cv in (("ioquake3", dict(LCD, r_availableModes=IOQ3)), ("retail", LCD)):
        for idx, pname in ((0, "High Quality"), (2, "Fast")):
            ui = boot(kind, cv); to_system(ui)
            it = ui.goto("Graphics Settings:"); c = ui.spin(it)[0]
            for _ in range(c): ui.key(K.K_LEFTARROW, redraw=True)
            for _ in range(idx): ui.key(K.K_RIGHTARROW, redraw=True)
            m = ui.goto("Video Mode:"); c2, n2, l2 = ui.spin(m)
            w = accept(ui)
            print("   %-7s %-8s preset %-12r -> Video Mode %r (%d of %d), ACCEPT r_mode=%s"
                  % (kind, eng, pname, l2[c2], c2 + 1, n2, w.get("r_mode")))
    ui = boot(kind, LCD); to_system(ui)
    d = ui.goto("GL Driver:"); ui.key(K.K_RIGHTARROW, redraw=True)
    m = ui.goto("Video Mode:"); before = ui.spin(m)[2][ui.spin(m)[0]]
    ui.key(K.K_RIGHTARROW, redraw=True); after = ui.spin(m)[2][ui.spin(m)[0]]
    w = accept(ui)
    print("   %-7s retail   GL Driver Voodoo, Video Mode %r -RIGHT-> %r (id's 3dfx clamp), ACCEPT r_mode=%s r_glDriver=%s"
          % (kind, before, after, w.get("r_mode"), w.get("r_glDriver")))
