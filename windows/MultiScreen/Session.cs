using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace MultiScreen;

record Params(int W, int H, int Scale, int Fps, int Kbps, bool Mirror, string Pos)
{
    public bool SameScreen(Params o) => o != null && Mirror == o.Mirror && (Mirror || (W == o.W && H == o.H && Scale == o.Scale));
}

/// Un tablet: porta propria (adb reverse), schermo virtuale, video, input, file, appunti.
sealed class Session : IDisposable
{
    readonly Host _h;
    public readonly string Serial;
    public string Model;
    public readonly int Index, Port;
    readonly TcpListener _listener;
    readonly object _tx = new();   // scrittura sul socket
    readonly object _ops = new();  // stato della sessione
    TcpClient _client;
    NetworkStream _ns;
    volatile bool _authed, _paused, _configSent, _disposed;
    Params _p;
    string _screen;                // nome GDI dello schermo usato (\\.\DISPLAYn)
    int _vddIndex = -1;
    Encoder _enc;
    DateTime _encStarted;
    Timer _grace;
    Monitor _mon;
    DateTime _monAt;
    public string ClipFromTablet;
    public readonly Files Files;
    public DateTime LastActive;

    public bool Connected => _client != null && _authed;

    public Session(Host h, string serial, string model, int index, int port)
    {
        _h = h; Serial = serial; Model = model; Index = index; Port = port;
        _listener = new TcpListener(IPAddress.Loopback, port);
        _listener.Start();
        Files = new Files(Send, () => Connected);
        new Thread(AcceptLoop) { IsBackground = true, Name = "accept-" + port }.Start();
        L($"tablet pronto sulla porta {port}");
    }

    void L(string m) => Log.I($"[{(string.IsNullOrEmpty(Model) ? Serial : Model)}] {m}");

    // ------------------------------------------------------------ rete
    void AcceptLoop()
    {
        while (!_disposed)
        {
            TcpClient c;
            try { c = _listener.AcceptTcpClient(); } catch { return; }
            c.NoDelay = true;
            c.SendBufferSize = 1 << 20;
            c.SendTimeout = 2000;
            lock (_ops)
            {
                CloseClient("nuova connessione");
                _client = c;
                _ns = c.GetStream();
                _authed = false;
                _configSent = false;
                _paused = false;
            }
            L("app collegata");
            new Thread(() => ReadLoop(c)) { IsBackground = true, Name = "read-" + Port }.Start();
        }
    }

    void ReadLoop(TcpClient c)
    {
        var s = c.GetStream();
        byte[] hdr = new byte[5];
        byte[] buf = new byte[1 << 20];
        try
        {
            while (true)
            {
                if (!ReadFull(s, hdr, 5)) break;
                int len = (int)Proto.U32(hdr, 1);
                if (len < 0 || len > (1 << 20)) break;
                if (!ReadFull(s, buf, len)) break;
                lock (_ops)
                {
                    if (_client != c) return;
                    Handle(hdr[0], buf, len);
                }
            }
        }
        catch (Exception e) { L($"lettura: {e.Message}"); }
        lock (_ops) { if (_client == c) CloseClient("il tablet ha chiuso la connessione"); }
    }

    static bool ReadFull(Stream s, byte[] b, int n)
    {
        int o = 0;
        while (o < n) { int r = s.Read(b, o, n - o); if (r <= 0) return false; o += r; }
        return true;
    }

    public bool Send(byte type, byte[] a, byte[] b)
    {
        lock (_tx)
        {
            var ns = _ns;
            if (ns == null) return false;
            int len = (a?.Length ?? 0) + (b?.Length ?? 0);
            byte[] h = { type, (byte)(len >> 24), (byte)(len >> 16), (byte)(len >> 8), (byte)len };
            try
            {
                ns.Write(h);
                if (a != null) ns.Write(a);
                if (b != null) ns.Write(b);
                return true;
            }
            catch (Exception e)
            {
                L($"invio fallito: {e.Message}");
                try { _client?.Close(); } catch { }
                return false;
            }
        }
    }

    void Status(string msg) => Send(Proto.STATUS, Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { msg })), null);

    void CloseClient(string why)
    {
        if (_client == null) return;
        L($"disconnesso: {why}");
        lock (_tx) { try { _client.Close(); } catch { } _client = null; _ns = null; }
        _authed = false;
        Files.ClientGone();
        Input.ReleaseButtons(Mon());
        StopEncoder();
        StartGrace();
    }

    // ------------------------------------------------------------ pacchetti
    void Handle(byte type, byte[] d, int len)
    {
        if (type == Proto.HELLO) { Hello(Encoding.UTF8.GetString(d, 0, len)); return; }
        if (!_authed) return;
        switch (type)
        {
            case Proto.TOUCH:
            {
                if (len < 1 || Mon() is not { } m) break;
                int n = d[0];
                if (len < 1 + n * 6) break;
                var pts = new (uint, byte, ushort, ushort)[n];
                for (int i = 0; i < n; i++)
                {
                    int o = 1 + i * 6;
                    pts[i] = (d[o], d[o + 1], Proto.U16(d, o + 2), Proto.U16(d, o + 4));
                }
                Input.Touch(m, pts);
                LastActive = DateTime.Now;
                break;
            }
            case Proto.TOUCH_CANCEL: Input.TouchCancel(); break;
            case Proto.POINTER:
                if (len >= 5 && Mon() is { } pm) Input.Pointer(pm, d[0], Proto.U16(d, 1), Proto.U16(d, 3));
                LastActive = DateTime.Now;
                break;
            case Proto.SCROLL:
                if (len >= 4) Input.Scroll((short)Proto.U16(d, 0), (short)Proto.U16(d, 2));
                break;
            case Proto.PING: Send(Proto.PONG, d[..len], null); break;
            case Proto.KEYFRAME_REQ:
                if (DateTime.Now - _encStarted > TimeSpan.FromSeconds(2)) RestartEncoder();
                break;
            case Proto.CLIPBOARD:
            {
                string t = Encoding.UTF8.GetString(d, 0, len);
                ClipFromTablet = t;
                if (t != _h.ClipFromPc) { L($"appunti: {len} byte dal tablet"); Tray.SetClipboard(t); }
                break;
            }
            case Proto.FILE_BEGIN: Files.OnBegin(Encoding.UTF8.GetString(d, 0, len)); break;
            case Proto.FILE_DATA: Files.OnData(d, len); break;
            case Proto.FILE_END: Files.OnEnd(d, len); break;
            case Proto.FILE_RESULT: Files.OnResult(Encoding.UTF8.GetString(d, 0, len)); break;
            case Proto.PAUSE:
                L("app in background: pausa video");
                _paused = true;
                Input.ReleaseButtons(Mon());
                StopEncoder();
                StartGrace();
                break;
            case Proto.BYE:
                L("disconnessione chiesta dal tablet");
                EndScreen("disconnessione");
                CloseClient("disconnesso dal tablet");
                break;
        }
    }

    // ------------------------------------------------------------ HELLO -> schermo
    static Params Compute(JsonElement r)
    {
        JsonElement p = r.TryGetProperty("prefs", out var pp) ? pp : default;
        double Num(JsonElement e, string k, double def) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(k, out var v) && v.TryGetDouble(out double x) ? x : def;
        string Str(JsonElement e, string k, string def) =>
            e.ValueKind == JsonValueKind.Object && e.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : def;
        double tw = Num(r, "width", 1920), th = Num(r, "height", 1200), dpi = Num(r, "dpi", 0), density = Num(r, "density", 2);
        double res = Math.Clamp(Num(p, "res", 75), 25, 100) / 100.0;
        string ui = Str(p, "ui", "normal");
        double div = ui == "small" ? 210 : ui == "large" ? 140 : 170;
        double native = dpi > 60 ? dpi / div : density * 0.9;
        double pw = tw * res, ph = th * res, fit = 1;
        double maxw = Num(r, "maxw", 0), maxh = Num(r, "maxh", 0);
        if (maxw > 0 && pw > maxw) fit = Math.Min(fit, maxw / pw);
        if (maxh > 0 && ph > maxh) fit = Math.Min(fit, maxh / ph);
        int w = (int)(pw * fit) / 8 * 8, h = (int)(ph * fit) / 8 * 8;
        int scale = (int)Math.Round(Math.Clamp(native * res * fit, 1, 4) * 4) * 25;
        return new Params(w, h, scale, (int)Math.Clamp(Num(p, "fps", 60), 10, 60), (int)Math.Clamp(Num(p, "bitrate", 20), 2, 150) * 1000,
            Str(p, "mode", "extend") == "mirror", Str(p, "pos", "right"));
    }

    void Hello(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var r = doc.RootElement;
        string token = r.TryGetProperty("token", out var t) ? t.GetString() : "";
        if (r.TryGetProperty("model", out var m)) Model = m.GetString();
        if (token != _h.Token)
        {
            L("codice di sicurezza sbagliato: rilancio l'app con quello giusto");
            _authed = false;
            Status("Collegamento in verifica…");
            _h.Adb?.RequestLaunch(Serial);
            return;
        }
        _authed = true;
        _paused = false;
        _configSent = false;
        LastActive = DateTime.Now;
        _grace?.Dispose();
        _grace = null;
        var p = Compute(r);
        L($"HELLO: {p}");
        var old = _p;
        _p = p;
        if (old != null && _screen != null && p.SameScreen(old) && Displays.Find(_screen) != null)
        {
            if (!p.Mirror && p.Pos != old.Pos) Displays.Place(_screen, p.Pos);
            RestartEncoder(); // nuovi fps/qualita' o ripresa: riparte solo la cattura, le finestre restano
            return;
        }
        EndScreen("nuove impostazioni");
        Status("Creo lo schermo…");
        if (!CreateScreen(p)) return;
        RestartEncoder();
    }

    bool CreateScreen(Params p)
    {
        if (p.Mirror)
        {
            _screen = Displays.Primary()?.Name;
            L($"duplica lo schermo {_screen}");
            return _screen != null;
        }
        if (!VirtualDisplay.Available)
        {
            Status("Manca il driver dello schermo virtuale: reinstalla MultiScreen");
            Tray.Notify("MultiScreen", "Manca il driver dello schermo virtuale (Parsec VDD): reinstalla MultiScreen");
            return false;
        }
        VirtualDisplay.EnsureCustomMode(p.W, p.H, 60);
        _screen = VirtualDisplay.Add(out _vddIndex);
        if (_screen == null) { Status("Windows non ha creato lo schermo"); return false; }
        if (!Displays.SetMode(_screen, p.W, p.H, 60)) L($"risoluzione {p.W}x{p.H} non disponibile, uso quella del driver");
        Displays.SetScale(_screen, p.Scale);
        Displays.Place(_screen, p.Pos);
        L($"schermo {_screen} pronto ({p.W}x{p.H}, scala {p.Scale}%)");
        return true;
    }

    void EndScreen(string why)
    {
        StopEncoder();
        if (_vddIndex >= 0)
        {
            L($"tolgo lo schermo ({why})");
            VirtualDisplay.Remove(_vddIndex);
        }
        _vddIndex = -1;
        _screen = null;
        _p = null;
    }

    void StartGrace()
    {
        if (_screen == null || _grace != null) return;
        _grace = new Timer(_ =>
        {
            lock (_ops) { if (!Connected || _paused) { _grace = null; EndScreen("tablet non piu' in uso"); } }
        }, null, _h.GraceSeconds * 1000, Timeout.Infinite);
    }

    Monitor Mon()
    {
        if (_screen == null) return null;
        if (_mon == null || _mon.Name != _screen || DateTime.Now - _monAt > TimeSpan.FromSeconds(1))
        {
            _mon = Displays.Find(_screen);
            _monAt = DateTime.Now;
        }
        return _mon;
    }

    // ------------------------------------------------------------ video
    void RestartEncoder()
    {
        StopEncoder();
        if (_screen == null || _p == null || !Connected || _paused) return;
        int adapter = -1, output = -1;
        uint vendor = 0;
        for (int i = 0; i < 30 && !Displays.DxgiIndex(_screen, out adapter, out output, out vendor); i++) Thread.Sleep(100);
        if (output < 0) { L($"non trovo {_screen} tra le uscite DXGI"); Status("Errore: non riesco a catturare lo schermo"); return; }
        var mon = Displays.Find(_screen);
        int w = mon?.W ?? _p.W, h = mon?.H ?? _p.H;
        _configSent = false;
        var enc = new Encoder(adapter, output, w, h, _p.Fps, _p.Kbps, (data, key) => OnFrame(data, key, w, h));
        _encStarted = DateTime.Now;
        _enc = enc;
        if (!enc.Start(vendor))
        {
            L("nessun encoder video funziona");
            Status("Errore video sul PC: guarda il registro di MultiScreen");
            _enc = null;
            enc.Dispose();
        }
    }

    void StopEncoder()
    {
        var e = _enc;
        _enc = null;
        e?.Dispose();
    }

    void OnFrame(byte[] data, bool key, int w, int h)
    {
        if (!Connected || _paused) return;
        if (!_configSent)
        {
            if (!key) return;
            string j = JsonSerializer.Serialize(new { codec = "h264", width = w, height = h, fps = _p?.Fps ?? 60 });
            if (!Send(Proto.CONFIG, Encoding.UTF8.GetBytes(j), null)) return;
            _configSent = true;
            L($"invio video {w}x{h}");
        }
        byte[] vh = new byte[9];
        vh[0] = (byte)(key ? 1 : 0);
        Proto.Put64(vh, 1, (ulong)(Environment.TickCount64 * 1000));
        Send(Proto.VIDEO, vh, data);
    }

    /// Chiamato ogni 2 s: se la cattura si e' fermata da sola (es. driver riavviato) la rimette in piedi.
    public void Watch()
    {
        lock (_ops)
        {
            if (!Connected || _paused || _p == null || _disposed) return;
            if (_enc != null && _enc.Running) return;
            if (DateTime.Now - _encStarted < TimeSpan.FromSeconds(5)) return;
            if (_screen == null || Displays.Find(_screen) == null)
            {
                L("lo schermo e' sparito: lo ricreo");
                var p = _p;
                _vddIndex = -1;
                _screen = null;
                if (!CreateScreen(p)) return;
                _p = p;
            }
            RestartEncoder();
        }
    }

    // ------------------------------------------------------------ dal resto del programma
    public void PcClipboard(string text)
    {
        if (Connected && text != ClipFromTablet) Send(Proto.CLIPBOARD_OUT, Encoding.UTF8.GetBytes(text), null);
    }

    public void Dispose()
    {
        _disposed = true;
        lock (_ops)
        {
            CloseClient("cavo scollegato");
            _grace?.Dispose();
            EndScreen("cavo scollegato");
        }
        try { _listener.Stop(); } catch { }
        Files.Dispose();
    }
}
