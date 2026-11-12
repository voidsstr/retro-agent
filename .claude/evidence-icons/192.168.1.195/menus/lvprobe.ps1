$src = @"
using System; using System.Runtime.InteropServices;
public class LvP {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string w);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string w);
 [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
 [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
 public static string Probe() {
   IntPtr p = FindWindowW("Progman", null);
   IntPtr d = FindWindowExW(p, IntPtr.Zero, "SHELLDLL_DefView", null);
   IntPtr w = IntPtr.Zero;
   while (d == IntPtr.Zero) { w = FindWindowExW(IntPtr.Zero, w, "WorkerW", null); if (w == IntPtr.Zero) break; d = FindWindowExW(w, IntPtr.Zero, "SHELLDLL_DefView", null); }
   IntPtr lv = FindWindowExW(d, IntPtr.Zero, "SysListView32", null);
   int st = GetWindowLong(lv, -16); IntPtr r, c;
   SendMessageTimeout(lv, 0x108F, IntPtr.Zero, IntPtr.Zero, 2, 2000, out r);
   SendMessageTimeout(lv, 0x1004, IntPtr.Zero, IntPtr.Zero, 2, 2000, out c);
   return String.Format("progman={0} workerw={1} defview={2} lv={3} style=0x{4:X8} type={5} getview={6} count={7}", p, w, d, lv, st, st & 3, r, c);
 }
}
"@
Add-Type -TypeDefinition $src
[LvP]::Probe()
