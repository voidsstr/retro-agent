Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public class M{[StructLayout(LayoutKind.Sequential,CharSet=CharSet.Ansi)]public struct DM{[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string n;public short a;public short b;public short size;public short ex;public int f;public int x;public int y;public int o;public int fx;public short c;public short d;public short yr;public short tt;public short co;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)]public string fn;public short lp;public int bpp;public int w;public int h;public int fl;public int hz;public int i1;public int i2;public int i3;public int i4;public int r1;public int r2;public int pw;public int ph;}
[DllImport("user32.dll")]public static extern bool EnumDisplaySettingsA(string d,int i,ref DM m);}
'@
"start";$s=@{};$i=0;$m=New-Object M+DM;$m.size=[Runtime.InteropServices.Marshal]::SizeOf($m)
while([M]::EnumDisplaySettingsA($null,$i,[ref]$m)){ if($m.bpp -eq 32){$k="$($m.w)x$($m.h)";if(-not $s[$k] -or $s[$k] -lt $m.hz){$s[$k]=$m.hz}};$i++}
"count $i";$s.GetEnumerator()|Sort-Object {[int]($_.Key.Split('x')[0])}|%{"$($_.Key)@$($_.Value)"}
