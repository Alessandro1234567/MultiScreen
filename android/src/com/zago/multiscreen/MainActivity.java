package com.zago.multiscreen;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcelable;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.lang.reflect.Method;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity implements Connection.Listener, VideoDecoder.Listener, InputHandler.Sink,
        FileTransfer.Ui {
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final int REQ_PICK = 42;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private FrameLayout root;
    private SurfaceView surfaceView;
    private ScrollView setupView, guideView;
    private FrameLayout panelView;
    private TextView statusText, statsText, panelStats, panelFiles;
    private Connection conn;
    private VideoDecoder decoder;
    private InputHandler input;
    private FileTransfer files;
    private ClipboardManager clipboard;

    private volatile boolean surfaceValid, streaming;
    private boolean pendingHelloAfterRotate;
    private volatile int cfgW, cfgH, cfgFps;
    private int decoderFailures;
    private String lastStatus = "";
    private final List<Uri> pendingUris = new ArrayList<Uri>();
    private String clipSent, clipReceived;
    private long clipStamp = -2;
    private long lastToast;

    private volatile int framesRendered;
    private volatile long bytesReceived;
    private volatile long lastRttNs;
    private String statsLine = "";

    // ================================================================ ciclo di vita
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("multiscreen", Context.MODE_PRIVATE);
        clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_FULLSCREEN
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS);
        setCutoutMode(w);
        pickHighestRefresh(w);
        applyOrientation();

        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new SurfaceView(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        statsText = Ui.text(this, "", 13, Color.WHITE, false);
        statsText.setTypeface(Typeface.MONOSPACE);
        statsText.setBackgroundColor(0xAA000000);
        statsText.setPadding(dp(10), dp(6), dp(10), dp(6));
        statsText.setVisibility(View.GONE);
        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START);
        slp.setMargins(dp(12), dp(12), 0, 0);
        root.addView(statsText, slp);

        setupView = new ScrollView(this);
        root.addView(setupView, full());
        panelView = new FrameLayout(this);
        panelView.setVisibility(View.GONE);
        root.addView(panelView, full());
        guideView = new ScrollView(this);
        guideView.setVisibility(View.GONE);
        root.addView(guideView, full());
        setContentView(root);

        decoder = new VideoDecoder(this);
        conn = new Connection(this);
        files = new FileTransfer(this, conn, this);
        input = new InputHandler(this, getResources().getDisplayMetrics().density);
        applyInputMode();
        buildSetup();

        surfaceView.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent e) {
                return streaming && input.onTouch(v, e);
            }
        });
        View.OnGenericMotionListener gm = new View.OnGenericMotionListener() {
            @Override
            public boolean onGenericMotion(View v, MotionEvent e) {
                return streaming && input.onGeneric(v, e);
            }
        };
        surfaceView.setOnGenericMotionListener(gm);
        surfaceView.setOnHoverListener(new View.OnHoverListener() {
            @Override
            public boolean onHover(View v, MotionEvent e) {
                return streaming && input.onGeneric(v, e);
            }
        });
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder h) {
                decoder.setSurface(h.getSurface());
                setFrameRate(h.getSurface(), 60f);
                surfaceValid = true;
                if (conn.isUp()) sendHello();
            }

            @Override
            public void surfaceChanged(SurfaceHolder h, int format, int width, int height) {
                if (pendingHelloAfterRotate && isOrientationApplied()) {
                    pendingHelloAfterRotate = false;
                    sendHello();
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder h) {
                surfaceValid = false;
                streaming = false;
                decoder.release();
                decoder.setSurface(null);
                conn.send(Proto.packet(Proto.PAUSE, null)); // il PC smette di mandare video ma resta collegato
            }
        });
        clipboard.addPrimaryClipChangedListener(new ClipboardManager.OnPrimaryClipChangedListener() {
            @Override
            public void onPrimaryClipChanged() {
                checkClipboard();
            }
        });
        readToken(getIntent());
        requestLegacyStorage();
        handleShareIntent(getIntent());
        conn.start();
        ui.postDelayed(statsTick, 1000);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (readToken(intent) && conn.isUp() && surfaceValid) sendHello();
        handleShareIntent(intent);
    }

    /** Il PC avvia l'app passando un codice segreto: solo lui puo' usare il collegamento. */
    private boolean readToken(Intent it) {
        String t = it != null ? it.getStringExtra("token") : null;
        if (t == null || t.isEmpty() || t.equals(prefs.getString("token", ""))) return false;
        prefs.edit().putString("token", t).apply();
        return true;
    }

    /** Android 6-9: serve il permesso per salvare i file ricevuti in Download. */
    private void requestLegacyStorage() {
        if (Build.VERSION.SDK_INT < 29
                && checkSelfPermission("android.permission.WRITE_EXTERNAL_STORAGE") != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{"android.permission.WRITE_EXTERNAL_STORAGE"}, 7);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUi();
            checkClipboard();
        }
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacksAndMessages(null);
        conn.stop();
        decoder.release();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (guideView.getVisibility() == View.VISIBLE) showGuide(false);
        else if (panelView.getVisibility() == View.VISIBLE) showPanel(false);
        else if (streaming) showPanel(true); // durante l'uso "indietro" apre il pannello invece di chiudere
        else super.onBackPressed();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK || res != RESULT_OK || data == null) return;
        List<Uri> l = new ArrayList<Uri>();
        ClipData cd = data.getClipData();
        if (cd != null) for (int i = 0; i < cd.getItemCount(); i++) l.add(cd.getItemAt(i).getUri());
        else if (data.getData() != null) l.add(data.getData());
        queueFiles(l);
    }

    // ================================================================ collegamento
    private void sendHello() {
        conn.send(Proto.packet(Proto.HELLO, buildHello().toString().getBytes(UTF8)));
    }

    private JSONObject buildHello() {
        JSONObject o = new JSONObject();
        try {
            DisplayMetrics m = new DisplayMetrics();
            Display d = getWindowManager().getDefaultDisplay();
            d.getRealMetrics(m);
            int big = Math.max(m.widthPixels, m.heightPixels), small = Math.min(m.widthPixels, m.heightPixels);
            boolean portrait = "portrait".equals(prefs.getString("orient", "landscape"));
            float maxRefresh = d.getRefreshRate();
            for (Display.Mode mode : d.getSupportedModes()) maxRefresh = Math.max(maxRefresh, mode.getRefreshRate());
            o.put("proto", 1);
            o.put("token", prefs.getString("token", ""));
            o.put("model", Build.MANUFACTURER + " " + Build.MODEL);
            o.put("android", Build.VERSION.SDK_INT);
            o.put("width", portrait ? small : big);
            o.put("height", portrait ? big : small);
            o.put("density", m.density);
            o.put("dpi", m.densityDpi);
            o.put("refresh", Math.round(maxRefresh));
            int[] max = VideoDecoder.maxSize(portrait ? small : big, portrait ? big : small);
            o.put("maxw", max[0]);
            o.put("maxh", max[1]);
            JSONArray codecs = new JSONArray();
            codecs.put("h264");
            if (hasDecoder("video/hevc")) codecs.put("h265");
            o.put("codecs", codecs);
            JSONObject p = new JSONObject();
            p.put("res", Integer.parseInt(prefs.getString("res", "75")));
            p.put("fps", Integer.parseInt(prefs.getString("fps", "60")));
            p.put("bitrate", Integer.parseInt(prefs.getString("bitrate", "20")));
            p.put("ui", prefs.getString("ui", "normal"));
            p.put("input", prefs.getString("input", "touch"));
            p.put("mode", prefs.getString("mode", "extend"));
            p.put("pos", prefs.getString("pos", "right"));
            o.put("prefs", p);
        } catch (Exception ignored) {
        }
        return o;
    }

    private static boolean hasDecoder(String mime) {
        try {
            for (MediaCodecInfo i : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                if (i.isEncoder()) continue;
                for (String t : i.getSupportedTypes()) if (t.equalsIgnoreCase(mime)) return true;
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    @Override
    public void onLinkUp() {
        if (surfaceValid) sendHello();
        ui.post(new Runnable() {
            @Override
            public void run() {
                if (!pendingUris.isEmpty()) {
                    files.send(new ArrayList<Uri>(pendingUris));
                    pendingUris.clear();
                }
                checkClipboard();
                buildSetup();
            }
        });
    }

    @Override
    public void onConfig(JSONObject cfg) {
        final int w = cfg.optInt("width"), h = cfg.optInt("height"), fps = cfg.optInt("fps", 60);
        cfgW = w;
        cfgH = h;
        cfgFps = fps;
        decoderFailures = 0;
        if (!surfaceValid) return;
        ui.post(new Runnable() {
            @Override
            public void run() {
                layoutVideo(w, h);
            }
        });
        if (!decoder.configure(w, h, fps)) setStatus("Errore: decoder video non disponibile");
        else setStatus("Ricezione video " + w + "×" + h + "…");
    }

    /** Adatta la superficie al formato del video (bande nere in modalita' duplica). */
    private void layoutVideo(int w, int h) {
        surfaceView.getHolder().setFixedSize(w, h);
        int rw = root.getWidth(), rh = root.getHeight();
        if (rw <= 0 || rh <= 0 || w <= 0 || h <= 0) return;
        float s = Math.min(rw / (float) w, rh / (float) h);
        int vw = Math.round(w * s), vh = Math.round(h * s);
        if (Math.abs(vw - rw) < 8) vw = rw;
        if (Math.abs(vh - rh) < 8) vh = rh;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        if (lp.width != vw || lp.height != vh) {
            lp.width = vw;
            lp.height = vh;
            lp.gravity = Gravity.CENTER;
            surfaceView.setLayoutParams(lp);
        }
    }

    @Override
    public void onVideo(byte[] data, int off, int len, long ptsUs, boolean key) {
        bytesReceived += len;
        if (surfaceValid) decoder.queue(data, off, len, ptsUs, key);
    }

    @Override
    public void onPong(long rttNs) {
        lastRttNs = rttNs;
    }

    @Override
    public void onStatus(String msg) {
        setStatus(msg);
    }

    @Override
    public void onLinkDown(boolean hadSession) {
        decoder.release();
        files.reset();
        if (streaming || hadSession) {
            streaming = false;
            ui.post(new Runnable() {
                @Override
                public void run() {
                    showPanel(false);
                    showSetup(true);
                    buildSetup();
                }
            });
        }
        if (hadSession) setStatus("In attesa del PC…\nCollega il cavo USB: lo schermo parte da solo.");
    }

    @Override
    public void onOther(int type, byte[] data, int len) {
        if (type == Proto.CLIPBOARD_IN) {
            final String text = new String(data, 0, len, UTF8);
            ui.post(new Runnable() {
                @Override
                public void run() {
                    onPcClipboard(text);
                }
            });
        } else if (type >= Proto.FILE_BEGIN_IN && type <= Proto.FILE_RESULT_IN) {
            files.onPacket(type, data, len);
        }
    }

    // ================================================================ appunti
    private boolean clipEnabled() {
        return "on".equals(prefs.getString("clip", "on"));
    }

    private void onPcClipboard(String text) {
        if (!clipEnabled()) return;
        clipReceived = text;
        try {
            clipboard.setPrimaryClip(ClipData.newPlainText("PC", text));
        } catch (Exception ignored) {
        }
        clipStamp = clipTimestamp();
    }

    private long clipTimestamp() {
        try {
            ClipDescription d = clipboard.getPrimaryClipDescription();
            if (d == null) return -1;
            Method m = ClipDescription.class.getMethod("getTimestamp");
            return (Long) m.invoke(d);
        } catch (Exception e) {
            return -1;
        }
    }

    /** Se gli appunti del tablet sono cambiati, li manda al PC (Android li fa leggere solo all'app in primo piano). */
    private void checkClipboard() {
        if (!clipEnabled() || !conn.isUp() || !hasWindowFocus()) return;
        long ts = clipTimestamp();
        if (ts != -1 && ts == clipStamp) return;
        clipStamp = ts;
        try {
            ClipData c = clipboard.getPrimaryClip();
            if (c == null || c.getItemCount() == 0) return;
            CharSequence t = c.getItemAt(0).coerceToText(this);
            if (t == null || t.length() == 0) return;
            String s = t.toString();
            if (s.length() > 500000) s = s.substring(0, 500000);
            if (s.equals(clipReceived) || s.equals(clipSent)) return;
            clipSent = s;
            conn.send(Proto.packet(Proto.CLIPBOARD, s.getBytes(UTF8)));
        } catch (Exception ignored) {
        }
    }

    // ================================================================ file
    private void handleShareIntent(Intent it) {
        if (it == null) return;
        String a = it.getAction();
        List<Uri> l = new ArrayList<Uri>();
        if (Intent.ACTION_SEND.equals(a)) {
            Parcelable p = it.getParcelableExtra(Intent.EXTRA_STREAM);
            if (p instanceof Uri) l.add((Uri) p);
            else {
                CharSequence text = it.getCharSequenceExtra(Intent.EXTRA_TEXT);
                if (text != null) {
                    // testo condiviso: va negli appunti del PC
                    clipSent = text.toString();
                    conn.send(Proto.packet(Proto.CLIPBOARD, clipSent.getBytes(UTF8)));
                    toast(conn.isUp() ? "Testo copiato negli appunti del PC" : "Collega il tablet al PC e riprova");
                }
            }
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(a)) {
            ArrayList<Parcelable> ps = it.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (ps != null) for (Parcelable p : ps) if (p instanceof Uri) l.add((Uri) p);
        }
        setIntent(new Intent(Intent.ACTION_MAIN));
        if (!l.isEmpty()) queueFiles(l);
    }

    private void queueFiles(List<Uri> l) {
        if (l.isEmpty()) return;
        if (conn.isUp()) {
            files.send(l);
            toast(l.size() == 1 ? "Invio al PC…" : "Invio di " + l.size() + " file al PC…");
        } else {
            pendingUris.addAll(l);
            toast("Collega il cavo: i file partono appena il PC è collegato");
        }
    }

    private void pickFiles() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            startActivityForResult(i, REQ_PICK);
        } catch (ActivityNotFoundException e) {
            toast("Nessun selettore di file disponibile");
        }
    }

    private void openReceived() {
        try {
            startActivity(new Intent("android.intent.action.VIEW_DOWNLOADS").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (Exception e) {
            toast("Apri l'app File → Download → MultiScreen");
        }
    }

    @Override
    public void fileMessage(final String msg, final boolean important) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                if (panelFiles != null) panelFiles.setText(msg);
                long now = System.currentTimeMillis();
                if (important && now - lastToast > 1500) {
                    lastToast = now;
                    Toast.makeText(MainActivity.this, msg, Toast.LENGTH_SHORT).show();
                }
            }
        });
    }

    // ================================================================ decoder / input
    @Override
    public void onFrameRendered(boolean first) {
        framesRendered++;
        if (first) {
            streaming = true;
            ui.post(new Runnable() {
                @Override
                public void run() {
                    showSetup(false);
                    statsText.setVisibility("on".equals(prefs.getString("stats", "off")) ? View.VISIBLE : View.GONE);
                }
            });
        }
    }

    @Override
    public void onNeedKeyframe() {
        conn.send(Proto.packet(Proto.KEYFRAME_REQ, null));
    }

    @Override
    public void onDecoderFailed(String why) {
        if (++decoderFailures <= 5 && cfgW > 0) {
            ui.postDelayed(new Runnable() {
                @Override
                public void run() {
                    new Thread(new Runnable() {
                        @Override
                        public void run() {
                            if (surfaceValid && decoder.configure(cfgW, cfgH, cfgFps)) onNeedKeyframe();
                        }
                    }).start();
                }
            }, 200);
        } else {
            setStatus("Errore video: " + why);
        }
    }

    @Override
    public void send(byte[] pkt) {
        conn.send(pkt);
    }

    @Override
    public void togglePanel() {
        showPanel(panelView.getVisibility() != View.VISIBLE);
    }

    private final Runnable statsTick = new Runnable() {
        long lastBytes;
        int lastFrames;

        @Override
        public void run() {
            if (conn.isUp()) conn.ping();
            int f = framesRendered;
            long by = bytesReceived;
            statsLine = String.format(Locale.ROOT, "%d×%d  %d fps  %.1f Mbit/s  ping %.1f ms", cfgW, cfgH, f - lastFrames,
                    (by - lastBytes) * 8 / 1e6, lastRttNs / 1e6);
            if (statsText.getVisibility() == View.VISIBLE) statsText.setText(statsLine);
            if (panelStats != null && panelView.getVisibility() == View.VISIBLE)
                panelStats.setText(statsLine + "\n" + decoder.name());
            lastFrames = f;
            lastBytes = by;
            ui.postDelayed(this, 1000);
        }
    };

    // ================================================================ schermate
    private void showSetup(boolean show) {
        setupView.setVisibility(show ? View.VISIBLE : View.GONE);
        if (!show) hideSystemUi();
    }

    private void setStatus(final String s) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                lastStatus = s;
                if (statusText != null) statusText.setText(s);
            }
        });
    }

    private void buildSetup() {
        setupView.removeAllViews();
        setupView.setBackgroundColor(Ui.BG);
        setupView.setFillViewport(true);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(32), dp(36), dp(32), dp(36));
        setupView.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        col.addView(Ui.text(this, "MultiScreen", 34, Color.WHITE, true));
        statusText = Ui.text(this, lastStatus.isEmpty() ? "In attesa del PC…\nCollega il cavo USB: lo schermo parte da solo." : lastStatus,
                17, 0xFFB8C4D9, false);
        statusText.setGravity(Gravity.CENTER);
        statusText.setPadding(0, dp(10), 0, dp(24));
        col.addView(statusText);

        LinearLayout card = settingsCard(false);
        col.addView(card, new LinearLayout.LayoutParams(dp(660), ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout row = Ui.row(this);
        row.setPadding(0, dp(18), 0, 0);
        Ui.addWeighted(row, Ui.button(this, "Guida e impostazioni tablet", true, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showGuide(true);
            }
        }), 0);
        Ui.addWeighted(row, Ui.button(this, "File ricevuti", false, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openReceived();
            }
        }), 10);
        col.addView(row, new LinearLayout.LayoutParams(dp(660), ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView hint = Ui.text(this, "Durante l'uso: tocca con 4 dita (o il gesto Indietro) per il pannello.\n"
                + "Per mandare foto al PC: Condividi → MultiScreen da qualsiasi app.", 13, 0xFF7F8BA3, false);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, dp(18), 0, 0);
        col.addView(hint);
    }

    /** Scheda impostazioni; live=true quando il tablet e' gia' in uso (pannello). */
    private LinearLayout settingsCard(final boolean live) {
        LinearLayout card = Ui.card(this);
        Ui.OnChoice save = new Ui.OnChoice() {
            @Override
            public void chosen(String key, String value) {
                prefs.edit().putString(key, value).apply();
                if ("input".equals(key)) applyInputMode();
                else if ("stats".equals(key)) {
                    if (streaming) statsText.setVisibility("on".equals(value) ? View.VISIBLE : View.GONE);
                } else if ("clip".equals(key)) {
                    checkClipboard();
                } else {
                    // il resto va al PC: applica da solo poco dopo l'ultimo tocco
                    ui.removeCallbacks(applySettings);
                    ui.postDelayed(applySettings, 600);
                }
            }
        };
        LinearLayout two = Ui.row(this);
        LinearLayout c1 = new LinearLayout(this), c2 = new LinearLayout(this);
        c1.setOrientation(LinearLayout.VERTICAL);
        c2.setOrientation(LinearLayout.VERTICAL);
        Ui.addWeighted(two, c1, 0);
        Ui.addWeighted(two, c2, 18);
        card.addView(two);

        opt(c1, "Modalità", "mode", "extend", new String[]{"Estendi", "Duplica"}, new String[]{"extend", "mirror"}, save);
        opt(c1, "Posizione rispetto al PC", "pos", "right", new String[]{"Sinistra", "Destra", "Sopra", "Sotto", "Auto"},
                new String[]{"left", "right", "top", "bottom", "auto"}, save);
        opt(c1, "Orientamento", "orient", "landscape", new String[]{"Orizzontale", "Verticale"}, new String[]{"landscape", "portrait"}, save);
        opt(c1, "Risoluzione", "res", "75", new String[]{"50%", "75%", "100%"}, new String[]{"50", "75", "100"}, save);
        opt(c1, "Fluidità", "fps", "60", new String[]{"30 fps", "60 fps"}, new String[]{"30", "60"}, save);
        opt(c1, "Qualità immagine", "bitrate", "20", new String[]{"Bassa", "Media", "Alta", "Max"}, new String[]{"8", "14", "20", "35"}, save);
        opt(c2, "Dimensione interfaccia", "ui", "normal", new String[]{"Piccola", "Normale", "Grande"}, new String[]{"small", "normal", "large"}, save);
        opt(c2, "Dito come", "input", "touch", new String[]{"Touch", "Mouse"}, new String[]{"touch", "mouse"}, save);
        opt(c2, "Appunti condivisi", "clip", "on", new String[]{"Sì", "No"}, new String[]{"on", "off"}, save);
        opt(c2, "Statistiche a schermo", "stats", "off", new String[]{"No", "Sì"}, new String[]{"off", "on"}, save);
        TextView note = Ui.text(this, "Le modifiche valgono subito. Risoluzione, modalità, orientamento e dimensione "
                + "ricreano lo schermo (le finestre tornano un attimo sul PC).\n"
                + "Posizione «Auto» = quella scelta sul PC in Impostazioni di sistema → Schermi.", 12, Ui.MUTED, false);
        note.setPadding(0, dp(14), 0, 0);
        c2.addView(note);
        return card;
    }

    private void opt(LinearLayout parent, String label, String key, String def, String[] labels, String[] values, Ui.OnChoice cb) {
        Ui.option(this, parent, label, key, prefs.getString(key, def), labels, values, cb);
    }

    /** Manda al PC le impostazioni nuove (se c'e' un'orientazione da cambiare, dopo la rotazione). */
    private final Runnable applySettings = new Runnable() {
        @Override
        public void run() {
            if (!conn.isUp()) return;
            if (applyOrientation()) {
                pendingHelloAfterRotate = true;
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
                lp.width = lp.height = ViewGroup.LayoutParams.MATCH_PARENT;
                surfaceView.setLayoutParams(lp);
                ui.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (pendingHelloAfterRotate) {
                            pendingHelloAfterRotate = false;
                            sendHello();
                        }
                    }
                }, 1500);
            } else if (surfaceValid) {
                sendHello();
            }
        }
    };

    private void showPanel(boolean show) {
        if (!show) {
            panelView.setVisibility(View.GONE);
            panelView.removeAllViews();
            panelStats = panelFiles = null;
            hideSystemUi();
            return;
        }
        panelView.removeAllViews();
        panelView.setBackgroundColor(0xCC000000);
        panelView.setClickable(true); // non far passare i tocchi al PC
        ScrollView sc = new ScrollView(this);
        panelView.addView(sc, full());
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(24), dp(24), dp(24), dp(24));
        sc.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout head = Ui.row(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = Ui.text(this, "MultiScreen", 24, Color.WHITE, true);
        head.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(Ui.button(this, "Chiudi ✕", false, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showPanel(false);
            }
        }));
        col.addView(head, new LinearLayout.LayoutParams(dp(760), ViewGroup.LayoutParams.WRAP_CONTENT));

        panelStats = Ui.text(this, statsLine + "\n" + decoder.name(), 13, Ui.MUTED, false);
        panelStats.setTypeface(Typeface.MONOSPACE);
        panelStats.setPadding(0, dp(6), 0, dp(12));
        col.addView(panelStats, new LinearLayout.LayoutParams(dp(760), ViewGroup.LayoutParams.WRAP_CONTENT));

        col.addView(settingsCard(true), new LinearLayout.LayoutParams(dp(760), ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout actions = Ui.row(this);
        actions.setPadding(0, dp(16), 0, 0);
        Ui.addWeighted(actions, Ui.button(this, "Invia file al PC", true, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pickFiles();
            }
        }), 0);
        Ui.addWeighted(actions, Ui.button(this, "File ricevuti", false, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openReceived();
            }
        }), 10);
        col.addView(actions, new LinearLayout.LayoutParams(dp(760), ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout actions2 = Ui.row(this);
        actions2.setPadding(0, dp(10), 0, 0);
        Ui.addWeighted(actions2, Ui.button(this, "Guida", false, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showGuide(true);
            }
        }), 0);
        Ui.addWeighted(actions2, Ui.button(this, "Disconnetti", false, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                conn.send(Proto.packet(Proto.BYE, null));
                ui.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        finish();
                    }
                }, 300);
            }
        }), 10);
        col.addView(actions2, new LinearLayout.LayoutParams(dp(760), ViewGroup.LayoutParams.WRAP_CONTENT));

        String cur = files.current();
        panelFiles = Ui.text(this, cur != null ? "Invio al PC: " + cur : "", 13, Ui.MUTED, false);
        panelFiles.setPadding(0, dp(10), 0, 0);
        col.addView(panelFiles, new LinearLayout.LayoutParams(dp(760), ViewGroup.LayoutParams.WRAP_CONTENT));
        panelView.setVisibility(View.VISIBLE);
    }

    private void showGuide(boolean show) {
        if (!show) {
            guideView.setVisibility(View.GONE);
            guideView.removeAllViews();
            hideSystemUi();
            return;
        }
        guideView.removeAllViews();
        guideView.setBackgroundColor(Ui.BG);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(40), dp(32), dp(40), dp(40));
        guideView.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout head = Ui.row(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.addView(Ui.text(this, "Guida", 28, Color.WHITE, true), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(Ui.button(this, "Chiudi ✕", false, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showGuide(false);
            }
        }));
        col.addView(head);

        step(col, "1. Attiva le Opzioni sviluppatore (una volta)",
                "Impostazioni → Info sul tablet → tocca 7 volte «Versione OS» finché compare «Ora sei uno sviluppatore».",
                "Apri Info tablet", new Intent(Settings.ACTION_DEVICE_INFO_SETTINGS));
        step(col, "2. Attiva Debug USB",
                "Impostazioni → Impostazioni aggiuntive → Opzioni sviluppatore → attiva «Debug USB».\n"
                        + "Attiva anche «Installa tramite USB»: serve per gli aggiornamenti automatici dell'app.",
                "Apri Opzioni sviluppatore", new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS));
        step(col, "3. Autorizza il PC",
                "Collega il cavo (va bene anche un cavo solo dati). Sul tablet compare «Consentire il debug USB?»: "
                        + "spunta «Consenti sempre da questo computer» e tocca Consenti.", null, null);
        step(col, "4. Sul PC (una volta)",
                "Nella cartella MultiScreen esegui  ./install.sh\nDa quel momento il programma parte da solo a ogni accesso: "
                        + "colleghi il cavo e lo schermo compare.", null, null);
        step(col, "5. Batteria (consigliato)",
                "Impostazioni app → MultiScreen → Risparmio energetico → «Nessuna restrizione». Così file e appunti "
                        + "continuano a funzionare anche quando usi altre app.",
                "Apri impostazioni app", new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
        step(col, "Come si usa",
                "• Stacca il cavo: lo schermo sparisce e le finestre tornano sul PC. Riattacca: riparte da solo.\n"
                        + "• 4 dita o gesto Indietro: pannello con impostazioni, invio file, disconnessione.\n"
                        + "• Foto e file al PC: Condividi → MultiScreen (vanno in Scaricati/MultiScreen sul PC).\n"
                        + "• File dal PC: tasto destro in Dolphin → «Invia al tablet» (arrivano in Download/MultiScreen).\n"
                        + "• Appunti: copi su un dispositivo e incolli sull'altro. Il testo copiato sul tablet passa al PC quando "
                        + "torni su MultiScreen.\n"
                        + "• Posizione dello schermo: sul PC, Impostazioni di sistema → Schermi.\n"
                        + "• Penna: avvicinala per muovere il cursore, tocca per cliccare, tasto laterale = click destro.\n"
                        + "• Modalità mouse: tocca = click, trascina = trascina, tieni premuto o tocco con 2 dita = click destro, "
                        + "2 dita = scorrimento.", null, null);
        step(col, "Problemi",
                "• Resta «In attesa del PC»: controlla Debug USB e l'autorizzazione; sul PC prova  multiscreen status\n"
                        + "• L'app non si aggiorna da sola: attiva «Installa tramite USB» (punto 2).\n"
                        + "• Immagine a scatti: abbassa Risoluzione o Qualità.\n"
                        + "• Testo troppo piccolo o grande: cambia Dimensione interfaccia.", null, null);
        guideView.setVisibility(View.VISIBLE);
    }

    private void step(LinearLayout col, String title, String body, String button, final Intent intent) {
        LinearLayout card = Ui.card(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(14), 0, 0);
        col.addView(card, lp);
        card.addView(Ui.text(this, title, 17, Color.WHITE, true));
        TextView b = Ui.text(this, body, 15, Ui.TEXT, false);
        b.setPadding(0, dp(6), 0, 0);
        b.setLineSpacing(0, 1.2f);
        card.addView(b);
        if (button != null) {
            TextView btn = Ui.button(this, button, true, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    try {
                        startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                    } catch (Exception e) {
                        try {
                            startActivity(new Intent(Settings.ACTION_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                        } catch (Exception ignored) {
                        }
                    }
                }
            });
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            blp.setMargins(0, dp(12), 0, 0);
            card.addView(btn, blp);
        }
    }

    // ================================================================ utilita'
    private void applyInputMode() {
        input.setMode("mouse".equals(prefs.getString("input", "touch")) ? InputHandler.MODE_MOUSE : InputHandler.MODE_TOUCH);
    }

    private int wantedOrientation() {
        return "portrait".equals(prefs.getString("orient", "landscape")) ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
    }

    /** @return true se l'orientamento deve cambiare (arrivera' surfaceChanged) */
    private boolean applyOrientation() {
        int want = wantedOrientation();
        boolean change = !isOrientationApplied();
        setRequestedOrientation(want);
        return change;
    }

    private boolean isOrientationApplied() {
        DisplayMetrics m = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        boolean portraitNow = m.heightPixels > m.widthPixels;
        return portraitNow == (wantedOrientation() == ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT);
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    private FrameLayout.LayoutParams full() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private int dp(int v) {
        return Ui.dp(this, v);
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void pickHighestRefresh(Window w) {
        try {
            Display d = getWindowManager().getDefaultDisplay();
            Display.Mode cur = d.getMode();
            Display.Mode best = cur;
            for (Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) best = m;
            }
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            w.setAttributes(lp);
        } catch (Exception ignored) {
        }
    }

    private static void setCutoutMode(Window w) {
        try {
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.getClass().getField("layoutInDisplayCutoutMode").setInt(lp, 1);
            w.setAttributes(lp);
        } catch (Exception ignored) {
        }
    }

    private static void setFrameRate(Surface s, float fps) {
        try {
            Method m = Surface.class.getMethod("setFrameRate", float.class, int.class);
            m.invoke(s, fps, 0);
        } catch (Exception ignored) {
        }
    }
}
