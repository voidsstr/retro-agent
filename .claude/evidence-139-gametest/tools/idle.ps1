Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class I{[StructLayout(LayoutKind.Sequential)]public struct L{public uint s;public uint t;}[DllImport("user32.dll")]public static extern bool GetLastInputInfo(ref L l);public static uint Idle(){L l=new L();l.s=8;GetLastInputInfo(ref l);return ((uint)Environment.TickCount-l.t)/1000;}}'
[I]::Idle()
