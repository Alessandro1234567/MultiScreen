using System.Diagnostics;
using System.IO.Compression;

namespace MultiScreen;

/// Eseguito dall'installazione (come amministratore): scarica adb e ffmpeg, installa il driver dello schermo virtuale,
/// registra l'avvio automatico.
static class Setup
{
    const string PlatformTools = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip";
    const string Ffmpeg = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
    const string ParsecVdd = "https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe";

    public static int Run(string[] args)
    {
        Log.Init("setup.log");
        string tools = Path.Combine(AppContext.BaseDirectory, "tools");
        Directory.CreateDirectory(tools);
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(15) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("MultiScreen-Setup");
        try
        {
            if (!File.Exists(Adb.AdbPath))
            {
                Log.I("scarico adb (platform-tools)");
                string zip = Download(http, PlatformTools);
                ZipFile.ExtractToDirectory(zip, tools, true);
                File.Delete(zip);
            }
            if (!File.Exists(Encoder.FfmpegPath))
            {
                Log.I("scarico ffmpeg");
                string zip = Download(http, Ffmpeg);
                string tmp = Path.Combine(Path.GetTempPath(), "ms-ffmpeg");
                if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
                ZipFile.ExtractToDirectory(zip, tmp);
                string exe = Directory.GetFiles(tmp, "ffmpeg.exe", SearchOption.AllDirectories).First();
                File.Copy(exe, Encoder.FfmpegPath, true);
                Directory.Delete(tmp, true);
                File.Delete(zip);
            }
            if (!VirtualDisplay.Available)
            {
                Log.I("installo il driver dello schermo virtuale (Parsec VDD)");
                string exe = Download(http, ParsecVdd, ".exe");
                using (var p = Process.Start(new ProcessStartInfo(exe, "/S") { UseShellExecute = false }))
                    p.WaitForExit(180000);
                File.Delete(exe);
                for (int i = 0; i < 20 && !VirtualDisplay.Available; i++) Thread.Sleep(500);
                if (!VirtualDisplay.Available) Log.W("driver installato ma non ancora visibile (potrebbe servire riavviare)");
            }
            RegisterAutostart();
            Log.I("installazione completata");
            return 0;
        }
        catch (Exception e)
        {
            Log.E($"installazione: {e}");
            Native.MessageBox(IntPtr.Zero, "Non sono riuscito a scaricare tutti i componenti di MultiScreen.\n" +
                "Controlla la connessione a internet e riprova l'installazione.\n\n" + e.Message, "MultiScreen", 0x10);
            return 1;
        }
    }

    static string Download(HttpClient http, string url, string ext = ".zip")
    {
        string path = Path.Combine(Path.GetTempPath(), "ms-" + Guid.NewGuid().ToString("N") + ext);
        using var resp = http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).Result;
        resp.EnsureSuccessStatusCode();
        using var src = resp.Content.ReadAsStream();
        using var dst = File.Create(path);
        src.CopyTo(dst);
        return path;
    }

    /// Avvio automatico all'accesso, con privilegi elevati senza chiedere conferma (serve per il driver e per
    /// usare touch/mouse anche sulle finestre amministratore).
    static void RegisterAutostart()
    {
        string user = $"{Environment.UserDomainName}\\{Environment.UserName}";
        string exe = Path.Combine(AppContext.BaseDirectory, "MultiScreen.exe");
        string xml = $@"<?xml version=""1.0"" encoding=""UTF-16""?>
<Task version=""1.2"" xmlns=""http://schemas.microsoft.com/windows/2004/02/mit/task"">
  <RegistrationInfo><Description>MultiScreen: tablet come secondo schermo via USB</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>{Esc(user)}</UserId></LogonTrigger></Triggers>
  <Principals><Principal id=""Author""><UserId>{Esc(user)}</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>4</Priority>
  </Settings>
  <Actions Context=""Author""><Exec><Command>""{Esc(exe)}""</Command></Exec></Actions>
</Task>";
        string f = Path.Combine(Path.GetTempPath(), "multiscreen-task.xml");
        File.WriteAllText(f, xml, System.Text.Encoding.Unicode);
        var psi = new ProcessStartInfo("schtasks") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        foreach (var a in new[] { "/Create", "/F", "/TN", "MultiScreen", "/XML", f }) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi);
        Log.I("avvio automatico: " + p.StandardOutput.ReadToEnd().Trim());
        p.WaitForExit();
        File.Delete(f);
    }

    static string Esc(string s) => System.Security.SecurityElement.Escape(s);
}
