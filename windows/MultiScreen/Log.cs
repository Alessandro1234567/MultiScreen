namespace MultiScreen;

static class Log
{
    static readonly object Lock = new();
    static StreamWriter _w;
    public static string Dir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MultiScreen");
    public static string FilePath => Path.Combine(Dir, "host.log");

    public static void Init(string name = "host.log")
    {
        Directory.CreateDirectory(Dir);
        string p = Path.Combine(Dir, name);
        try { if (File.Exists(p)) File.Copy(p, p + ".1", true); } catch { }
        _w = new StreamWriter(new FileStream(p, FileMode.Create, FileAccess.Write, FileShare.ReadWrite)) { AutoFlush = true };
    }

    public static void I(string m) => Write("INFO", m);
    public static void W(string m) => Write("WARN", m);
    public static void E(string m) => Write("ERRO", m);

    static void Write(string lvl, string m)
    {
        lock (Lock) { _w?.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {lvl} {m}"); }
    }
}
