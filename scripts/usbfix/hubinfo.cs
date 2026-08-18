// hubinfo.exe - dump USB hub power mode + per-port connection status via IOCTLs.
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

class HubInfo
{
    static Guid GUID_DEVINTERFACE_USB_HUB = new Guid("F18A0E88-C30C-11D0-8815-00A0C906BED8");

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }

    [DllImport("setupapi.dll", CharSet = CharSet.Ansi)]
    static extern IntPtr SetupDiGetClassDevsA(ref Guid gid, IntPtr enumerator, IntPtr hwnd, int flags);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr did, ref Guid gid, int index, ref SP_DEVICE_INTERFACE_DATA ifd);
    [DllImport("setupapi.dll", CharSet = CharSet.Ansi)]
    static extern bool SetupDiGetDeviceInterfaceDetailA(IntPtr set, ref SP_DEVICE_INTERFACE_DATA ifd, IntPtr detail, int size, out int req, ref SP_DEVINFO_DATA did);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Ansi)]
    static extern int CM_Get_Device_IDA(int devInst, System.Text.StringBuilder buf, int len, int flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    static extern SafeFileHandle CreateFileA(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int ret, IntPtr ov);

    const uint IOCTL_USB_GET_NODE_INFORMATION = 0x220408;
    const uint IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX = 0x220448;
    static string[] CONN = { "NoDeviceConnected", "DeviceConnected", "DeviceFailedEnumeration",
        "DeviceGeneralFailure", "DeviceCausedOvercurrent", "DeviceNotEnoughPower",
        "DeviceNotEnoughBandwidth", "DeviceHubNestedTooDeeply", "DeviceInLegacyHub" };

    static void Main(string[] args)
    {
        string filter = args.Length > 0 ? args[0].ToUpper() : "";
        const int DIGCF_PRESENT = 2, DIGCF_DEVICEINTERFACE = 0x10;
        IntPtr set = SetupDiGetClassDevsA(ref GUID_DEVINTERFACE_USB_HUB, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        SP_DEVICE_INTERFACE_DATA ifd = new SP_DEVICE_INTERFACE_DATA();
        ifd.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
        for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref GUID_DEVINTERFACE_USB_HUB, i, ref ifd); i++)
        {
            SP_DEVINFO_DATA did = new SP_DEVINFO_DATA();
            did.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            int req;
            SetupDiGetDeviceInterfaceDetailA(set, ref ifd, IntPtr.Zero, 0, out req, ref did);
            IntPtr detail = Marshal.AllocHGlobal(req);
            Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 5); // cbSize of detail struct (ansi x86 = 5)
            if (!SetupDiGetDeviceInterfaceDetailA(set, ref ifd, detail, req, out req, ref did)) { Marshal.FreeHGlobal(detail); continue; }
            string path = Marshal.PtrToStringAnsi(new IntPtr(detail.ToInt64() + 4));
            Marshal.FreeHGlobal(detail);
            System.Text.StringBuilder sb = new System.Text.StringBuilder(512);
            CM_Get_Device_IDA(did.DevInst, sb, 512, 0);
            string instId = sb.ToString();
            if (filter != "" && !instId.ToUpper().Contains(filter)) continue;
            Console.WriteLine("HUB " + instId);
            SafeFileHandle h = CreateFileA(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (h.IsInvalid) { h = CreateFileA(path, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); }
            if (h.IsInvalid) { Console.WriteLine("  open failed err=" + Marshal.GetLastWin32Error()); continue; }
            byte[] node = new byte[128];
            int ret;
            int ports = 0;
            if (DeviceIoControl(h, IOCTL_USB_GET_NODE_INFORMATION, node, node.Length, node, node.Length, out ret, IntPtr.Zero))
            {
                ports = node[6];
                int chars = node[7] | (node[8] << 8);
                bool busPowered = node[75] != 0;
                Console.WriteLine("  ports=" + ports + " wHubCharacteristics=0x" + chars.ToString("x4")
                    + " HubIsBusPowered=" + busPowered + " PowerOnToPowerGood=" + (node[9] * 2) + "ms"
                    + " HubControlCurrent=" + node[10] + "mA");
            }
            else Console.WriteLine("  NODE_INFORMATION failed err=" + Marshal.GetLastWin32Error());
            for (int p = 1; p <= ports; p++)
            {
                byte[] buf = new byte[1024];
                buf[0] = (byte)p;
                if (!DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf, buf.Length, buf, buf.Length, out ret, IntPtr.Zero))
                { Console.WriteLine("  port " + p + ": ioctl failed err=" + Marshal.GetLastWin32Error()); continue; }
                int connStatus = BitConverter.ToInt32(buf, 31); // usbioctl.h structs are pack(1)
                int vid = BitConverter.ToUInt16(buf, 4 + 8);
                int pid = BitConverter.ToUInt16(buf, 4 + 10);
                int devClass = buf[4 + 4];
                int maxPower0 = buf[4 + 7]; // bMaxPacketSize0 actually; keep descriptor basics
                bool isHub = buf[24] != 0;
                int speed = buf[23];
                string cs = (connStatus >= 0 && connStatus < CONN.Length) ? CONN[connStatus] : ("status=" + connStatus);
                Console.WriteLine("  port " + p + ": " + cs
                    + (connStatus != 0 ? (" vid=" + vid.ToString("x4") + " pid=" + pid.ToString("x4") + " speed=" + speed + " isHub=" + isHub) : ""));
            }
            h.Close();
        }
        SetupDiDestroyDeviceInfoList(set);
    }
}
