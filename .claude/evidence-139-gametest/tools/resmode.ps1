Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class D{[DllImport("user32.dll")]public static extern int ChangeDisplaySettingsA(IntPtr m,int f);}'
[D]::ChangeDisplaySettingsA([IntPtr]::Zero,0)
