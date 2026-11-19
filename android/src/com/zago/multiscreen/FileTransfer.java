package com.zago.multiscreen;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;
import android.util.SparseArray;

import org.json.JSONObject;

import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.LinkedBlockingQueue;

/** File tra tablet e PC. Ricevuti -> Download/MultiScreen (MediaStore, nessun permesso necessario). */
final class FileTransfer {
    private static final String TAG = "MultiScreen";
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final int CHUNK = 64 * 1024;
    private static final Uri DOWNLOADS = Uri.parse("content://media/external/downloads");

    interface Ui {
        void fileMessage(String msg, boolean important);
    }

    private final Context ctx;
    private final ContentResolver cr;
    private final Connection conn;
    private final Ui ui;
    private final SparseArray<In> incoming = new SparseArray<In>();
    private final LinkedBlockingQueue<Uri> outQ = new LinkedBlockingQueue<Uri>();
    private Thread sender;
    private int nextId = 1;
    private volatile String current;

    private static final class In {
        Uri uri;
        java.io.File file; // Android 6-9
        OutputStream os;
        String name;
        long size, got;
        int lastPct = -1;
    }

    FileTransfer(Context c, Connection conn, Ui ui) {
        ctx = c;
        cr = c.getContentResolver();
        this.conn = conn;
        this.ui = ui;
    }

    String current() {
        return current;
    }

    // ------------------------------------------------------------ ricezione (thread di rete)
    void onPacket(int type, byte[] d, int len) {
        try {
            if (type == Proto.FILE_BEGIN_IN) begin(new JSONObject(new String(d, 0, len, UTF8)));
            else if (type == Proto.FILE_DATA_IN && len >= 4) data(Proto.get32(d, 0), d, 4, len - 4);
            else if (type == Proto.FILE_END_IN && len >= 5) end(Proto.get32(d, 0), d[4] != 0);
            else if (type == Proto.FILE_RESULT_IN) {
                JSONObject j = new JSONObject(new String(d, 0, len, UTF8));
                boolean ok = j.optBoolean("ok");
                ui.fileMessage(ok ? "Salvato sul PC: " + j.optString("name") + "\n(" + j.optString("where") + ")"
                        : "Il PC non è riuscito a salvare " + j.optString("name"), true);
            }
        } catch (Exception e) {
            Log.w(TAG, "file: " + e);
        }
    }

    private void begin(JSONObject j) {
        In in = new In();
        in.name = j.optString("name", "file");
        in.size = j.optLong("size");
        String mime = j.optString("mime", "application/octet-stream");
        try {
            if (android.os.Build.VERSION.SDK_INT < 29) {
                java.io.File dir = new java.io.File(android.os.Environment.getExternalStoragePublicDirectory(
                        android.os.Environment.DIRECTORY_DOWNLOADS), "MultiScreen");
                dir.mkdirs();
                String safe = in.name.replace('/', '_');
                java.io.File f = new java.io.File(dir, safe);
                for (int i = 1; f.exists(); i++) {
                    int dot = safe.lastIndexOf('.');
                    f = new java.io.File(dir, dot > 0 ? safe.substring(0, dot) + " (" + i + ")" + safe.substring(dot)
                            : safe + " (" + i + ")");
                }
                in.file = f;
                in.os = new java.io.FileOutputStream(f);
            } else {
                ContentValues v = new ContentValues();
                v.put("_display_name", in.name);
                v.put("mime_type", mime);
                v.put("relative_path", "Download/MultiScreen");
                v.put("is_pending", 1);
                in.uri = cr.insert(DOWNLOADS, v);
                if (in.uri != null) in.os = cr.openOutputStream(in.uri);
            }
        } catch (Exception e) {
            Log.w(TAG, "file: impossibile creare " + in.name + ": " + e);
        }
        incoming.put(j.optInt("id"), in);
        ui.fileMessage("Ricevo dal PC: " + in.name, false);
    }

    private void data(int id, byte[] d, int off, int len) {
        In in = incoming.get(id);
        if (in == null || in.os == null) return;
        try {
            in.os.write(d, off, len);
            in.got += len;
            if (in.size > 0) {
                int pct = (int) (in.got * 100 / in.size);
                if (pct / 10 != in.lastPct / 10) {
                    in.lastPct = pct;
                    ui.fileMessage("Ricevo dal PC: " + in.name + " " + pct + "%", false);
                }
            }
        } catch (Exception e) {
            closeQuiet(in);
            in.os = null;
        }
    }

    private void end(int id, boolean ok) {
        In in = incoming.get(id);
        incoming.remove(id);
        if (in == null) return;
        boolean saved = false;
        if (in.os != null) {
            try {
                in.os.close();
                if (ok && in.file != null) {
                    android.media.MediaScannerConnection.scanFile(ctx, new String[]{in.file.getPath()}, null, null);
                    saved = true;
                } else if (ok) {
                    ContentValues v = new ContentValues();
                    v.put("is_pending", 0);
                    cr.update(in.uri, v, null, null);
                    saved = true;
                }
            } catch (Exception e) {
                Log.w(TAG, "file: " + e);
            }
        }
        if (!saved) discard(in);
        try {
            JSONObject r = new JSONObject();
            r.put("id", id);
            r.put("name", in.name);
            r.put("ok", saved);
            conn.send(Proto.packet(Proto.FILE_RESULT, r.toString().getBytes(UTF8)));
        } catch (Exception ignored) {
        }
        ui.fileMessage(saved ? "Ricevuto dal PC: " + in.name + "\n(Download/MultiScreen)" : "Ricezione interrotta: " + in.name, true);
    }

    /** Collegamento caduto: elimina i file a meta'. */
    void reset() {
        for (int i = 0; i < incoming.size(); i++) {
            In in = incoming.valueAt(i);
            closeQuiet(in);
            discard(in);
        }
        incoming.clear();
    }

    private void discard(In in) {
        if (in.file != null) in.file.delete();
        if (in.uri != null) {
            try {
                cr.delete(in.uri, null, null);
            } catch (Exception ignored) {
            }
        }
    }

    private static void closeQuiet(In in) {
        if (in.os != null) {
            try {
                in.os.close();
            } catch (Exception ignored) {
            }
        }
    }

    // ------------------------------------------------------------ invio al PC
    int pending() {
        return outQ.size();
    }

    synchronized void send(List<Uri> uris) {
        outQ.addAll(uris);
        if (sender == null || !sender.isAlive()) {
            sender = new Thread(new Runnable() {
                @Override
                public void run() {
                    sendLoop();
                }
            }, "ms-files");
            sender.start();
        }
    }

    private void sendLoop() {
        int sent = 0;
        String last = null;
        while (true) {
            if (!conn.isUp()) {
                // aspetta il collegamento (i file restano in coda)
                Connection.sleepQuiet(500);
                if (outQ.isEmpty()) break;
                continue;
            }
            Uri u = outQ.poll();
            if (u == null) break;
            String name = queryName(u);
            long size = querySize(u);
            int id = nextId++;
            boolean ok = false;
            InputStream is = null;
            try {
                is = cr.openInputStream(u);
                if (is == null) throw new Exception("non leggibile");
                JSONObject j = new JSONObject();
                j.put("id", id);
                j.put("name", name);
                j.put("size", size);
                String mime = cr.getType(u);
                j.put("mime", mime != null ? mime : "application/octet-stream");
                current = name;
                conn.sendBulk(Proto.packet(Proto.FILE_BEGIN, j.toString().getBytes(UTF8)));
                long done = 0;
                int lastPct = -1;
                while (true) {
                    byte[] p = Proto.alloc(Proto.FILE_DATA, 4 + CHUNK);
                    int n = 0;
                    while (n < CHUNK) {
                        int r = is.read(p, 9 + n, CHUNK - n);
                        if (r < 0) break;
                        n += r;
                    }
                    if (n == 0) break;
                    if (n < CHUNK) {
                        byte[] q = Proto.alloc(Proto.FILE_DATA, 4 + n);
                        System.arraycopy(p, 9, q, 9, n);
                        p = q;
                    }
                    Proto.put32(p, 5, id);
                    conn.sendBulk(p);
                    done += n;
                    if (size > 0) {
                        int pct = (int) (done * 100 / size);
                        if (pct / 5 != lastPct / 5) {
                            lastPct = pct;
                            ui.fileMessage("Invio al PC: " + name + " " + pct + "%", false);
                        }
                    }
                    if (n < CHUNK) break;
                }
                ok = true;
            } catch (Exception e) {
                Log.w(TAG, "invio " + name + ": " + e);
                ui.fileMessage("Invio non riuscito: " + name, true);
            } finally {
                if (is != null) {
                    try {
                        is.close();
                    } catch (Exception ignored) {
                    }
                }
            }
            byte[] e = Proto.alloc(Proto.FILE_END, 5);
            Proto.put32(e, 5, id);
            e[9] = (byte) (ok ? 1 : 0);
            try {
                conn.sendBulk(e);
            } catch (Exception ignored) {
            }
            if (ok) {
                sent++;
                last = name;
            }
            current = null;
        }
        if (sent > 1) ui.fileMessage("Inviati " + sent + " file al PC", false);
        else if (sent == 1) ui.fileMessage("Inviato al PC: " + last, false);
    }

    private String queryName(Uri u) {
        Cursor c = null;
        try {
            c = cr.query(u, new String[]{"_display_name"}, null, null, null);
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        String s = u.getLastPathSegment();
        return s != null ? s : "file";
    }

    private long querySize(Uri u) {
        Cursor c = null;
        try {
            c = cr.query(u, new String[]{"_size"}, null, null, null);
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        return -1;
    }

    static List<Uri> one(Uri u) {
        List<Uri> l = new ArrayList<Uri>();
        l.add(u);
        return l;
    }
}
