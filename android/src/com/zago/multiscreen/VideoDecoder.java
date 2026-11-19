package com.zago.multiscreen;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/** Decoder H.264 hardware a bassa latenza: ogni frame viene mostrato appena decodificato. */
final class VideoDecoder {
    private static final String TAG = "MultiScreen";
    private static final String MIME = "video/avc";

    interface Listener {
        void onFrameRendered(boolean first);
        void onNeedKeyframe();
        void onDecoderFailed(String why);
    }

    private final Listener listener;
    private Surface surface;
    private MediaCodec codec;
    private HandlerThread thread;
    private int generation;
    private final LinkedBlockingQueue<Integer> freeInputs = new LinkedBlockingQueue<Integer>();
    private volatile boolean waitKey = true;
    private volatile boolean firstRendered;
    private int width, height;
    private String codecName = "";

    VideoDecoder(Listener l) {
        listener = l;
    }

    String name() {
        return codecName;
    }

    synchronized void setSurface(Surface s) {
        surface = s;
    }

    /** (Ri)crea il decoder per la risoluzione indicata. */
    synchronized boolean configure(int w, int h, int fps) {
        releaseLocked();
        if (surface == null || !surface.isValid()) return false;
        width = w;
        height = h;
        generation++;
        final int gen = generation;
        thread = new HandlerThread("ms-dec", Process.THREAD_PRIORITY_URGENT_DISPLAY);
        thread.start();
        // tentativi: prima con tutte le opzioni di bassa latenza, poi via via piu' semplici
        for (int attempt = 0; attempt < 3; attempt++) {
            MediaCodec c = null;
            try {
                String name = attempt < 2 ? pickDecoder() : null;
                c = name != null ? MediaCodec.createByCodecName(name) : MediaCodec.createDecoderByType(MIME);
                MediaFormat f = MediaFormat.createVideoFormat(MIME, w, h);
                f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, Math.max(w * h, 1 << 20));
                if (attempt < 2) {
                    f.setInteger("low-latency", 1);
                    f.setInteger(MediaFormat.KEY_PRIORITY, 0);
                    f.setInteger("vendor.qti-ext-dec-low-latency.enable", 1);
                    f.setInteger("vendor.qti-ext-dec-picture-order.enable", 1);
                    f.setInteger("vendor.low-latency.enable", 1);
                }
                if (attempt == 0) f.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE);
                if (fps > 0) f.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
                c.setCallback(new Callback(gen), new Handler(thread.getLooper()));
                c.configure(f, surface, null, 0);
                c.start();
                codec = c;
                codecName = c.getName();
                waitKey = true;
                firstRendered = false;
                Log.i(TAG, "decoder " + codecName + " " + w + "x" + h + " tentativo " + attempt);
                return true;
            } catch (Exception e) {
                Log.w(TAG, "configure tentativo " + attempt + " fallito: " + e);
                if (c != null) {
                    try {
                        c.release();
                    } catch (Exception ignored) {
                    }
                }
            }
        }
        listener.onDecoderFailed("impossibile avviare il decoder video");
        return false;
    }

    private static String pickDecoder() {
        String best = null;
        try {
            MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
            for (MediaCodecInfo info : list.getCodecInfos()) {
                if (info.isEncoder()) continue;
                String n = info.getName();
                String ln = n.toLowerCase();
                if (ln.startsWith("omx.google.") || ln.startsWith("c2.android.") || ln.contains(".sw.")
                        || ln.contains("secure")) continue;
                boolean avc = false;
                for (String t : info.getSupportedTypes()) if (t.equalsIgnoreCase(MIME)) avc = true;
                if (!avc) continue;
                if (ln.contains("low_latency") || ln.contains("lowlatency")) return n; // es. c2.qti.avc.decoder.low_latency
                if (best == null) best = n;
            }
        } catch (Exception ignored) {
        }
        return best;
    }

    /**
     * La risoluzione piu' grande (con le proporzioni date) che il decoder hardware regge.
     * Ritorna {0,0} se non si sa (nessun limite).
     */
    static int[] maxSize(int w, int h) {
        try {
            String name = pickDecoder();
            for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                if (info.isEncoder() || (name != null && !info.getName().equals(name))) continue;
                MediaCodecInfo.VideoCapabilities vc;
                try {
                    vc = info.getCapabilitiesForType(MIME).getVideoCapabilities();
                } catch (Exception e) {
                    continue;
                }
                if (vc.isSizeSupported(w, h)) return new int[]{w, h};
                for (int pct = 95; pct >= 30; pct -= 5) {
                    int ww = w * pct / 100 / 16 * 16, hh = h * pct / 100 / 16 * 16;
                    if (vc.isSizeSupported(ww, hh)) return new int[]{ww, hh};
                }
                return new int[]{0, 0};
            }
        } catch (Exception ignored) {
        }
        return new int[]{0, 0};
    }

    /** Accoda un access unit. Chiamato dal thread di rete. */
    void queue(byte[] data, int off, int len, long ptsUs, boolean key) {
        int gen;
        MediaCodec c;
        synchronized (this) {
            c = codec;
            gen = generation;
        }
        if (c == null) return;
        if (waitKey) {
            if (!key) return;
            waitKey = false;
        }
        Integer idx;
        try {
            idx = freeInputs.poll(250, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            return;
        }
        if (idx == null) {
            // decoder bloccato: scarta e chiedi un nuovo keyframe
            waitKey = true;
            listener.onNeedKeyframe();
            return;
        }
        synchronized (this) {
            if (gen != generation || codec != c) return;
            try {
                ByteBuffer ib = c.getInputBuffer(idx);
                if (ib == null || ib.capacity() < len) {
                    c.queueInputBuffer(idx, 0, 0, ptsUs, 0);
                    waitKey = true;
                    listener.onNeedKeyframe();
                    return;
                }
                ib.clear();
                ib.put(data, off, len);
                c.queueInputBuffer(idx, 0, len, ptsUs, key ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);
            } catch (Exception e) {
                Log.w(TAG, "queue: " + e);
                waitKey = true;
                listener.onNeedKeyframe();
            }
        }
    }

    synchronized void release() {
        releaseLocked();
    }

    private void releaseLocked() {
        generation++;
        freeInputs.clear();
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            try {
                codec.release();
            } catch (Exception ignored) {
            }
            codec = null;
        }
        if (thread != null) {
            thread.quitSafely();
            thread = null;
        }
    }

    private final class Callback extends MediaCodec.Callback {
        private final int gen;

        Callback(int gen) {
            this.gen = gen;
        }

        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            if (gen == generation) freeInputs.offer(index);
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index, MediaCodec.BufferInfo info) {
            if (gen != generation) return;
            try {
                c.releaseOutputBuffer(index, true); // mostra subito
            } catch (Exception e) {
                return;
            }
            boolean first = !firstRendered;
            firstRendered = true;
            listener.onFrameRendered(first);
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            if (gen != generation) return;
            Log.w(TAG, "decoder error: " + e);
            listener.onDecoderFailed("errore decoder: " + e.getDiagnosticInfo());
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
        }
    }
}
