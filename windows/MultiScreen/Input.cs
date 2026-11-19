using System.Runtime.InteropServices;
using static MultiScreen.Native;

namespace MultiScreen;

/// Inietta touch (Windows 8+) e mouse nelle coordinate dello schermo del tablet.
static class Input
{
    static readonly object Lock = new();
    static bool _touchReady;
    // contatti attivi: Windows vuole tutti i contatti in ogni iniezione
    static readonly Dictionary<uint, POINT> Active = new();
    static int _buttons;

    public static void Touch(Monitor m, (uint id, byte state, ushort x, ushort y)[] pts)
    {
        lock (Lock)
        {
            if (!_touchReady) _touchReady = InitializeTouchInjection(10, 1);
            if (!_touchReady) return;
            var changed = new Dictionary<uint, (byte state, POINT p)>();
            foreach (var t in pts) changed[t.id % 10] = (t.state, Map(m, t.x, t.y)); // Windows: id 0..9
            var list = new List<POINTER_TOUCH_INFO>();
            foreach (var (id, ch) in changed)
                list.Add(Contact(id, ch.p, ch.state == 0 ? POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
                    : ch.state == 1 ? POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT : POINTER_FLAG_UP));
            foreach (var (id, p) in Active)
                if (!changed.ContainsKey(id)) list.Add(Contact(id, p, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT));
            foreach (var (id, ch) in changed)
            {
                if (ch.state == 2) Active.Remove(id);
                else Active[id] = ch.p;
            }
            if (list.Count > 0 && !InjectTouchInput((uint)list.Count, list.ToArray()))
                Log.W($"touch: iniezione fallita ({Marshal.GetLastWin32Error()})");
        }
    }

    public static void TouchCancel()
    {
        lock (Lock)
        {
            if (!_touchReady || Active.Count == 0) return;
            var list = Active.Select(a => Contact(a.Key, a.Value, POINTER_FLAG_UP | POINTER_FLAG_CANCELED)).ToArray();
            Active.Clear();
            InjectTouchInput((uint)list.Length, list);
        }
    }

    static POINTER_TOUCH_INFO Contact(uint id, POINT p, uint flags)
    {
        var c = new POINTER_TOUCH_INFO();
        c.pointerInfo.pointerType = PT_TOUCH;
        c.pointerInfo.pointerId = id;
        c.pointerInfo.ptPixelLocation = p;
        c.pointerInfo.pointerFlags = flags;
        c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        c.rcContact = new RECT { left = p.x - 2, top = p.y - 2, right = p.x + 2, bottom = p.y + 2 };
        c.pressure = 512;
        return c;
    }

    static POINT Map(Monitor m, ushort nx, ushort ny) =>
        new() { x = m.X + (int)(nx / 65535.0 * (m.W - 1)), y = m.Y + (int)(ny / 65535.0 * (m.H - 1)) };

    /// Mouse assoluto (penna, modalita' mouse). buttons: bit0 sinistro, bit1 destro, bit2 centrale.
    public static void Pointer(Monitor m, int buttons, ushort nx, ushort ny)
    {
        lock (Lock)
        {
            var p = Map(m, nx, ny);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = Math.Max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1), vh = Math.Max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
            uint flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            int ch = buttons ^ _buttons;
            if ((ch & 1) != 0) flags |= (buttons & 1) != 0 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            if ((ch & 2) != 0) flags |= (buttons & 2) != 0 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            if ((ch & 4) != 0) flags |= (buttons & 4) != 0 ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            _buttons = buttons;
            Send(new MOUSEINPUT { dx = (int)((p.x - vx) * 65535L / vw), dy = (int)((p.y - vy) * 65535L / vh), dwFlags = flags });
        }
    }

    public static void ReleaseButtons(Monitor m)
    {
        if (_buttons != 0 && m != null) Pointer(m, 0, 0, 0);
        TouchCancel();
    }

    /// Scorrimento: dx/dy in decimi di dp del tablet (positivo = contenuto che va giu'/destra).
    public static void Scroll(short dx, short dy)
    {
        if (dy != 0) Send(new MOUSEINPUT { mouseData = unchecked((uint)(-dy * 3 / 10)), dwFlags = MOUSEEVENTF_WHEEL });
        if (dx != 0) Send(new MOUSEINPUT { mouseData = unchecked((uint)(dx * 3 / 10)), dwFlags = MOUSEEVENTF_HWHEEL });
    }

    static void Send(MOUSEINPUT mi) =>
        SendInput(1, new[] { new INPUT { type = 0, mi = mi } }, Marshal.SizeOf<INPUT>());
}
