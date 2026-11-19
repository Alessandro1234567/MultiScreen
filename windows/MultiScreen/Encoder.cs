using System.Diagnostics;

namespace MultiScreen;

/// Cattura uno schermo e lo codifica in H.264 con ffmpeg (ddagrab + encoder hardware della scheda video).
/// ffmpeg scrive FLV: ogni pacchetto e' un frame intero, cosi' non aspettiamo il frame dopo per spedirlo.
sealed class Encoder : IDisposable
{
    public delegate void FrameHandler(byte[] annexB, bool key);

    readonly int _adapter, _output, _fps, _kbps, _w, _h;
    readonly FrameHandler _onFrame;
    Process _p;
    volatile bool _stop;
    public string Name { get; private set; } = "";
    public static string FfmpegPath => Path.Combine(AppContext.BaseDirectory, "tools", "ffmpeg.exe");
    static string _lastGood; // encoder che ha funzionato (per partire subito da quello)

    public Encoder(int adapter, int output, int w, int h, int fps, int kbps, FrameHandler onFrame)
    {
        _adapter = adapter; _output = output; _w = w; _h = h; _fps = fps; _kbps = kbps; _onFrame = onFrame;
    }

    List<(string id, string args)> Candidates(uint vendor)
    {
        int g = Math.Max(_fps, 15) * 2, buf = Math.Max(_kbps / Math.Max(_fps, 1) * 2, 500);
        string rate = $"-b:v {_kbps}k -maxrate {_kbps}k -bufsize {buf}k -g {g} -bf 0";
        var all = new List<(string, string)>
        {
            ("nvenc", $"-c:v h264_nvenc -preset p1 -tune ull -zerolatency 1 -delay 0 -rc cbr {rate} -forced-idr 1"),
            ("qsv", $"-vf hwmap=derive_device=qsv,format=qsv,vpp_qsv=format=nv12 -c:v h264_qsv -preset veryfast -low_power 1 -async_depth 1 -look_ahead 0 {rate}"),
            ("qsv2", $"-vf hwmap=derive_device=qsv,format=qsv -c:v h264_qsv -preset veryfast -async_depth 1 {rate}"),
            ("amf", $"-c:v h264_amf -usage ultralowlatency -quality speed -rc cbr {rate}"),
            ("mf", $"-vf hwdownload,format=bgra,format=nv12 -c:v h264_mf -hw_encoding 1 -scenario display_remoting {rate}"),
            ("x264", $"-vf hwdownload,format=bgra,format=yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -threads 4 {rate}"),
        };
        string first = vendor switch { 0x10DE => "nvenc", 0x8086 => "qsv", 0x1002 => "amf", _ => "mf" };
        return all.OrderBy(c => c.Item1 == _lastGood ? 0 : c.Item1 == first ? 1 : c.Item1.StartsWith(first) ? 2 : 3).ToList();
    }

    /// Prova gli encoder finche' uno produce video. Ritorna false se nessuno funziona.
    public bool Start(uint vendor)
    {
        foreach (var (id, args) in Candidates(vendor))
        {
            if (_stop) return false;
            var started = new ManualResetEventSlim();
            if (!Launch(id, args, started)) continue;
            if (started.Wait(6000) && !_p.HasExited)
            {
                Name = id;
                _lastGood = id;
                Log.I($"video: encoder {id} ok ({_w}x{_h}, {_fps} fps, {_kbps} kbit/s)");
                return true;
            }
            Log.W($"video: encoder {id} non va");
            Kill();
        }
        return false;
    }

    bool Launch(string id, string encArgs, ManualResetEventSlim started)
    {
        string a = $"-hide_banner -loglevel error -nostdin -fflags nobuffer -init_hw_device d3d11va=d3d:{_adapter} -filter_hw_device d3d " +
                   $"-f lavfi -i ddagrab=output_idx={_output}:framerate={_fps}:dup_frames=0:draw_mouse=1 " +
                   $"{encArgs} -flush_packets 1 -flvflags no_duration_filesize -f flv pipe:1";
        try
        {
            var psi = new ProcessStartInfo(FfmpegPath, a)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            };
            _p = Process.Start(psi);
            try { _p.PriorityClass = ProcessPriorityClass.AboveNormal; } catch { }
        }
        catch (Exception e) { Log.E($"ffmpeg non avviabile: {e.Message}"); return false; }
        var p = _p;
        new Thread(() => { string l; while ((l = p.StandardError.ReadLine()) != null) Log.W($"ffmpeg[{id}]: {l}"); })
            { IsBackground = true }.Start();
        new Thread(() => ReadFlv(p, started)) { IsBackground = true, Name = "flv" }.Start();
        return true;
    }

    // ------------------------------------------------------------ FLV -> H.264 Annex-B
    void ReadFlv(Process p, ManualResetEventSlim started)
    {
        var s = new BufferedStream(p.StandardOutput.BaseStream, 1 << 20);
        byte[] hdr = new byte[11];
        byte[] sps = null, pps = null;
        int lenSize = 4;
        try
        {
            if (!ReadFull(s, new byte[13], 13)) return; // intestazione FLV + PreviousTagSize0
            while (!_stop)
            {
                if (!ReadFull(s, hdr, 11)) return;
                int size = (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
                byte[] d = new byte[size + 4];
                if (!ReadFull(s, d, size + 4)) return;
                if (hdr[0] != 9 || size < 5) continue; // solo video
                bool key = (d[0] >> 4) == 1;
                int type = d[1];
                if (type == 0) // AVCDecoderConfigurationRecord
                {
                    lenSize = (d[5 + 4] & 3) + 1;
                    int o = 5 + 5;
                    int nsps = d[o++] & 0x1f;
                    for (int i = 0; i < nsps; i++) { int l = (d[o] << 8) | d[o + 1]; sps = d[(o + 2)..(o + 2 + l)]; o += 2 + l; }
                    int npps = d[o++];
                    for (int i = 0; i < npps; i++) { int l = (d[o] << 8) | d[o + 1]; pps = d[(o + 2)..(o + 2 + l)]; o += 2 + l; }
                    continue;
                }
                if (type != 1) continue;
                using var ms = new MemoryStream(size + 64);
                if (key && sps != null && pps != null) { StartCode(ms); ms.Write(sps); StartCode(ms); ms.Write(pps); }
                int pos = 5;
                while (pos + lenSize <= size)
                {
                    int l = 0;
                    for (int i = 0; i < lenSize; i++) l = (l << 8) | d[pos + i];
                    pos += lenSize;
                    if (l <= 0 || pos + l > size) break;
                    StartCode(ms);
                    ms.Write(d, pos, l);
                    pos += l;
                }
                started.Set();
                _onFrame(ms.ToArray(), key);
            }
        }
        catch (Exception e) { if (!_stop) Log.W($"video: lettura interrotta: {e.Message}"); }
    }

    static void StartCode(Stream s) { s.WriteByte(0); s.WriteByte(0); s.WriteByte(0); s.WriteByte(1); }

    static bool ReadFull(Stream s, byte[] b, int n)
    {
        int o = 0;
        while (o < n) { int r = s.Read(b, o, n - o); if (r <= 0) return false; o += r; }
        return true;
    }

    public bool Running => _p != null && !_p.HasExited;

    void Kill()
    {
        try { if (_p != null && !_p.HasExited) _p.Kill(true); } catch { }
        try { _p?.WaitForExit(2000); } catch { }
        _p?.Dispose();
        _p = null;
    }

    public void Dispose()
    {
        _stop = true;
        Kill();
    }
}
