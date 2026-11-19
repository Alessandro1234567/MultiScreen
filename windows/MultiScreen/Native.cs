using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MultiScreen;

/// Tutte le chiamate Win32 usate dal programma.
static unsafe class Native
{
    // ------------------------------------------------------------ schermi
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public uint StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    public const uint DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 1, DISPLAY_DEVICE_PRIMARY_DEVICE = 4;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public ushort dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public uint dmFields;
        public int dmPositionX, dmPositionY;
        public uint dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public ushort dmLogPixels;
        public uint dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public uint dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    public const uint DM_POSITION = 0x20, DM_PELSWIDTH = 0x80000, DM_PELSHEIGHT = 0x100000, DM_DISPLAYFREQUENCY = 0x400000;
    public const uint CDS_UPDATEREGISTRY = 1, CDS_NORESET = 0x10000000;
    public const int ENUM_CURRENT_SETTINGS = -1;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE dd, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettings(string name, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int ChangeDisplaySettingsEx(string name, ref DEVMODE dm, IntPtr hwnd, uint flags, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int ChangeDisplaySettingsEx(string name, IntPtr dm, IntPtr hwnd, uint flags, IntPtr lParam);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    public const int SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77, SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79;
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);

    // ------------------------------------------------------------ DisplayConfig (scala DPI)
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)]
    public struct PATH_INFO
    {
        public LUID srcAdapter; public uint srcId, srcModeIdx, srcFlags;
        public LUID tgtAdapter; public uint tgtId, tgtModeIdx, tech, rotation, scaling, rrNum, rrDen, scanline; public int available; public uint tgtFlags;
        public uint flags;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MODE_INFO { public uint infoType, id; public LUID adapter; public fixed byte data[48]; }
    [StructLayout(LayoutKind.Sequential)]
    public struct DEVICE_INFO_HEADER { public int type; public uint size; public LUID adapter; public uint id; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SOURCE_DEVICE_NAME { public DEVICE_INFO_HEADER h; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string gdiName; }
    [StructLayout(LayoutKind.Sequential)]
    public struct DPI_GET { public DEVICE_INFO_HEADER h; public int minRel, curRel, maxRel; }
    [StructLayout(LayoutKind.Sequential)]
    public struct DPI_SET { public DEVICE_INFO_HEADER h; public int rel; }
    public const uint QDC_ONLY_ACTIVE_PATHS = 2;

    [DllImport("user32.dll")] public static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")]
    public static extern int QueryDisplayConfig(uint flags, ref uint np, [Out] PATH_INFO[] p, ref uint nm, [Out] MODE_INFO[] m, IntPtr topo);
    [DllImport("user32.dll")] public static extern int DisplayConfigGetDeviceInfo(ref SOURCE_DEVICE_NAME r);
    [DllImport("user32.dll")] public static extern int DisplayConfigGetDeviceInfo(ref DPI_GET r);
    [DllImport("user32.dll")] public static extern int DisplayConfigSetDeviceInfo(ref DPI_SET r);

    // ------------------------------------------------------------ input
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public MOUSEINPUT mi; }
    public const uint MOUSEEVENTF_MOVE = 1, MOUSEEVENTF_LEFTDOWN = 2, MOUSEEVENTF_LEFTUP = 4, MOUSEEVENTF_RIGHTDOWN = 8,
        MOUSEEVENTF_RIGHTUP = 0x10, MOUSEEVENTF_MIDDLEDOWN = 0x20, MOUSEEVENTF_MIDDLEUP = 0x40, MOUSEEVENTF_WHEEL = 0x800,
        MOUSEEVENTF_HWHEEL = 0x1000, MOUSEEVENTF_VIRTUALDESK = 0x4000, MOUSEEVENTF_ABSOLUTE = 0x8000;
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);

    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINTER_INFO
    {
        public uint pointerType, pointerId, frameId, pointerFlags;
        public IntPtr sourceDevice, hwndTarget;
        public POINT ptPixelLocation, ptHimetricLocation, ptPixelLocationRaw, ptHimetricLocationRaw;
        public uint dwTime, historyCount;
        public int inputData;
        public uint dwKeyStates;
        public ulong performanceCount;
        public int buttonChangeType;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINTER_TOUCH_INFO
    {
        public POINTER_INFO pointerInfo;
        public uint touchFlags, touchMask;
        public RECT rcContact, rcContactRaw;
        public uint orientation, pressure;
    }
    public const uint PT_TOUCH = 2;
    public const uint POINTER_FLAG_INRANGE = 0x2, POINTER_FLAG_INCONTACT = 0x4, POINTER_FLAG_DOWN = 0x10000,
        POINTER_FLAG_UPDATE = 0x20000, POINTER_FLAG_UP = 0x40000, POINTER_FLAG_CANCELED = 0x8000;
    public const uint TOUCH_MASK_CONTACTAREA = 1, TOUCH_MASK_ORIENTATION = 2, TOUCH_MASK_PRESSURE = 4;
    [DllImport("user32.dll", SetLastError = true)] public static extern bool InitializeTouchInjection(uint max, uint mode);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool InjectTouchInput(uint n, [In] POINTER_TOUCH_INFO[] c);

    // ------------------------------------------------------------ finestra, tray, appunti
    public delegate IntPtr WndProc(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WNDCLASSEX
    {
        public uint cbSize, style; public WndProc lpfnWndProc; public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground; public string lpszMenuName, lpszClassName; public IntPtr hIconSm;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public POINT pt; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern ushort RegisterClassEx(ref WNDCLASSEX c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowEx(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr p);
    [DllImport("user32.dll")] public static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetMessage(out MSG m, IntPtr h, uint a, uint b);
    [DllImport("user32.dll")] public static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] public static extern IntPtr DispatchMessage(ref MSG m);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void PostQuitMessage(int c);
    [DllImport("user32.dll")] public static extern IntPtr LoadIcon(IntPtr inst, IntPtr name);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr ExtractIcon(IntPtr inst, string file, int idx);
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(IntPtr name);
    [DllImport("user32.dll")] public static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool AppendMenu(IntPtr m, uint flags, UIntPtr id, string text);
    [DllImport("user32.dll")] public static extern int TrackPopupMenu(IntPtr m, uint flags, int x, int y, int r, IntPtr h, IntPtr rect);
    [DllImport("user32.dll")] public static extern bool DestroyMenu(IntPtr m);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int MessageBox(IntPtr h, string text, string caption, uint type);
    public const uint MF_STRING = 0, MF_GRAYED = 1, MF_SEPARATOR = 0x800, TPM_RETURNCMD = 0x100, TPM_RIGHTBUTTON = 2;
    public const uint WM_DESTROY = 2, WM_COMMAND = 0x111, WM_CLIPBOARDUPDATE = 0x31D, WM_APP = 0x8000, WM_RBUTTONUP = 0x205,
        WM_LBUTTONUP = 0x202, WM_CONTEXTMENU = 0x7B;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct NOTIFYICONDATA
    {
        public uint cbSize; public IntPtr hWnd; public uint uID, uFlags, uCallbackMessage; public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState, dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags; public Guid guidItem; public IntPtr hBalloonIcon;
    }
    public const uint NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2, NIF_MESSAGE = 1, NIF_ICON = 2, NIF_TIP = 4, NIF_INFO = 0x10, NIIF_INFO = 1;
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern bool Shell_NotifyIcon(uint msg, ref NOTIFYICONDATA d);

    [DllImport("user32.dll")] public static extern bool AddClipboardFormatListener(IntPtr h);
    [DllImport("user32.dll")] public static extern bool OpenClipboard(IntPtr h);
    [DllImport("user32.dll")] public static extern bool CloseClipboard();
    [DllImport("user32.dll")] public static extern bool EmptyClipboard();
    [DllImport("user32.dll")] public static extern IntPtr GetClipboardData(uint f);
    [DllImport("user32.dll")] public static extern IntPtr SetClipboardData(uint f, IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetClipboardOwner();
    [DllImport("kernel32.dll")] public static extern IntPtr GlobalAlloc(uint f, UIntPtr n);
    [DllImport("kernel32.dll")] public static extern IntPtr GlobalLock(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool GlobalUnlock(IntPtr h);
    public const uint CF_UNICODETEXT = 13, GMEM_MOVEABLE = 2;

    [DllImport("shell32.dll")] public static extern int SHGetKnownFolderPath(ref Guid id, uint flags, IntPtr token, out IntPtr path);

    // ------------------------------------------------------------ driver schermo virtuale (SetupAPI + IOCTL)
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA { public uint cbSize; public Guid classGuid; public uint flags; public IntPtr reserved; }
    public const uint DIGCF_PRESENT = 2, DIGCF_DEVICEINTERFACE = 0x10;
    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid g, uint idx, ref SP_DEVICE_INTERFACE_DATA d);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA d, IntPtr detail, uint size, out uint req, IntPtr devInfo);
    [DllImport("setupapi.dll")] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte* inBuf, uint inSize, byte* outBuf, uint outSize, IntPtr ret, NativeOverlapped* ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetOverlappedResultEx(SafeFileHandle h, NativeOverlapped* ov, out uint n, uint ms, bool alertable);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr CreateEvent(IntPtr sa, bool manual, bool initial, IntPtr name);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    public const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2,
        OPEN_EXISTING = 3, FILE_ATTRIBUTE_NORMAL = 0x80, FILE_FLAG_NO_BUFFERING = 0x20000000, FILE_FLAG_OVERLAPPED = 0x40000000,
        FILE_FLAG_WRITE_THROUGH = 0x80000000;

    // ------------------------------------------------------------ DXGI (per trovare l'indice dell'uscita da catturare)
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DXGI_ADAPTER_DESC
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId, DeviceId, SubSysId, Revision;
        public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
        public LUID AdapterLuid;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DXGI_OUTPUT_DESC
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        public RECT DesktopCoordinates; public int AttachedToDesktop; public uint Rotation; public IntPtr Monitor;
    }

    [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIFactory1
    {
        void _SetPrivateData(); void _SetPrivateDataInterface(); void _GetPrivateData(); void _GetParent();
        void _EnumAdapters(); void _MakeWindowAssociation(); void _GetWindowAssociation(); void _CreateSwapChain(); void _CreateSoftwareAdapter();
        [PreserveSig] int EnumAdapters1(uint index, out IDXGIAdapter1 adapter);
    }
    [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIAdapter1
    {
        void _SetPrivateData(); void _SetPrivateDataInterface(); void _GetPrivateData(); void _GetParent();
        [PreserveSig] int EnumOutputs(uint index, out IDXGIOutput output);
        [PreserveSig] int GetDesc(out DXGI_ADAPTER_DESC desc);
    }
    [ComImport, Guid("ae02eedb-c735-4690-8d52-5a8dc20213aa"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIOutput
    {
        void _SetPrivateData(); void _SetPrivateDataInterface(); void _GetPrivateData(); void _GetParent();
        [PreserveSig] int GetDesc(out DXGI_OUTPUT_DESC desc);
    }
    [DllImport("dxgi.dll")] public static extern int CreateDXGIFactory1(ref Guid riid, out IDXGIFactory1 f);
}
