package com.zago.multiscreen;

import android.util.Log;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.Charset;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Connessione al PC. Si collega a 127.0.0.1:PORT (inoltrato dal PC via "adb reverse")
 * e riprova finche' non viene fermata.
 */
final class Connection {
    private static final String TAG = "MultiScreen";
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final byte[] POISON = new byte[0];

    interface Listener {
        /** Socket aperto, HELLO inviato. */
        void onLinkUp();
        void onConfig(JSONObject cfg);
        /** I dati sono validi solo durante la chiamata. */
        void onVideo(byte[] data, int off, int len, long ptsUs, boolean key);
        void onPong(long rttNs);
        void onStatus(String msg);
        void onLinkDown(boolean hadSession);
        /** Pacchetti di appunti/file: dati validi solo durante la chiamata. */
        void onOther(int type, byte[] data, int len);
    }

    private final Listener listener;
    private volatile boolean running;
    private volatile Socket socket;
    private Thread thread;
    private final LinkedBlockingQueue<byte[]> outQ = new LinkedBlockingQueue<byte[]>(4096);
    /** coda per i file: piccola e bloccante, ha priorita' piu' bassa dell'input */
    private final java.util.concurrent.ArrayBlockingQueue<byte[]> fileQ = new java.util.concurrent.ArrayBlockingQueue<byte[]>(6);

    Connection(Listener l) {
        listener = l;
    }

    synchronized void start() {
        if (running) return;
        running = true;
        thread = new Thread(new Runnable() {
            @Override
            public void run() {
                loop();
            }
        }, "ms-conn");
        thread.start();
    }

    synchronized void stop() {
        running = false;
        closeSocket();
        if (thread != null) thread.interrupt();
        thread = null;
    }

    boolean isUp() {
        return socket != null;
    }

    /** Accoda un pacchetto da inviare (thread-safe, non blocca). */
    void send(byte[] pkt) {
        if (socket == null) return;
        if (!outQ.offer(pkt)) {
            outQ.clear(); // tablet troppo veloce o PC bloccato: meglio perdere input che accumulare ritardo
        }
    }

    /** Accoda un pezzo di file; blocca se la coda e' piena. Eccezione se il collegamento cade. */
    void sendBulk(byte[] pkt) throws IOException, InterruptedException {
        while (true) {
            if (socket == null) throw new IOException("non collegato");
            if (fileQ.offer(pkt, 200, TimeUnit.MILLISECONDS)) return;
        }
    }

    private void closeSocket() {
        Socket s = socket;
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }

    private void loop() {
        byte[] buf = new byte[1 << 20];
        while (running) {
            Socket s = new Socket();
            boolean hadSession = false;
            Thread writer = null;
            try {
                s.setTcpNoDelay(true);
                s.setReceiveBufferSize(1 << 21);
                s.connect(new InetSocketAddress("127.0.0.1", Proto.PORT), 1000);
                socket = s;
                outQ.clear();
                writer = startWriter(s.getOutputStream());
                listener.onLinkUp();
                DataInputStream in = new DataInputStream(new BufferedInputStream(s.getInputStream(), 1 << 18));
                while (running) {
                    int type = in.readUnsignedByte();
                    int len = in.readInt();
                    if (len < 0 || len > Proto.MAX_PACKET) throw new IOException("pacchetto non valido " + len);
                    if (len > buf.length) buf = new byte[Math.max(len, buf.length * 2)];
                    in.readFully(buf, 0, len);
                    switch (type) {
                        case Proto.CONFIG:
                            hadSession = true;
                            listener.onConfig(new JSONObject(new String(buf, 0, len, UTF8)));
                            break;
                        case Proto.VIDEO:
                            if (len >= 9) {
                                boolean key = (buf[0] & 1) != 0;
                                long pts = Proto.get64(buf, 1);
                                listener.onVideo(buf, 9, len - 9, pts, key);
                            }
                            break;
                        case Proto.PONG:
                            if (len >= 8) listener.onPong(System.nanoTime() - Proto.get64(buf, 0));
                            break;
                        case Proto.STATUS:
                            try {
                                listener.onStatus(new JSONObject(new String(buf, 0, len, UTF8)).optString("msg"));
                            } catch (Exception ignored) {
                            }
                            break;
                        default:
                            listener.onOther(type, buf, len);
                            break;
                    }
                }
            } catch (EOFException e) {
                // il PC ha chiuso (o adb reverse senza host in ascolto)
            } catch (Exception e) {
                if (running) Log.d(TAG, "connessione: " + e);
            } finally {
                socket = null;
                try {
                    s.close();
                } catch (IOException ignored) {
                }
                fileQ.clear();
                if (writer != null) {
                    outQ.clear();
                    outQ.offer(POISON);
                    writer.interrupt();
                }
                listener.onLinkDown(hadSession);
            }
            if (!running) break;
            try {
                Thread.sleep(hadSession ? 300 : 700);
            } catch (InterruptedException e) {
                break;
            }
        }
    }

    private Thread startWriter(final OutputStream raw) {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                BufferedOutputStream out = new BufferedOutputStream(raw, 1 << 14);
                try {
                    while (true) {
                        byte[] p = outQ.poll();
                        if (p == null) p = fileQ.poll();
                        if (p == null) {
                            out.flush();
                            p = outQ.poll(15, TimeUnit.MILLISECONDS);
                            if (p == null) continue;
                        }
                        if (p == POISON) break;
                        out.write(p);
                    }
                } catch (Exception ignored) {
                }
            }
        }, "ms-writer");
        t.start();
        return t;
    }

    /** Usato per il ping periodico. */
    void ping() {
        byte[] p = Proto.alloc(Proto.PING, 8);
        Proto.put64(p, 5, System.nanoTime());
        send(p);
    }

    static void sleepQuiet(long ms) {
        try {
            TimeUnit.MILLISECONDS.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }
}
