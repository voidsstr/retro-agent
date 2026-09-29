#!/usr/bin/env python3
"""A Quake III VM interpreter with a stub UI engine - enough to drive a real
baseq3 ui.qvm through Main > Setup > System by keystrokes and record what
ACCEPT writes, with no game, no hardware and no Windows.

WHY: the question this patch answers is behavioural - "what does the Video
Mode menu show, and what does ACCEPT write, on retail 1.32c and on ioquake3?"
The hardware answer for the STOCK pak8 QVM was measured on four boxes
(label "640x480", ACCEPT writes r_mode 3). Running the stock QVM here must
reproduce exactly that; running the patched QVM through the same keystrokes
shows the fix. apply.py --build uses it as its behaviour gate.

The opcode semantics follow id's qcommon/vm_interpreted.c (1.32 GPL release)
with its one known interpreter slip corrected (OP_BCOM writes opStack[-1]
there; the x86 JIT the games actually run complements the top of stack).
Branch operands and CONST call/jump targets are instruction indices, as
q3asm writes them. Memory is the image's data+lit+bss rounded up to a power
of two, the program stack at its top, exactly as VM_Create lays it out.

Engine stubs: cvars (a dict, with every write recorded), glconfig
(vidWidth/vidHeight/driverType), the key catcher, CD key (valid), and inert
answers for files, rendering, sound, LAN and cinematics. Only what the UI
needs to boot and draw its menus.
"""
import math
import struct

# --- opcodes (qcommon/vm_local.h) ---------------------------------------------
(OP_UNDEF, OP_IGNORE, OP_BREAK, OP_ENTER, OP_LEAVE, OP_CALL, OP_PUSH, OP_POP,
 OP_CONST, OP_LOCAL, OP_JUMP, OP_EQ, OP_NE, OP_LTI, OP_LEI, OP_GTI, OP_GEI,
 OP_LTU, OP_LEU, OP_GTU, OP_GEU, OP_EQF, OP_NEF, OP_LTF, OP_LEF, OP_GTF, OP_GEF,
 OP_LOAD1, OP_LOAD2, OP_LOAD4, OP_STORE1, OP_STORE2, OP_STORE4, OP_ARG,
 OP_BLOCK_COPY, OP_SEX8, OP_SEX16, OP_NEGI, OP_ADD, OP_SUB, OP_DIVI, OP_DIVU,
 OP_MODI, OP_MODU, OP_MULI, OP_MULU, OP_BAND, OP_BOR, OP_BXOR, OP_BCOM, OP_LSH,
 OP_RSHI, OP_RSHU, OP_NEGF, OP_ADDF, OP_SUBF, OP_DIVF, OP_MULF, OP_CVIF,
 OP_CVFI) = range(60)
_OPERAND4 = {OP_ENTER, OP_LEAVE, OP_CONST, OP_LOCAL, OP_BLOCK_COPY} | set(range(OP_EQ, OP_GEF + 1))
VM_MAGIC = 0x12721444

# --- the UI API (ui/ui_public.h, UI_API_VERSION 6) -----------------------------
UI_GETAPIVERSION, UI_INIT, UI_SHUTDOWN, UI_KEY_EVENT, UI_MOUSE_EVENT, UI_REFRESH, \
    UI_IS_FULLSCREEN, UI_SET_ACTIVE_MENU = range(8)
UIMENU_MAIN = 1
KEYCATCH_UI = 2
K_ENTER, K_ESCAPE, K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW = 13, 27, 132, 133, 134, 135
# glconfig_t (cgame/tr_types.h): four strings of 1024/1024/1024/8192 bytes, then ints
GLC_DRIVERTYPE, GLC_HARDWARETYPE, GLC_VIDWIDTH, GLC_VIDHEIGHT, GLC_ASPECT, GLC_FULLSCREEN = \
    11284, 11288, 11304, 11308, 11312, 11320
GLCONFIG_SIZE = 11332
# struct offsets (q3_ui/ui_local.h), all 4-byte fields
UIS_ACTIVEMENU = 20                  # uiStatic_t: frametime realtime cursorx cursory menusp activemenu
MF_CURSOR, MF_NITEMS, MF_ITEMS = 0, 8, 12   # menuframework_s
MC_TYPE, MC_NAME, MC_FLAGS, MC_CALLBACK = 0, 4, 44, 48   # menucommon_s (60 bytes)
ML_CURVALUE, ML_NUMITEMS, ML_ITEMNAMES = 64, 68, 76      # menulist_s after its menucommon_s
MT_STRING = 60                       # menutext_s: generic, string
MTYPE_BITMAP = 6


class VMError(RuntimeError):
    pass


def s32(x):
    x &= 0xffffffff
    return x - 0x100000000 if x & 0x80000000 else x


def i2f(i):
    return struct.unpack("<f", struct.pack("<I", i & 0xffffffff))[0]


def f2i(f):
    try:
        return struct.unpack("<i", struct.pack("<f", f))[0]
    except OverflowError:
        return struct.unpack("<i", struct.pack("<f", math.copysign(math.inf, f)))[0]


def cvt_fi(f):
    """C (int)float on x86 (cvttss2si): truncate; NaN/out of range -> 0x80000000."""
    if f != f or f >= 2147483648.0 or f < -2147483648.0:
        return -0x80000000
    return int(f)


class QVM:
    def __init__(self, data):
        (magic, ic, co, cl, do, dl, ll, bl) = struct.unpack_from("<8i", data, 0)
        if magic & 0xffffffff != VM_MAGIC:
            raise VMError("not an original-format QVM (magic 0x%08x)" % (magic & 0xffffffff))
        ops, args = [], []
        code, pc = data[co:co + cl], 0
        while pc < len(code):
            op = code[pc]
            pc += 1
            if op in _OPERAND4:
                args.append(struct.unpack_from("<i", code, pc)[0])
                pc += 4
            elif op == OP_ARG:
                args.append(code[pc])
                pc += 1
            else:
                args.append(0)
            ops.append(op)
        if len(ops) != ic:
            raise VMError("decoded %d instructions, header says %d" % (len(ops), ic))
        size = 1
        while size < dl + ll + bl:
            size <<= 1
        self.mem = bytearray(size)
        self.mem[0:dl + ll] = data[do:do + dl + ll]
        self.mask = size - 1
        self.stack_top = size
        self.ops, self.args = ops, args
        self.data_len, self.lit_len = dl, ll
        self.steps = 0

    # memory helpers ---------------------------------------------------------
    def rd32(self, a):
        return struct.unpack_from("<i", self.mem, a & self.mask)[0]

    def wr32(self, a, v):
        struct.pack_into("<i", self.mem, a & self.mask & ~3, s32(v))

    def rdstr(self, a, limit=4096):
        a &= self.mask
        end = self.mem.find(b"\0", a, a + limit)
        return self.mem[a:end if end >= 0 else a + limit].decode("latin-1")

    def wrstr(self, a, s, size):
        """Q_strncpyz into VM memory: at most size-1 chars, always terminated."""
        if size <= 0:
            return
        b = s.encode("latin-1")[:size - 1] + b"\0"
        a &= self.mask
        self.mem[a:a + len(b)] = b

    # execution --------------------------------------------------------------
    def call(self, entry, args, syscall, max_steps=50000000):
        mem, mask, ops, argv = self.mem, self.mask, self.ops, self.args
        saved_top = self.stack_top
        ps = saved_top - 48
        for i, v in enumerate(list(args)[:10]):
            struct.pack_into("<i", mem, ps + 8 + 4 * i, s32(v))
        struct.pack_into("<i", mem, ps + 4, 0)
        struct.pack_into("<i", mem, ps, -1)
        st = []
        pc = entry
        unpack, pack = struct.unpack_from, struct.pack_into
        steps = 0
        try:
            while True:
                op = ops[pc]
                a = argv[pc]
                pc += 1
                steps += 1
                if steps > max_steps:
                    raise VMError("step limit")
                if op == OP_CONST:
                    st.append(a)
                elif op == OP_LOCAL:
                    st.append(ps + a)
                elif op == OP_LOAD4:
                    st[-1] = unpack("<i", mem, st[-1] & mask)[0]
                elif op == OP_STORE4:
                    v = st.pop()
                    pack("<i", mem, st.pop() & mask & ~3, s32(v))
                elif op == OP_ARG:
                    pack("<i", mem, (ps + a) & mask, s32(st.pop()))
                elif op == OP_ADD:
                    v = st.pop()
                    st[-1] = s32(st[-1] + v)
                elif op == OP_SUB:
                    v = st.pop()
                    st[-1] = s32(st[-1] - v)
                elif op == OP_CALL:
                    pack("<i", mem, ps & mask, pc)
                    target = st.pop()
                    if target < 0:
                        pack("<i", mem, (ps + 4) & mask, -1 - target)
                        save = self.stack_top
                        self.stack_top = ps - 4
                        r = syscall(self, -1 - target, ps + 4)
                        self.stack_top = save
                        st.append(s32(r or 0))
                        pc = unpack("<i", mem, ps & mask)[0]
                    else:
                        pc = target
                elif op == OP_ENTER:
                    ps -= a
                elif op == OP_LEAVE:
                    ps += a
                    pc = unpack("<i", mem, ps & mask)[0]
                    if pc == -1:
                        return st[-1] if st else 0
                elif op == OP_JUMP:
                    pc = st.pop()
                elif OP_EQ <= op <= OP_GEU:
                    r0 = st.pop()
                    r1 = st.pop()
                    if op == OP_EQ:
                        t = r1 == r0
                    elif op == OP_NE:
                        t = r1 != r0
                    elif op == OP_LTI:
                        t = r1 < r0
                    elif op == OP_LEI:
                        t = r1 <= r0
                    elif op == OP_GTI:
                        t = r1 > r0
                    elif op == OP_GEI:
                        t = r1 >= r0
                    else:
                        u1, u0 = r1 & 0xffffffff, r0 & 0xffffffff
                        t = (u1 < u0, u1 <= u0, u1 > u0, u1 >= u0)[op - OP_LTU]
                    if t:
                        pc = a
                elif OP_EQF <= op <= OP_GEF:
                    f0 = i2f(st.pop())
                    f1 = i2f(st.pop())
                    t = (f1 == f0, f1 != f0, f1 < f0, f1 <= f0, f1 > f0, f1 >= f0)[op - OP_EQF]
                    if t:
                        pc = a
                elif op == OP_LOAD1:
                    st[-1] = mem[st[-1] & mask]
                elif op == OP_LOAD2:
                    st[-1] = unpack("<H", mem, st[-1] & mask)[0]
                elif op == OP_STORE1:
                    v = st.pop()
                    mem[st.pop() & mask] = v & 0xff
                elif op == OP_STORE2:
                    v = st.pop()
                    pack("<H", mem, st.pop() & mask & ~1, v & 0xffff)
                elif op == OP_BLOCK_COPY:
                    src = st.pop() & mask
                    dst = st.pop() & mask
                    mem[dst:dst + a] = mem[src:src + a]
                elif op == OP_PUSH:
                    st.append(0)
                elif op == OP_POP:
                    st.pop()
                elif op == OP_SEX8:
                    st[-1] = ((st[-1] & 0xff) ^ 0x80) - 0x80
                elif op == OP_SEX16:
                    st[-1] = ((st[-1] & 0xffff) ^ 0x8000) - 0x8000
                elif op == OP_NEGI:
                    st[-1] = s32(-st[-1])
                elif op in (OP_DIVI, OP_MODI):
                    b = st.pop()
                    x = st[-1]
                    if b == 0:
                        raise VMError("integer divide by zero at %d" % (pc - 1))
                    q = abs(x) // abs(b)
                    q = q if (x >= 0) == (b >= 0) else -q
                    st[-1] = s32(q if op == OP_DIVI else x - b * q)
                elif op in (OP_DIVU, OP_MODU):
                    b = st.pop() & 0xffffffff
                    x = st[-1] & 0xffffffff
                    if b == 0:
                        raise VMError("integer divide by zero at %d" % (pc - 1))
                    st[-1] = s32(x // b if op == OP_DIVU else x % b)
                elif op in (OP_MULI, OP_MULU):
                    v = st.pop()
                    st[-1] = s32(st[-1] * v)
                elif op == OP_BAND:
                    v = st.pop()
                    st[-1] = s32(st[-1] & v)
                elif op == OP_BOR:
                    v = st.pop()
                    st[-1] = s32(st[-1] | v)
                elif op == OP_BXOR:
                    v = st.pop()
                    st[-1] = s32(st[-1] ^ v)
                elif op == OP_BCOM:
                    st[-1] = s32(~st[-1])
                elif op == OP_LSH:
                    v = st.pop()
                    st[-1] = s32(st[-1] << (v & 31))
                elif op == OP_RSHI:
                    v = st.pop()
                    st[-1] = st[-1] >> (v & 31)
                elif op == OP_RSHU:
                    v = st.pop()
                    st[-1] = s32((st[-1] & 0xffffffff) >> (v & 31))
                elif op in (OP_ADDF, OP_SUBF, OP_DIVF, OP_MULF):
                    b = i2f(st.pop())
                    x = i2f(st[-1])
                    if op == OP_ADDF:
                        r = x + b
                    elif op == OP_SUBF:
                        r = x - b
                    elif op == OP_MULF:
                        r = x * b
                    elif b == 0.0:
                        r = math.nan if x == 0.0 or x != x else math.copysign(math.inf, x) * math.copysign(1.0, b)
                    else:
                        r = x / b
                    st[-1] = f2i(r)
                elif op == OP_NEGF:
                    st[-1] = f2i(-i2f(st[-1]))
                elif op == OP_CVIF:
                    st[-1] = f2i(float(st[-1]))
                elif op == OP_CVFI:
                    st[-1] = cvt_fi(i2f(st[-1]))
                elif op == OP_IGNORE:
                    pass
                else:
                    raise VMError("opcode %d at %d" % (op, pc - 1))
        finally:
            self.steps += steps
            self.stack_top = saved_top


class UIEngine:
    """The engine side of the UI API, reduced to what q3_ui needs to boot,
    draw and change video settings. Every cvar write is recorded in order."""

    def __init__(self, cvars, vid=(1920, 1080), driver_type=0, hardware_type=0):
        self.cvars = {k: str(v) for k, v in cvars.items()}
        self.writes = []                 # (name, value) in call order
        self.commands = []               # Cmd_ExecuteText
        self.prints = []
        self.vid = vid
        self.driver_type, self.hardware_type = driver_type, hardware_type
        self.catcher = 0
        self.handles = []                # vmCvar handle -> name

    def _fill_vmcvar(self, vm, addr, name):
        s = self.cvars.get(name, "")
        try:
            val = float(s)
        except ValueError:
            val = 0.0
        vm.wr32(addr + 4, 1)
        vm.wr32(addr + 8, f2i(val))
        vm.wr32(addr + 12, cvt_fi(val) if s else 0)
        vm.wrstr(addr + 16, s, 256)

    def __call__(self, vm, num, base):
        A = lambda i: vm.rd32(base + 4 * i)      # A(0) is the call number
        if num == 0:                             # UI_ERROR
            raise VMError("trap_Error: " + vm.rdstr(A(1)))
        if num == 1:                             # UI_PRINT
            self.prints.append(vm.rdstr(A(1)))
            return 0
        if num in (2, 64):                       # UI_MILLISECONDS / UI_REAL_TIME
            return 1000
        if num == 3:                             # UI_CVAR_SET
            name, val = vm.rdstr(A(1)), vm.rdstr(A(2))
            self.cvars[name] = val
            self.writes.append((name, val))
            return 0
        if num == 4:                             # UI_CVAR_VARIABLEVALUE
            try:
                return f2i(float(self.cvars.get(vm.rdstr(A(1)), "0") or "0"))
            except ValueError:
                return f2i(0.0)
        if num == 5:                             # UI_CVAR_VARIABLESTRINGBUFFER
            vm.wrstr(A(2), self.cvars.get(vm.rdstr(A(1)), ""), A(3))
            return 0
        if num == 6:                             # UI_CVAR_SETVALUE (Cvar_SetValue: "%i" if integral)
            name, f = vm.rdstr(A(1)), i2f(A(2))
            val = "%d" % int(f) if f == int(f) else "%f" % f
            self.cvars[name] = val
            self.writes.append((name, val))
            return 0
        if num in (7, 8):                        # UI_CVAR_RESET / UI_CVAR_CREATE
            if num == 8:
                self.cvars.setdefault(vm.rdstr(A(1)), vm.rdstr(A(2)))
            return 0
        if num == 9:                             # UI_CVAR_INFOSTRINGBUFFER
            vm.wrstr(A(2), "", A(3))
            return 0
        if num == 10:                            # UI_ARGC
            return 0
        if num in (11, 33, 34, 42, 45):          # ARGV, KEYNUM/BINDING bufs, CLIPBOARD, CONFIGSTRING
            buf, size = (A(1), A(2)) if num == 42 else (A(2), A(3))
            vm.wrstr(buf, "", size)
            return 0
        if num == 12:                            # UI_CMD_EXECUTETEXT
            self.commands.append(vm.rdstr(A(2)))
            return 0
        if num == 13:                            # UI_FS_FOPENFILE: nothing exists
            if A(2):
                vm.wr32(A(2), 0)
            return -1
        if num == 17:                            # UI_FS_GETFILELIST: empty
            return 0
        if num in (18, 19, 20, 30, 31):          # register model/skin/shader, CM model, sound
            return 1
        if num == 40:                            # UI_KEY_GETCATCHER
            return self.catcher
        if num == 41:                            # UI_KEY_SETCATCHER
            self.catcher = A(1)
            return 0
        if num == 43:                            # UI_GETGLCONFIG
            g = A(1)
            vm.mem[g & vm.mask:(g & vm.mask) + GLCONFIG_SIZE] = bytes(GLCONFIG_SIZE)
            vm.wrstr(g, "stub renderer", 1024)
            vm.wr32(g + GLC_DRIVERTYPE, self.driver_type)
            vm.wr32(g + GLC_HARDWARETYPE, self.hardware_type)
            vm.wr32(g + GLC_VIDWIDTH, self.vid[0])
            vm.wr32(g + GLC_VIDHEIGHT, self.vid[1])
            vm.wr32(g + GLC_ASPECT, f2i(self.vid[0] / float(self.vid[1])))
            vm.wr32(g + GLC_FULLSCREEN, 1)
            return 0
        if num == 44:                            # UI_GETCLIENTSTATE: disconnected
            c = A(1) & vm.mask
            vm.mem[c:c + 12 + 3 * 1024] = bytes(12 + 3 * 1024)
            return 0
        if num == 50:                            # UI_CVAR_REGISTER(vmCvar_t *, name, default, flags)
            name, default = vm.rdstr(A(2)), vm.rdstr(A(3))
            self.cvars.setdefault(name, default)
            if A(1):
                self.handles.append(name)
                vm.wr32(A(1), len(self.handles) - 1)
                self._fill_vmcvar(vm, A(1), name)
            return 0
        if num == 51:                            # UI_CVAR_UPDATE(vmCvar_t *)
            h = vm.rd32(A(1))
            if 0 <= h < len(self.handles):
                self._fill_vmcvar(vm, A(1), self.handles[h])
            return 0
        if num == 52:                            # UI_MEMORY_REMAINING
            return 64 << 20
        if num == 53:                            # UI_GET_CDKEY
            vm.wrstr(A(1), "", A(2))
            return 0
        if num == 81:                            # UI_VERIFY_CDKEY: accept
            return 1
        if num == 100:                           # memset(dest, c, n)
            d, c, n = A(1) & vm.mask, A(2) & 0xff, A(3)
            vm.mem[d:d + n] = bytes([c]) * n
            return A(1)
        if num == 101:                           # memcpy(dest, src, n)
            d, s, n = A(1) & vm.mask, A(2) & vm.mask, A(3)
            vm.mem[d:d + n] = vm.mem[s:s + n]
            return A(1)
        if num == 102:                           # strncpy(dest, src, n)
            d, s, n = A(1) & vm.mask, A(2) & vm.mask, A(3)
            src = vm.mem[s:s + n]
            z = src.find(b"\0")
            src = src[:z] if z >= 0 else src
            vm.mem[d:d + n] = src + bytes(n - len(src))
            return A(1)
        if 103 <= num <= 108:                    # sin cos atan2 sqrt floor ceil
            x = i2f(A(1))
            if num == 103:
                return f2i(math.sin(x))
            if num == 104:
                return f2i(math.cos(x))
            if num == 105:
                return f2i(math.atan2(x, i2f(A(2))))
            if num == 106:
                return f2i(math.sqrt(x) if x >= 0 else math.nan)
            return f2i(float(math.floor(x) if num == 107 else math.ceil(x)))
        return 0                                 # everything else: inert


def load_map(map_text, data_len, lit_len):
    """q3asm -m map -> {name: VM address}. Segment 0 holds code (instruction
    indices, negative = syscalls); 1 data, 2 lit and 3 bss are each written
    RELATIVE TO THEIR OWN SEGMENT - data at 0, lit after data, bss after lit
    (measured: memset(&s_main) lands at the map value + dataLength +
    litLength). q3asm also files _stackStart/_stackEnd under segment 0."""
    base = {0: 0, 1: 0, 2: data_len, 3: data_len + lit_len}
    syms = {}
    for line in map_text.splitlines():
        p = line.split()
        if len(p) != 3:
            continue
        seg, a, name = int(p[0]), int(p[1], 16), p[2]
        if a >= 0x80000000:
            a -= 0x100000000
        syms[name] = a + base.get(seg, 0)
    return syms


class UISim:
    """A booted UI module: keystrokes in, menu state and cvar writes out."""

    def __init__(self, qvm_bytes, map_text, cvars, vid=(1920, 1080), driver_type=0):
        self.vm = QVM(qvm_bytes)
        self.syms = load_map(map_text, self.vm.data_len, self.vm.lit_len)
        self.eng = UIEngine(cvars, vid=vid, driver_type=driver_type)
        self.time = 1000
        # q3_ui answers UI_OLD_API_VERSION 4 (q3_ui/ui_local.h); the engine
        # (client/cl_ui.c CL_InitUI) accepts 4 and 6
        self.api = self.main(UI_GETAPIVERSION)
        if self.api not in (4, 6):
            raise VMError("UI_GETAPIVERSION returned %r, engines accept 4 or 6" % self.api)
        self.main(UI_INIT)

    def main(self, cmd, *args):
        return self.vm.call(0, [cmd] + list(args), self.eng)

    def key(self, k, redraw=False):
        """A key press. The menus only change flags when they DRAW (the
        graphics menu un-hides ACCEPT in GraphicsOptions_UpdateMenuItems), so
        redraw where the flow needs it - drawing every frame is what makes a
        Python interpreter slow."""
        self.main(UI_KEY_EVENT, k, 1)
        if redraw:
            self.refresh()

    def refresh(self):
        self.time += 16
        self.main(UI_REFRESH, self.time)

    # --- reading the menu state -------------------------------------------
    def active_menu(self):
        return self.vm.rd32(self.syms["uis"] + UIS_ACTIVEMENU)

    def focused(self):
        """(item address, label) of the item under the menu cursor."""
        m = self.active_menu()
        if not m:
            return 0, None
        cur = self.vm.rd32(m + MF_CURSOR)
        item = self.vm.rd32(m + MF_ITEMS + 4 * cur)
        name = self.vm.rd32(item + MC_NAME)
        if not name and self.vm.rd32(item + MC_TYPE) != MTYPE_BITMAP:
            name = self.vm.rd32(item + MT_STRING)
        return item, (self.vm.rdstr(name) if name else "")

    def spin(self, item):
        """A spin control's (curvalue, numitems, [labels])."""
        cur, n = self.vm.rd32(item + ML_CURVALUE), self.vm.rd32(item + ML_NUMITEMS)
        names = self.vm.rd32(item + ML_ITEMNAMES)
        labels = [self.vm.rdstr(self.vm.rd32(names + 4 * i)) for i in range(n)]
        return cur, n, labels

    def goto(self, label, limit=40):
        """Press DOWN until the focused item's label is 'label'."""
        for _ in range(limit):
            if self.focused()[1] == label:
                return self.focused()[0]
            self.key(K_DOWNARROW)
        raise VMError("never reached %r (at %r)" % (label, self.focused()[1]))


def system_menu_session(qvm_bytes, map_text, cvars, vid, actions):
    """Main > Setup > System by keystrokes, then 'actions' on the Video Mode
    control, then change Texture Filter (so ACCEPT appears, as a user would)
    and press ACCEPT. Returns what the menu showed and what ACCEPT wrote."""
    ui = UISim(qvm_bytes, map_text, cvars, vid=vid)
    ui.main(UI_SET_ACTIVE_MENU, UIMENU_MAIN)
    ui.refresh()
    path = []
    for label in ("SETUP", "SYSTEM"):
        ui.goto(label)
        path.append(label)
        ui.key(K_ENTER, redraw=True)
    if ui.active_menu() != ui.syms.get("s_graphicsoptions"):
        raise VMError("SYSTEM did not open the graphics options menu")
    mode = ui.goto("Video Mode:")
    shown = ui.spin(mode)
    for k in actions:
        ui.key(k)
    ui.refresh()
    chosen = ui.spin(mode)
    filt = ui.goto("Texture Filter:")
    ui.key(K_RIGHTARROW if ui.spin(filt)[0] == 0 else K_LEFTARROW, redraw=True)
    before = len(ui.eng.writes)
    for _ in range(40):                      # DOWN to ACCEPT (hidden until a change)
        item, label = ui.focused()
        if ui.vm.rd32(item + MC_CALLBACK) == ui.syms["GraphicsOptions_ApplyChanges"]:
            break
        ui.key(K_DOWNARROW)
    else:
        raise VMError("ACCEPT never became reachable")
    ui.key(K_ENTER)
    writes = ui.eng.writes[before:]
    return {
        "path": path + ["Video Mode:"],
        "shown": {"index": shown[0], "label": shown[2][shown[0]], "count": shown[1], "labels": shown[2]},
        "chosen": {"index": chosen[0], "label": chosen[2][chosen[0]]},
        "accept_writes": writes,
        "accept": {k: v for k, v in writes},
        "commands": ui.eng.commands[-3:],
        "vm_steps": ui.vm.steps,
    }
