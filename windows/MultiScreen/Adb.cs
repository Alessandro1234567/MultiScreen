using System.Diagnostics;
using System.Text;

namespace MultiScreen;

/// Tablet collegati via USB: installa/aggiorna l'app, attiva "adb reverse", avvia l'app con il codice segreto.
sealed class Adb
{
    public static string AdbPath => Path.Combine(AppContext.BaseDirectory, "tools", "platform-tools", "adb.exe");
    const string Package = "com.zago.multiscreen";

    readonly Host _h;
    readonly Dictionary<string, Dev> _devs = new();
    readonly HashSet<string> _launch = new();

    class Dev { public string State = "", Model = ""; public bool Handled, Ignored, Warned; public DateTime LastCheck; }

    public Adb(Host h) { _h = h; }

    public void Start() => new Thread(Loop) { IsBackground = true, Name = "adb" }.Start();

    public void RequestLaunch(string serial) { lock (_launch) _launch.Add(serial); }

    public static string Run(string serial, int timeoutS, out int code, params string[] args)
    {
        code = -1;
        try
        {
            var psi = new ProcessStartInfo(AdbPath)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
            };
            if (serial != null) { psi.ArgumentList.Add("-s"); psi.ArgumentList.Add(serial); }
            foreach (var a in args) psi.ArgumentList.Add(a);
            using var p = Process.Start(psi);
            var err = p.StandardError.ReadToEndAsync();
            var outp = p.StandardOutput.ReadToEndAsync();
            if (!p.WaitForExit(timeoutS * 1000)) { try { p.Kill(true); } catch { } return ""; }
            code = p.ExitCode;
            return outp.Result + err.Result;
        }
        catch (Exception e) { Log.W($"adb: {e.Message}"); return ""; }
    }

    static string Run(string serial, int timeoutS, params string[] args) => Run(serial, timeoutS, out _, args);

    void Loop()
    {
        if (!File.Exists(AdbPath)) { Log.E($"adb non trovato in {AdbPath}: rilancia l'installazione"); Tray.Notify("MultiScreen", "Manca adb: reinstalla MultiScreen"); return; }
        Run(null, 15, "start-server");
        while (true)
        {
            try { Poll(); } catch (Exception e) { Log.W($"adb: {e.Message}"); }
            Thread.Sleep(1000);
        }
    }

    void Poll()
    {
        string[] launch;
        lock (_launch) { launch = _launch.ToArray(); _launch.Clear(); }
        foreach (var s in launch) { if (_devs.TryGetValue(s, out var d) && d.Handled) { EnsureReverse(s); LaunchApp(s); } }

        string o = Run(null, 8, "devices");
        var seen = new HashSet<string>();
        foreach (var raw in o.Split('\n'))
        {
            var l = raw.Trim();
            if (l.Length == 0 || l.StartsWith("List of") || l.StartsWith("*")) continue;
            var f = l.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (f.Length < 2) continue;
            string serial = f[0], state = f[1];
            seen.Add(serial);
            if (!_devs.TryGetValue(serial, out var d)) { d = new Dev(); _devs[serial] = d; Log.I($"adb: visto {serial} ({state})"); }
            if (d.State != state) { d.State = state; Log.I($"adb: {serial} -> {state}"); }
            if (state == "unauthorized" && !d.Warned)
            {
                d.Warned = true;
                Tray.Notify("MultiScreen", "Sul tablet tocca «Consenti» per il debug USB (spunta «Consenti sempre»).");
            }
            if (state == "device" && !d.Handled && !d.Ignored) Prepare(serial, d);
            else if (d.Handled && DateTime.Now - d.LastCheck > TimeSpan.FromSeconds(5)) { d.LastCheck = DateTime.Now; EnsureReverse(serial); }
        }
        foreach (var s in _devs.Keys.Where(k => !seen.Contains(k)).ToList())
        {
            Log.I($"adb: {s} scollegato");
            if (_devs[s].Handled) _h.DeviceGone(s);
            _devs.Remove(s);
        }
    }

    void Prepare(string serial, Dev d)
    {
        d.Model = Run(serial, 5, "shell", "getprop", "ro.product.model").Trim();
        int ver = -1;
        string dump = Run(serial, 10, "shell", "dumpsys", "package", Package);
        int i = dump.IndexOf("versionCode=", StringComparison.Ordinal);
        if (i >= 0) int.TryParse(new string(dump[(i + 12)..].TakeWhile(char.IsDigit).ToArray()), out ver);
        Log.I($"adb: {serial} ({d.Model}) app {ver}, disponibile {_h.ApkVersion}");
        if (ver < 0)
        {
            if (!Run(serial, 5, "shell", "getprop", "ro.build.characteristics").Contains("tablet"))
            {
                Log.I($"adb: {serial} non e' un tablet e non ha l'app: lo ignoro");
                d.Ignored = true;
                return;
            }
            if (!Install(serial)) { d.Ignored = true; return; }
        }
        else if (ver < _h.ApkVersion) Install(serial);
        if (!EnsureReverse(serial)) return;
        d.Handled = true;
        _h.DeviceReady(serial, d.Model);
        LaunchApp(serial);
    }

    bool Install(string serial)
    {
        if (!File.Exists(_h.ApkPath)) { Log.E($"APK mancante: {_h.ApkPath}"); return false; }
        Tray.Notify("MultiScreen", "Installo l'app sul tablet… (se il tablet chiede conferma, tocca Installa)");
        string o = Run(serial, 120, out int c, "install", "-r", "-g", _h.ApkPath);
        bool ok = c == 0 && o.Contains("Success");
        if (!ok && o.Contains("INSTALL_FAILED_UPDATE_INCOMPATIBLE"))
        {
            Run(serial, 30, "uninstall", Package);
            o = Run(serial, 120, out c, "install", "-g", _h.ApkPath);
            ok = c == 0 && o.Contains("Success");
        }
        if (!ok)
        {
            Log.W($"adb install: {o}");
            Run(serial, 60, "push", _h.ApkPath, "/sdcard/Download/MultiScreen.apk");
            Tray.Notify("MultiScreen: app da installare",
                "Il tablet ha bloccato l'installazione via USB. Attiva «Installa tramite USB» oppure apri Download/MultiScreen.apk sul tablet.");
        }
        return ok;
    }

    bool EnsureReverse(string serial)
    {
        int port = _h.PortFor(serial);
        string want = $"tcp:{Proto.DevicePort} tcp:{port}";
        if (Run(serial, 5, "reverse", "--list").Contains(want)) return true;
        Run(serial, 5, out int c, "reverse", $"tcp:{Proto.DevicePort}", $"tcp:{port}");
        Log.I($"adb: reverse {want} su {serial}: {(c == 0 ? "ok" : "FALLITO")}");
        return c == 0;
    }

    void LaunchApp(string serial)
    {
        Run(serial, 10, "shell", "am", "start", "-n", Package + "/.MainActivity", "--es", "token", _h.Token);
        Log.I($"adb: avvio app su {serial}");
    }
}
