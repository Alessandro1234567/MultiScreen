using System.Net;
using System.Net.Sockets;
using System.Text;

namespace MultiScreen;

/// Stato globale: un Session per tablet, appunti, "Invia al tablet".
sealed class Host
{
    public readonly string Token;
    public readonly string ApkPath = Path.Combine(AppContext.BaseDirectory, "multiscreen.apk");
    public readonly int ApkVersion;
    public readonly int GraceSeconds = 30;
    public Adb Adb;
    public string ClipFromPc;
    readonly object _lock = new();
    readonly Dictionary<string, Session> _sessions = new();
    readonly Dictionary<string, int> _ports = new();
    int _nextPort = Proto.DevicePort;

    public static string ConfigDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MultiScreen");

    public Host()
    {
        Directory.CreateDirectory(ConfigDir);
        string tf = Path.Combine(ConfigDir, "token");
        string t = File.Exists(tf) ? File.ReadAllText(tf).Trim() : "";
        if (t.Length < 16)
        {
            t = Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
            File.WriteAllText(tf, t);
        }
        Token = t;
        string vf = Path.Combine(AppContext.BaseDirectory, "apk-version");
        if (File.Exists(vf)) int.TryParse(File.ReadAllText(vf).Trim(), out ApkVersion);
    }

    public void Start()
    {
        Tray.ReceivedDir = Files.Dir;
        Tray.Status = () =>
        {
            lock (_lock)
            {
                if (_sessions.Count == 0) return "nessun tablet collegato";
                return string.Join(", ", _sessions.Values.Select(s => $"{s.Model} {(s.Connected ? "in uso" : "collegato")}"));
            }
        };
        Tray.ClipboardChanged += text =>
        {
            if (text == ClipFromPc) return;
            ClipFromPc = text;
            foreach (var s in Snapshot()) s.PcClipboard(text);
        };
        Adb = new Adb(this);
        Adb.Start();
        new Thread(ControlLoop) { IsBackground = true, Name = "control" }.Start();
        new Timer(_ => { foreach (var s in Snapshot()) s.Watch(); }, null, 2000, 2000);
    }

    List<Session> Snapshot() { lock (_lock) return _sessions.Values.ToList(); }

    public int PortFor(string serial)
    {
        lock (_lock)
        {
            if (!_ports.TryGetValue(serial, out int p)) { p = _nextPort++; _ports[serial] = p; }
            return p;
        }
    }

    public void DeviceReady(string serial, string model)
    {
        lock (_lock)
        {
            if (_sessions.ContainsKey(serial)) return;
            int index = 1;
            while (_sessions.Values.Any(s => s.Index == index)) index++;
            try { _sessions[serial] = new Session(this, serial, model, index, PortFor(serial)); }
            catch (Exception e) { Log.E($"porta occupata per {serial}: {e.Message}"); Tray.Notify("MultiScreen", "Porta occupata: riavvia il PC"); }
        }
    }

    public void DeviceGone(string serial)
    {
        Session s;
        lock (_lock) { if (!_sessions.Remove(serial, out s)) return; }
        s.Dispose();
    }

    // ------------------------------------------------------------ "Invia al tablet" (127.0.0.1:27182, con token)
    void ControlLoop()
    {
        TcpListener l;
        try { l = new TcpListener(IPAddress.Loopback, Proto.ControlPort); l.Start(); }
        catch (Exception e) { Log.W($"porta di controllo: {e.Message}"); return; }
        while (true)
        {
            try
            {
                using var c = l.AcceptTcpClient();
                c.ReceiveTimeout = 3000;
                using var r = new StreamReader(c.GetStream(), Encoding.UTF8);
                using var w = new StreamWriter(c.GetStream(), new UTF8Encoding(false)) { AutoFlush = true };
                if (r.ReadLine() != "TOKEN " + Token) continue;
                string line;
                while ((line = r.ReadLine()) != null && line.Length > 0)
                {
                    if (!line.StartsWith("SEND ")) continue;
                    var target = Snapshot().Where(s => s.Connected).OrderByDescending(s => s.LastActive).FirstOrDefault();
                    string err = target == null ? "nessun tablet collegato" : target.Files.Queue(line[5..]);
                    w.WriteLine(err == null ? $"OK {target.Model}" : "ERR " + err);
                }
            }
            catch (Exception e) { Log.W($"controllo: {e.Message}"); }
        }
    }

    public static int SendFiles(string[] paths)
    {
        try
        {
            string token = File.ReadAllText(Path.Combine(ConfigDir, "token")).Trim();
            using var c = new TcpClient();
            c.Connect(IPAddress.Loopback, Proto.ControlPort);
            using var w = new StreamWriter(c.GetStream(), new UTF8Encoding(false)) { AutoFlush = true };
            using var r = new StreamReader(c.GetStream(), Encoding.UTF8);
            w.WriteLine("TOKEN " + token);
            var files = paths.SelectMany(p => Directory.Exists(p) ? Directory.GetFiles(p) : new[] { p }).ToList();
            foreach (var f in files) w.WriteLine("SEND " + Path.GetFullPath(f));
            w.WriteLine();
            int rc = 0;
            for (int i = 0; i < files.Count; i++)
            {
                string resp = r.ReadLine() ?? "ERR nessuna risposta";
                if (resp.StartsWith("ERR")) { rc = 1; Native.MessageBox(IntPtr.Zero, "Invio non riuscito: " + resp[4..], "MultiScreen", 0x30); break; }
            }
            return rc;
        }
        catch
        {
            Native.MessageBox(IntPtr.Zero, "MultiScreen non è in esecuzione. Riavvia il PC o apri MultiScreen dal menu Start.", "MultiScreen", 0x30);
            return 1;
        }
    }
}
