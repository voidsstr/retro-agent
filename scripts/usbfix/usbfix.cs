// usbfix.exe - remove failed USB devnodes, restart a hub, rescan PnP.
// usage: usbfix remove|restart|status <instance-id-prefix> | usbfix rescan
using System;
using System.Runtime.InteropServices;
using System.Text;

class UsbFix
{
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }

    [StructLayout(LayoutKind.Sequential)]
    struct SP_PROPCHANGE_PARAMS
    {
        public int cbSize; public int InstallFunction;
        public int StateChange; public int Scope; public int HwProfile;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Ansi)]
    static extern IntPtr SetupDiGetClassDevsA(IntPtr gid, string enumerator, IntPtr hwnd, int flags);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, int index, ref SP_DEVINFO_DATA did);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiCallClassInstaller(int dif, IntPtr set, ref SP_DEVINFO_DATA did);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiSetClassInstallParams(IntPtr set, ref SP_DEVINFO_DATA did, ref SP_PROPCHANGE_PARAMS p, int size);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Ansi)]
    static extern int CM_Get_Device_IDA(int devInst, StringBuilder buf, int len, int flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Ansi)]
    static extern int CM_Locate_DevNodeA(out int devInst, string id, int flags);
    [DllImport("cfgmgr32.dll")]
    static extern int CM_Reenumerate_DevNode(int devInst, int flags);
    [DllImport("cfgmgr32.dll")]
    static extern int CM_Get_DevNode_Status(out int status, out int problem, int devInst, int flags);

    const int DIGCF_ALLCLASSES = 4;
    const int DIF_REMOVE = 5, DIF_PROPERTYCHANGE = 0x12;
    const int DICS_ENABLE = 1, DICS_DISABLE = 2, DICS_FLAG_GLOBAL = 1;

    delegate void Action2(IntPtr set, SP_DEVINFO_DATA did, string id);

    static int ForEach(string prefix, Action2 fn)
    {
        string enumerator = prefix.IndexOf('\\') > 0 ? prefix.Substring(0, prefix.IndexOf('\\')) : "USB";
        IntPtr set = SetupDiGetClassDevsA(IntPtr.Zero, enumerator, IntPtr.Zero, DIGCF_ALLCLASSES);
        if (set == (IntPtr)(-1)) { Console.WriteLine("GetClassDevs failed"); return -1; }
        int hits = 0;
        SP_DEVINFO_DATA did = new SP_DEVINFO_DATA();
        did.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        for (int i = 0; SetupDiEnumDeviceInfo(set, i, ref did); i++)
        {
            StringBuilder sb = new StringBuilder(512);
            if (CM_Get_Device_IDA(did.DevInst, sb, 512, 0) != 0) continue;
            string id = sb.ToString();
            if (!id.ToUpper().StartsWith(prefix.ToUpper())) continue;
            hits++;
            fn(set, did, id);
        }
        SetupDiDestroyDeviceInfoList(set);
        return hits;
    }

    static void ChangeState(IntPtr set, SP_DEVINFO_DATA did, int state, string id, string verb)
    {
        SP_PROPCHANGE_PARAMS p = new SP_PROPCHANGE_PARAMS();
        p.cbSize = 8; // sizeof(SP_CLASSINSTALL_HEADER)
        p.InstallFunction = DIF_PROPERTYCHANGE;
        p.StateChange = state; p.Scope = DICS_FLAG_GLOBAL; p.HwProfile = 0;
        if (!SetupDiSetClassInstallParams(set, ref did, ref p, Marshal.SizeOf(typeof(SP_PROPCHANGE_PARAMS))) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, ref did))
            Console.WriteLine(verb + "-FAIL " + id + " err=" + Marshal.GetLastWin32Error());
        else
            Console.WriteLine(verb + "-OK " + id);
    }

    static int Main(string[] args)
    {
        if (args.Length < 1) { Console.WriteLine("usage: usbfix remove|restart|status <prefix> | rescan"); return 2; }
        string cmd = args[0].ToLower();
        if (cmd == "rescan")
        {
            int root;
            if (CM_Locate_DevNodeA(out root, null, 0) == 0 && CM_Reenumerate_DevNode(root, 0) == 0)
                Console.WriteLine("RESCAN-OK");
            else Console.WriteLine("RESCAN-FAIL");
            return 0;
        }
        if (args.Length < 2) { Console.WriteLine("need prefix"); return 2; }
        string prefix = args[1];
        int n = -1;
        if (cmd == "remove")
            n = ForEach(prefix, delegate(IntPtr set, SP_DEVINFO_DATA did, string id) {
                if (SetupDiCallClassInstaller(DIF_REMOVE, set, ref did))
                    Console.WriteLine("REMOVED " + id);
                else
                    Console.WriteLine("REMOVE-FAIL " + id + " err=" + Marshal.GetLastWin32Error());
            });
        else if (cmd == "restart")
            n = ForEach(prefix, delegate(IntPtr set, SP_DEVINFO_DATA did, string id) {
                ChangeState(set, did, DICS_DISABLE, id, "DISABLE");
                System.Threading.Thread.Sleep(2000);
                ChangeState(set, did, DICS_ENABLE, id, "ENABLE");
            });
        else if (cmd == "status")
            n = ForEach(prefix, delegate(IntPtr set, SP_DEVINFO_DATA did, string id) {
                int st, pr; CM_Get_DevNode_Status(out st, out pr, did.DevInst, 0);
                Console.WriteLine("STATUS " + id + " status=0x" + st.ToString("x") + " problem=" + pr);
            });
        Console.WriteLine("MATCHES=" + n);
        return n > 0 ? 0 : 1;
    }
}
