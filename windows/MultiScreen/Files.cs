using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace MultiScreen;

/// File tra PC e tablet. Ricevuti -> Download\MultiScreen.
sealed class Files : IDisposable
{
    const int Chunk = 64 * 1024;
    readonly Func<byte, byte[], byte[], bool> _send;   // (tipo, intestazione, dati)
    readonly Func<bool> _connected;
    readonly Dictionary<uint, (FileStream fs, string tmp, string final, string name)> _in = new();
    readonly BlockingCollection<string> _out = new();
    readonly Thread _sender;
    static int _nextId = 1;
    static int _recvCount;
    static string _recvLast;
    static Timer _recvTimer;

    public static string Dir
    {
        get
        {
            var g = new Guid("374DE290-123F-4565-9164-39C4925E467B"); // Download
            string root = Native.SHGetKnownFolderPath(ref g, 0, IntPtr.Zero, out IntPtr p) == 0 ? Marshal.PtrToStringUni(p)
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
            Marshal.FreeCoTaskMem(p);
            return Path.Combine(root, "MultiScreen");
        }
    }

    public Files(Func<byte, byte[], byte[], bool> send, Func<bool> connected)
    {
        _send = send;
        _connected = connected;
        _sender = new Thread(SendLoop) { IsBackground = true, Name = "files" };
        _sender.Start();
    }

    // ------------------------------------------------------------ dal tablet
    public void OnBegin(string json)
    {
        using var j = JsonDocument.Parse(json);
        uint id = j.RootElement.TryGetProperty("id", out var v) ? v.GetUInt32() : 0;
        string name = j.RootElement.TryGetProperty("name", out var n) ? n.GetString() : "file";
        name = string.Concat(Path.GetFileName(name ?? "file").Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c));
        if (name is "" or "." or "..") name = "file";
        Directory.CreateDirectory(Dir);
        string final = Unique(Path.Combine(Dir, name));
        string tmp = final + ".part";
        try { _in[id] = (new FileStream(tmp, FileMode.Create), tmp, final, name); Log.I($"file: ricevo {name}"); }
        catch (Exception e) { Log.E($"file: {e.Message}"); }
    }

    public void OnData(byte[] d, int len)
    {
        if (len < 4 || !_in.TryGetValue(Proto.U32(d, 0), out var f)) return;
        try { f.fs.Write(d, 4, len - 4); } catch { }
    }

    public void OnEnd(byte[] d, int len)
    {
        if (len < 5) return;
        uint id = Proto.U32(d, 0);
        if (!_in.Remove(id, out var f)) return;
        bool saved = false;
        try
        {
            f.fs.Dispose();
            if (d[4] != 0) { File.Move(f.tmp, f.final); saved = true; }
            else File.Delete(f.tmp);
        }
        catch (Exception e) { Log.W($"file: {e.Message}"); }
        string name = Path.GetFileName(f.final);
        string j = JsonSerializer.Serialize(new { id, name, ok = saved, where = @"Download\MultiScreen" });
        _send(Proto.FILE_RESULT_OUT, Encoding.UTF8.GetBytes(j), null);
        if (saved)
        {
            Log.I($"file: salvato {f.final}");
            Interlocked.Increment(ref _recvCount);
            _recvLast = name;
            _recvTimer?.Dispose();
            _recvTimer = new Timer(_ =>
            {
                int n = Interlocked.Exchange(ref _recvCount, 0);
                Tray.Notify(n == 1 ? $"Ricevuto dal tablet: {_recvLast}" : $"Ricevuti {n} file dal tablet", "Clicca per aprire la cartella");
            }, null, 1200, Timeout.Infinite);
        }
    }

    public static void OnResult(string json)
    {
        using var j = JsonDocument.Parse(json);
        bool ok = j.RootElement.TryGetProperty("ok", out var v) && v.GetBoolean();
        string name = j.RootElement.TryGetProperty("name", out var n) ? n.GetString() : "";
        Tray.Notify(ok ? $"Inviato al tablet: {name}" : $"Invio non riuscito: {name}", ok ? "Sul tablet: Download/MultiScreen" : "");
    }

    public void ClientGone()
    {
        foreach (var f in _in.Values) { try { f.fs.Dispose(); File.Delete(f.tmp); } catch { } }
        _in.Clear();
    }

    static string Unique(string p)
    {
        if (!File.Exists(p)) return p;
        string dir = Path.GetDirectoryName(p), stem = Path.GetFileNameWithoutExtension(p), ext = Path.GetExtension(p);
        for (int i = 1; ; i++) { string q = Path.Combine(dir, $"{stem} ({i}){ext}"); if (!File.Exists(q)) return q; }
    }

    // ------------------------------------------------------------ al tablet
    public string Queue(string path)
    {
        if (!_connected()) return "tablet non collegato";
        if (!File.Exists(path)) return "non e' un file: " + path;
        _out.Add(path);
        return null;
    }

    void SendLoop()
    {
        foreach (string path in _out.GetConsumingEnumerable())
        {
            uint id = (uint)Interlocked.Increment(ref _nextId);
            string name = Path.GetFileName(path);
            bool ok;
            try
            {
                using var fs = File.OpenRead(path);
                string j = JsonSerializer.Serialize(new { id, name, size = fs.Length, mime = "application/octet-stream" });
                ok = _send(Proto.FILE_BEGIN_OUT, Encoding.UTF8.GetBytes(j), null);
                byte[] idb = new byte[4];
                Proto.Put32(idb, 0, id);
                byte[] buf = new byte[Chunk];
                int n;
                while (ok && (n = fs.Read(buf, 0, Chunk)) > 0) ok = _send(Proto.FILE_DATA_OUT, idb, buf[..n]);
                _send(Proto.FILE_END_OUT, new byte[] { idb[0], idb[1], idb[2], idb[3], (byte)(ok ? 1 : 0) }, null);
                Log.I($"file: inviato {name} ({(ok ? "ok" : "interrotto")})");
            }
            catch (Exception e) { ok = false; Log.W($"file: {e.Message}"); }
            if (!ok) Tray.Notify($"Invio non riuscito: {name}", "Il tablet si e' scollegato");
        }
    }

    public void Dispose()
    {
        _out.CompleteAdding();
        ClientGone();
    }
}
