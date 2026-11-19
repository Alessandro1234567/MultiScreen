using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using static MultiScreen.Native;

namespace MultiScreen;

/// Icona vicino all'orologio, notifiche e appunti di Windows (tutto su una finestra nascosta, thread STA).
static class Tray
{
    const uint WM_TRAY = WM_APP + 1, WM_SETCLIP = WM_APP + 2, WM_BALLOON = WM_APP + 3, NIN_BALLOONUSERCLICK = 0x405;
    static IntPtr _hwnd;
    static WndProc _proc; // tenuto vivo per il GC
    static readonly ConcurrentQueue<string> ClipQ = new();
    static readonly ConcurrentQueue<(string, string)> BalloonQ = new();
    static string _lastSet;
    public static event Action<string> ClipboardChanged;
    public static Func<string> Status = () => "";
    public static string ReceivedDir = "";

    /// Gira il ciclo dei messaggi (blocca fino a "Esci").
    public static void Run()
    {
        _proc = Proc;
        var wc = new WNDCLASSEX
        {
            cbSize = (uint)Marshal.SizeOf<WNDCLASSEX>(), lpfnWndProc = _proc, hInstance = GetModuleHandle(IntPtr.Zero),
            lpszClassName = "MultiScreenTray",
        };
        RegisterClassEx(ref wc);
        _hwnd = CreateWindowEx(0, "MultiScreenTray", "MultiScreen", 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        var nid = Nid();
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        Shell_NotifyIcon(NIM_ADD, ref nid);
        AddClipboardFormatListener(_hwnd);
        while (GetMessage(out var m, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref m);
            DispatchMessage(ref m);
        }
        var d = Nid();
        Shell_NotifyIcon(NIM_DELETE, ref d);
    }

    static NOTIFYICONDATA Nid()
    {
        IntPtr icon = ExtractIcon(IntPtr.Zero, Environment.ProcessPath ?? "", 0);
        if (icon == IntPtr.Zero || icon == new IntPtr(1)) icon = LoadIcon(IntPtr.Zero, new IntPtr(32512));
        return new NOTIFYICONDATA
        {
            cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATA>(), hWnd = _hwnd, uID = 1, uCallbackMessage = WM_TRAY,
            hIcon = icon, szTip = "MultiScreen", szInfo = "", szInfoTitle = "",
        };
    }

    public static void Notify(string title, string body)
    {
        BalloonQ.Enqueue((title, body));
        if (_hwnd != IntPtr.Zero) PostMessage(_hwnd, WM_BALLOON, IntPtr.Zero, IntPtr.Zero);
    }

    public static void SetClipboard(string text)
    {
        ClipQ.Enqueue(text);
        if (_hwnd != IntPtr.Zero) PostMessage(_hwnd, WM_SETCLIP, IntPtr.Zero, IntPtr.Zero);
    }

    static IntPtr Proc(IntPtr h, uint msg, IntPtr w, IntPtr l)
    {
        switch (msg)
        {
            case WM_TRAY:
                uint ev = (uint)(l.ToInt64() & 0xFFFF);
                if (ev == WM_RBUTTONUP || ev == WM_LBUTTONUP || ev == WM_CONTEXTMENU) Menu();
                else if (ev == NIN_BALLOONUSERCLICK && ReceivedDir != "") Open(ReceivedDir);
                return IntPtr.Zero;
            case WM_CLIPBOARDUPDATE:
                if (GetClipboardOwner() != _hwnd)
                {
                    string t = ReadClipboard();
                    if (!string.IsNullOrEmpty(t) && t != _lastSet) ClipboardChanged?.Invoke(t);
                }
                return IntPtr.Zero;
            case WM_SETCLIP:
                while (ClipQ.TryDequeue(out var text)) WriteClipboard(text);
                return IntPtr.Zero;
            case WM_BALLOON:
                while (BalloonQ.TryDequeue(out var b))
                {
                    var nid = Nid();
                    nid.uFlags = NIF_INFO;
                    nid.szInfoTitle = b.Item1.Length > 63 ? b.Item1[..63] : b.Item1;
                    nid.szInfo = b.Item2.Length > 255 ? b.Item2[..255] : b.Item2;
                    nid.dwInfoFlags = NIIF_INFO;
                    Shell_NotifyIcon(NIM_MODIFY, ref nid);
                }
                return IntPtr.Zero;
        }
        return DefWindowProc(h, msg, w, l);
    }

    static void Menu()
    {
        IntPtr m = CreatePopupMenu();
        AppendMenu(m, MF_STRING | MF_GRAYED, (UIntPtr)1, "MultiScreen — " + Status());
        AppendMenu(m, MF_SEPARATOR, UIntPtr.Zero, null);
        AppendMenu(m, MF_STRING, (UIntPtr)2, "Apri i file ricevuti");
        AppendMenu(m, MF_STRING, (UIntPtr)3, "Apri il registro (log)");
        AppendMenu(m, MF_SEPARATOR, UIntPtr.Zero, null);
        AppendMenu(m, MF_STRING, (UIntPtr)4, "Esci");
        GetCursorPos(out var p);
        SetForegroundWindow(_hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, _hwnd, IntPtr.Zero);
        DestroyMenu(m);
        if (cmd == 2) { Directory.CreateDirectory(ReceivedDir); Open(ReceivedDir); }
        else if (cmd == 3) Open(Log.FilePath);
        else if (cmd == 4) PostQuitMessage(0);
    }

    public static void Open(string path)
    {
        try { Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }); } catch { }
    }

    static string ReadClipboard()
    {
        for (int i = 0; i < 10; i++)
        {
            if (OpenClipboard(_hwnd))
            {
                try
                {
                    IntPtr hd = GetClipboardData(CF_UNICODETEXT);
                    if (hd == IntPtr.Zero) return null;
                    IntPtr p = GlobalLock(hd);
                    try { return Marshal.PtrToStringUni(p); } finally { GlobalUnlock(hd); }
                }
                finally { CloseClipboard(); }
            }
            Thread.Sleep(20);
        }
        return null;
    }

    static void WriteClipboard(string text)
    {
        for (int i = 0; i < 10; i++)
        {
            if (!OpenClipboard(_hwnd)) { Thread.Sleep(20); continue; }
            try
            {
                EmptyClipboard();
                int bytes = (text.Length + 1) * 2;
                IntPtr hg = GlobalAlloc(GMEM_MOVEABLE, (UIntPtr)bytes);
                IntPtr p = GlobalLock(hg);
                Marshal.Copy(text.ToCharArray(), 0, p, text.Length);
                Marshal.WriteInt16(p, text.Length * 2, 0);
                GlobalUnlock(hg);
                SetClipboardData(CF_UNICODETEXT, hg);
                _lastSet = text;
            }
            finally { CloseClipboard(); }
            return;
        }
    }
}
