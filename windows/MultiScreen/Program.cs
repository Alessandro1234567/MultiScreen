namespace MultiScreen;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        Native.SetProcessDpiAwarenessContext(new IntPtr(-4)); // per-monitor v2: coordinate in pixel reali
        if (args.Length > 0 && args[0] == "--setup") return Setup.Run(args.Skip(1).ToArray());
        if (args.Length > 0 && args[0] == "--send") return Host.SendFiles(args.Skip(1).ToArray());

        using var single = new Mutex(true, @"Local\MultiScreenHost", out bool first);
        if (!first) return 0; // gia' in esecuzione
        Log.Init();
        Log.I($"MultiScreen {typeof(Program).Assembly.GetName().Version} avviato");
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Log.E($"errore: {e.ExceptionObject}");
        var host = new Host();
        host.Start();
        Tray.Notify("MultiScreen è attivo", "Collega il tablet con il cavo USB");
        Tray.Run(); // fino a "Esci"
        Log.I("chiuso");
        return 0;
    }
}
