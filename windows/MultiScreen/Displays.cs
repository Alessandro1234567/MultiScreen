using System.Runtime.InteropServices;
using static MultiScreen.Native;

namespace MultiScreen;

record Monitor(string Name, string Adapter, int X, int Y, int W, int H, bool Primary);

/// Schermi di Windows: elenco, risoluzione, posizione, scala, indice DXGI per la cattura.
static class Displays
{
    public static List<Monitor> List()
    {
        var res = new List<Monitor>();
        for (uint i = 0; ; i++)
        {
            var dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
            if (!EnumDisplayDevices(null, i, ref dd, 0)) break;
            if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;
            var dm = NewDevMode();
            if (!EnumDisplaySettings(dd.DeviceName, ENUM_CURRENT_SETTINGS, ref dm)) continue;
            res.Add(new Monitor(dd.DeviceName, dd.DeviceString, dm.dmPositionX, dm.dmPositionY, (int)dm.dmPelsWidth,
                (int)dm.dmPelsHeight, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0));
        }
        return res;
    }

    public static Monitor Find(string name) => List().FirstOrDefault(m => m.Name == name);
    public static Monitor Primary() => List().FirstOrDefault(m => m.Primary) ?? List().FirstOrDefault();

    static DEVMODE NewDevMode() => new() { dmSize = (ushort)Marshal.SizeOf<DEVMODE>(), dmDeviceName = "", dmFormName = "" };

    /// Imposta risoluzione (e opzionalmente posizione) di uno schermo. Ritorna false se il modo non esiste.
    public static bool SetMode(string name, int w, int h, int hz, int? x = null, int? y = null)
    {
        var dm = NewDevMode();
        EnumDisplaySettings(name, ENUM_CURRENT_SETTINGS, ref dm);
        dm.dmPelsWidth = (uint)w;
        dm.dmPelsHeight = (uint)h;
        dm.dmDisplayFrequency = (uint)hz;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        if (x != null) { dm.dmPositionX = x.Value; dm.dmPositionY = y.Value; dm.dmFields |= DM_POSITION; }
        int r = ChangeDisplaySettingsEx(name, ref dm, IntPtr.Zero, CDS_UPDATEREGISTRY | CDS_NORESET, IntPtr.Zero);
        ChangeDisplaySettingsEx(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);
        if (r != 0) Log.W($"schermo {name}: modo {w}x{h}@{hz} rifiutato ({r})");
        return r == 0;
    }

    public static bool HasMode(string name, int w, int h)
    {
        for (int i = 0; ; i++)
        {
            var dm = NewDevMode();
            if (!EnumDisplaySettings(name, i, ref dm)) return false;
            if (dm.dmPelsWidth == w && dm.dmPelsHeight == h) return true;
        }
    }

    /// Sposta lo schermo del tablet a sinistra/destra/sopra/sotto dello schermo principale.
    public static void Place(string name, string pos)
    {
        var all = List();
        var me = all.FirstOrDefault(m => m.Name == name);
        var others = all.Where(m => m.Name != name).ToList();
        var main = others.FirstOrDefault(m => m.Primary) ?? others.FirstOrDefault();
        if (me == null || main == null || pos == "auto") return;
        int l = others.Min(m => m.X), t = others.Min(m => m.Y), r = others.Max(m => m.X + m.W), b = others.Max(m => m.Y + m.H);
        (int x, int y) = pos switch
        {
            "left" => (l - me.W, main.Y + (main.H - me.H) / 2),
            "top" => (main.X + (main.W - me.W) / 2, t - me.H),
            "bottom" => (main.X + (main.W - me.W) / 2, b),
            _ => (r, main.Y + (main.H - me.H) / 2),
        };
        if (me.X == x && me.Y == y) return;
        var dm = NewDevMode();
        EnumDisplaySettings(name, ENUM_CURRENT_SETTINGS, ref dm);
        dm.dmPositionX = x;
        dm.dmPositionY = y;
        dm.dmFields = DM_POSITION;
        ChangeDisplaySettingsEx(name, ref dm, IntPtr.Zero, CDS_UPDATEREGISTRY | CDS_NORESET, IntPtr.Zero);
        ChangeDisplaySettingsEx(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero); // Windows riallinea il resto da solo
        Log.I($"posizione {name}: {pos} ({x},{y})");
    }

    // ------------------------------------------------------------ scala (100%, 125%, ...)
    static readonly int[] DpiValues = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

    static bool SourceOf(string gdiName, out LUID adapter, out uint id)
    {
        adapter = default; id = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out uint np, out uint nm) != 0) return false;
        var paths = new PATH_INFO[np];
        var modes = new MODE_INFO[nm];
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref np, paths, ref nm, modes, IntPtr.Zero) != 0) return false;
        for (int i = 0; i < np; i++)
        {
            var sn = new SOURCE_DEVICE_NAME();
            sn.h.type = 1; // GET_SOURCE_NAME
            sn.h.size = (uint)Marshal.SizeOf<SOURCE_DEVICE_NAME>();
            sn.h.adapter = paths[i].srcAdapter;
            sn.h.id = paths[i].srcId;
            if (DisplayConfigGetDeviceInfo(ref sn) == 0 && sn.gdiName == gdiName)
            {
                adapter = paths[i].srcAdapter; id = paths[i].srcId;
                return true;
            }
        }
        return false;
    }

    /// Imposta la scala dello schermo (es. 200 = 200%). API non documentata ma usata da Impostazioni di Windows.
    public static void SetScale(string gdiName, int percent)
    {
        if (!SourceOf(gdiName, out var adapter, out uint id)) return;
        var g = new DPI_GET();
        g.h.type = -3;
        g.h.size = (uint)Marshal.SizeOf<DPI_GET>();
        g.h.adapter = adapter;
        g.h.id = id;
        if (DisplayConfigGetDeviceInfo(ref g) != 0) return;
        int recIdx = Math.Abs(g.minRel);              // indice del valore consigliato
        int want = Array.IndexOf(DpiValues, DpiValues.OrderBy(v => Math.Abs(v - percent)).First());
        int rel = Math.Clamp(want - recIdx, g.minRel, g.maxRel);
        if (rel == g.curRel) return;
        var s = new DPI_SET();
        s.h.type = -4;
        s.h.size = (uint)Marshal.SizeOf<DPI_SET>();
        s.h.adapter = adapter;
        s.h.id = id;
        s.rel = rel;
        int r = DisplayConfigSetDeviceInfo(ref s);
        Log.I($"scala {gdiName}: {DpiValues[Math.Clamp(recIdx + rel, 0, DpiValues.Length - 1)]}% ({r})");
    }

    // ------------------------------------------------------------ DXGI
    /// Indice scheda video e uscita (per ffmpeg ddagrab) dello schermo GDI indicato.
    public static bool DxgiIndex(string gdiName, out int adapter, out int output, out uint vendor)
    {
        adapter = output = -1; vendor = 0;
        var iid = typeof(IDXGIFactory1).GUID;
        if (CreateDXGIFactory1(ref iid, out var f) != 0) return false;
        try
        {
            for (uint a = 0; f.EnumAdapters1(a, out var ad) == 0; a++)
            {
                ad.GetDesc(out var adesc);
                for (uint o = 0; ad.EnumOutputs(o, out var outp) == 0; o++)
                {
                    outp.GetDesc(out var od);
                    Marshal.ReleaseComObject(outp);
                    if (od.DeviceName == gdiName)
                    {
                        adapter = (int)a; output = (int)o; vendor = adesc.VendorId;
                        Marshal.ReleaseComObject(ad);
                        return true;
                    }
                }
                Marshal.ReleaseComObject(ad);
            }
        }
        finally { Marshal.ReleaseComObject(f); }
        return false;
    }

    /// Fornitore della scheda video principale: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel.
    public static uint PrimaryVendor()
    {
        var p = Primary();
        return p != null && DxgiIndex(p.Name, out _, out _, out uint v) ? v : 0;
    }
}
