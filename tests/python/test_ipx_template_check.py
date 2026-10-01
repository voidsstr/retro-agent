"""scripts/vm/win98/ipx-template-check.py - IPXSETUP's Win98 template vs a disk image.

The Win98 half of IPXSETUP writes nothing until IpxSetup9xTemplateOk=1, which
is set only once the template (agent/shared/ipxplan.h) matches a golden
Network-applet install. This tool makes that check repeatable: it reads
SYSTEM.DAT out of a raw disk image read-only and compares value by value. It
first ran 2026-10-01 against W98BUILD after the orchestrator's applet install
(0 differences once six applet-only values were added to the template).

Pinned here, without any disk image:
* the tool reads the template FROM THE HEADER and renders it exactly as the
  agent's C ipx9x_render does (compiled and compared when gcc is present);
* every rendered line parses, and the binding is the last value;
* compare() reports MISSING / DIFFERS / EXTRA and treats the consumed first-boot
  queue as unobservable - and can say no;
* it never writes: the image is opened 'rb' and nothing is mounted.
"""
import importlib.util
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts" / "vm" / "win98" / "ipx-template-check.py"
VALUES = {"K": "0003", "I": "0001", "Q": "0", "FRAME": "1",
          "NIC": "PCI\\VEN_10EC&DEV_8029&SUBSYS_802910EC&REV_00\\BUS_00&DEV_0B&FUNC_00",
          "SYSDIR:Q": "C:\\\\WINDOWS\\\\SYSTEM"}


def tool():
    spec = importlib.util.spec_from_file_location("ipx_template_check", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def rendered(t):
    parts = t.template_parts()
    return "".join(t.render(parts[p], VALUES) for p in t.PARTS)


C_RENDER = r"""
#include <stdio.h>
#include "ipxplan.h"
int main(void)
{
    static char out[16384];
    ipx_var_t v[6];
    int p;
    v[0].name = "K"; v[0].value = "0003";
    v[1].name = "I"; v[1].value = "0001";
    v[2].name = "Q"; v[2].value = "0";
    v[3].name = "FRAME"; v[3].value = "1";
    v[4].name = "NIC"; v[4].value = "PCI\\VEN_10EC&DEV_8029&SUBSYS_802910EC&REV_00\\BUS_00&DEV_0B&FUNC_00";
    v[5].name = "SYSDIR:Q"; v[5].value = "C:\\\\WINDOWS\\\\SYSTEM";
    for (p = 0; p < IPX9X_NPARTS; p++) {
        if (ipx9x_render(ipx9x_part(p), v, 6, out, sizeof(out)) < 0)
            return 1;
        fwrite(out, 1, strlen(out), stdout);
    }
    return 0;
}
"""


def test_the_tool_renders_exactly_what_the_agent_renders(tmp_path):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - the Python/C render comparison did NOT run")
    src = tmp_path / "r.c"
    src.write_text(C_RENDER)
    exe = tmp_path / "r"
    subprocess.run([cc, "-std=c11", "-I", str(ROOT / "agent" / "shared"), str(src), "-o", str(exe)],
                   check=True)
    c_out = subprocess.run([str(exe)], capture_output=True, check=True).stdout.decode("latin-1")
    assert c_out == rendered(tool()), "the tool's template extraction drifted from the C header"


def test_every_line_parses_and_the_binding_is_last():
    t = tool()
    entries = t.parse_reg(rendered(t))
    assert len(entries) > 25
    last_key, last_vals = entries[-1]
    assert last_key == "Enum\\" + VALUES["NIC"] + "\\Bindings"
    assert list(last_vals.items()) == [("NWLINK\\0001", (1, b""))]
    keys = [k for k, _v in entries]
    assert "Enum\\Network\\NWLINK\\0001\\Bindings" in keys
    assert dict(entries)["Enum\\Network\\NWLINK\\0001\\Bindings"] == {}, "nothing rides IPX"


class FakeReg:
    def __init__(self, tree):
        self.tree = tree

    def find(self, key):
        return key if key in self.tree else None

    def values(self, off):
        return self.tree.get(off, {})


def test_compare_can_say_yes_and_no():
    t = tool()
    parts = t.template_parts()
    entries = t.parse_reg(rendered(t))
    golden = {k: dict(v) for k, v in entries if not k.startswith(t.QUEUE_KEYS)}
    golden["Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"] = {"Other": (1, b"x.exe")}
    lines, bad = t.compare(FakeReg(golden), parts, VALUES)
    assert bad == 0, lines
    assert any("not observable" in l and "RunOnce" in l for l in lines)

    cls = "System\\CurrentControlSet\\Services\\Class\\NetTrans\\0003"
    broken = {k: dict(v) for k, v in golden.items()}
    del broken[cls]["DriverDate"]
    broken[cls]["Frame_Type"] = (1, b"4")
    broken[cls]["Mystery"] = (1, b"1")
    lines, bad = t.compare(FakeReg(broken), parts, VALUES)
    assert bad == 3, lines
    assert any(l.startswith("MISSING") and "DriverDate" in l for l in lines)
    assert any(l.startswith("DIFFERS") and "Frame_Type" in l for l in lines)
    assert any(l.startswith("EXTRA") and "Mystery" in l for l in lines)


def test_it_never_writes():
    src = TOOL.read_text()
    code = re.sub(r'"""(?:.|\n)*?"""', "", src)
    assert 'open(path, "rb")' in code
    for w in ('"wb"', '"r+b"', '"ab"', "mount", "losetup", "os.remove", "unlink", ".write("):
        assert w not in code, w
