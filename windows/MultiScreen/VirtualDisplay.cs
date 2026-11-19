using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using static MultiScreen.Native;

namespace MultiScreen;

/// Schermi virtuali con il driver "Parsec Virtual Display" (firmato, gratuito).
/// Gli schermi restano finche' il programma manda il "keepalive": se il programma si chiude spariscono da soli.
static unsafe class VirtualDisplay
{
    static readonly Guid AdapterGuid = new("00b41627-04c4-429e-a26e-0265cf50c8fa");
    const uint IOCTL_ADD = 0x0022e004, IOCTL_REMOVE = 0x0022a008, IOCTL_UPDATE = 0x0022a00c, IOCTL_VERSION = 0x0022e010;
    const string RegKey = @"SOFTWARE\Parsec\vdd";

    static readonly object Lock = new();
    static SafeFileHandle _h;
    static Thread _keepalive;

    public static bool Available
    {
        get { lock (Lock) return EnsureOpen(); }
    }

    static bool EnsureOpen()
    {
        if (_h != null && !_h.IsInvalid) return true;
        string path = DevicePath();
        if (path == null) return false;
        _h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH, IntPtr.Zero);
        if (_h.IsInvalid)
        {
            Log.E($"driver schermo virtuale: apertura fallita ({Marshal.GetLastWin32Error()})");
            _h = null;
            return false;
        }
        Log.I($"driver schermo virtuale aperto (versione {Io(IOCTL_VERSION, null)})");
        if (_keepalive == null)
        {
            _keepalive = new Thread(() =>
            {
                while (true)
                {
                    lock (Lock) { if (_h != null) Io(IOCTL_UPDATE, null); }
                    Thread.Sleep(100);
                }
            }) { IsBackground = true, Name = "vdd-keepalive" };
            _keepalive.Start();
        }
        return true;
    }

    static string DevicePath()
    {
        var g = AdapterGuid;
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == new IntPtr(-1)) return null;
        try
        {
            var d = new SP_DEVICE_INTERFACE_DATA { cbSize = (uint)Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>() };
            if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, 0, ref d)) return null;
            SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, out uint req, IntPtr.Zero);
            IntPtr buf = Marshal.AllocHGlobal((int)req);
            try
            {
                Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6); // cbSize di SP_DEVICE_INTERFACE_DETAIL_DATA_W
                if (!SetupDiGetDeviceInterfaceDetail(set, ref d, buf, req, out _, IntPtr.Zero)) return null;
                return Marshal.PtrToStringUni(buf + 4);
            }
            finally { Marshal.FreeHGlobal(buf); }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
    }

    static uint Io(uint code, byte[] data)
    {
        byte* inBuf = stackalloc byte[32];
        new Span<byte>(inBuf, 32).Clear();
        if (data != null) data.AsSpan(0, Math.Min(32, data.Length)).CopyTo(new Span<byte>(inBuf, 32));
        uint outVal = 0;
        var ov = (NativeOverlapped*)Marshal.AllocHGlobal(sizeof(NativeOverlapped));
        *ov = default;
        ov->EventHandle = CreateEvent(IntPtr.Zero, true, false, IntPtr.Zero);
        try
        {
            DeviceIoControl(_h, code, inBuf, 32, (byte*)&outVal, 4, IntPtr.Zero, ov);
            GetOverlappedResultEx(_h, ov, out _, 5000, false);
            return outVal;
        }
        finally
        {
            CloseHandle(ov->EventHandle);
            Marshal.FreeHGlobal((IntPtr)ov);
        }
    }

    /// Aggiunge uno schermo e ritorna il suo nome GDI (es. \\.\DISPLAY5) o null.
    public static string Add(out int index)
    {
        index = -1;
        var before = Displays.List().Select(m => m.Name).ToHashSet();
        lock (Lock)
        {
            if (!EnsureOpen()) return null;
            index = (int)Io(IOCTL_ADD, null);
            Io(IOCTL_UPDATE, null);
        }
        for (int i = 0; i < 50; i++) // aspetta che Windows lo attivi (max 5 s)
        {
            Thread.Sleep(100);
            var nw = Displays.List().FirstOrDefault(m => !before.Contains(m.Name));
            if (nw != null) return nw.Name;
        }
        Log.W("schermo virtuale aggiunto ma Windows non lo mostra");
        return null;
    }

    public static void Remove(int index)
    {
        if (index < 0) return;
        lock (Lock)
        {
            if (_h == null) return;
            ushort v = (ushort)(((index & 0xFF) << 8) | ((index >> 8) & 0xFF));
            Io(IOCTL_REMOVE, new[] { (byte)v, (byte)(v >> 8) });
            Io(IOCTL_UPDATE, null);
        }
    }

    /// Il driver conosce solo alcune risoluzioni: le altre vanno aggiunte nel registro (max 5) e il driver riavviato.
    public static bool EnsureCustomMode(int w, int h, int hz)
    {
        using var k = Registry.LocalMachine.CreateSubKey(RegKey);
        int free = -1;
        for (int i = 0; i < 5; i++)
        {
            using var s = k.OpenSubKey(i.ToString());
            if (s == null) { if (free < 0) free = i; continue; }
            if ((int)(s.GetValue("width") ?? 0) == w && (int)(s.GetValue("height") ?? 0) == h) return false; // c'e' gia'
        }
        if (free < 0)
        {
            int next = (int)(k.GetValue("next") ?? 0) % 5;
            free = next;
            k.SetValue("next", (next + 1) % 5, RegistryValueKind.DWord);
        }
        using (var s = k.CreateSubKey(free.ToString()))
        {
            s.SetValue("width", w, RegistryValueKind.DWord);
            s.SetValue("height", h, RegistryValueKind.DWord);
            s.SetValue("hz", hz, RegistryValueKind.DWord);
        }
        Log.I($"driver: aggiungo la risoluzione {w}x{h}@{hz} (slot {free}) e riavvio il driver");
        RestartDriver();
        return true;
    }

    static void RestartDriver()
    {
        lock (Lock) { _h?.Dispose(); _h = null; }
        string id = InstanceId();
        if (id == null) { Log.W("driver: non trovo l'ID del dispositivo"); return; }
        Run("pnputil", "/restart-device", id);
        Thread.Sleep(1500);
        lock (Lock) EnsureOpen();
    }

    /// ID dispositivo del driver Parsec (l'output di pnputil e' tradotto: cerco il blocco che nomina Parsec)
    static string InstanceId()
    {
        string o = Run("pnputil", "/enum-devices", "/class", "Display");
        foreach (var block in o.Replace("\r", "").Split("\n\n"))
        {
            if (!block.Contains("Parsec", StringComparison.OrdinalIgnoreCase)) continue;
            var first = block.Split('\n').FirstOrDefault(l => l.Contains(':'));
            if (first != null) return first[(first.IndexOf(':') + 1)..].Trim();
        }
        return null;
    }

    static string Run(string exe, params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo(exe) { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true };
            foreach (var a in args) psi.ArgumentList.Add(a);
            using var p = Process.Start(psi);
            string s = p.StandardOutput.ReadToEnd();
            p.WaitForExit(15000);
            return s;
        }
        catch (Exception e) { Log.W($"{exe}: {e.Message}"); return ""; }
    }
}
