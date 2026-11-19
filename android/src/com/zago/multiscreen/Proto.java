package com.zago.multiscreen;

/** Costanti e helper del protocollo (vedi docs/protocol.md). Big endian. */
final class Proto {
    static final int PORT = 27183;

    // tablet -> PC
    static final int HELLO = 0x01;
    static final int TOUCH = 0x02;
    static final int TOUCH_CANCEL = 0x03;
    static final int POINTER = 0x04;
    static final int SCROLL = 0x05;
    static final int PING = 0x06;
    static final int KEYFRAME_REQ = 0x07;
    static final int CLIPBOARD = 0x08;
    static final int FILE_BEGIN = 0x09;
    static final int FILE_DATA = 0x0A;
    static final int FILE_END = 0x0B;
    static final int FILE_RESULT = 0x0C;
    static final int BYE = 0x0D;
    static final int PAUSE = 0x0E;

    // PC -> tablet
    static final int CONFIG = 0x41;
    static final int VIDEO = 0x42;
    static final int PONG = 0x43;
    static final int STATUS = 0x44;
    static final int CLIPBOARD_IN = 0x45;
    static final int FILE_BEGIN_IN = 0x46;
    static final int FILE_DATA_IN = 0x47;
    static final int FILE_END_IN = 0x48;
    static final int FILE_RESULT_IN = 0x49;

    static final int MAX_PACKET = 32 * 1024 * 1024;

    private Proto() {}

    static byte[] packet(int type, byte[] payload) {
        int len = payload == null ? 0 : payload.length;
        byte[] p = new byte[5 + len];
        p[0] = (byte) type;
        p[1] = (byte) (len >>> 24);
        p[2] = (byte) (len >>> 16);
        p[3] = (byte) (len >>> 8);
        p[4] = (byte) len;
        if (len > 0) System.arraycopy(payload, 0, p, 5, len);
        return p;
    }

    /** Pacchetto con payload gia' allocato dopo i 5 byte di header. */
    static byte[] alloc(int type, int payloadLen) {
        byte[] p = new byte[5 + payloadLen];
        p[0] = (byte) type;
        p[1] = (byte) (payloadLen >>> 24);
        p[2] = (byte) (payloadLen >>> 16);
        p[3] = (byte) (payloadLen >>> 8);
        p[4] = (byte) payloadLen;
        return p;
    }

    static void put16(byte[] b, int off, int v) {
        b[off] = (byte) (v >>> 8);
        b[off + 1] = (byte) v;
    }

    static void put32(byte[] b, int off, int v) {
        b[off] = (byte) (v >>> 24);
        b[off + 1] = (byte) (v >>> 16);
        b[off + 2] = (byte) (v >>> 8);
        b[off + 3] = (byte) v;
    }

    static int get32(byte[] b, int off) {
        return ((b[off] & 0xff) << 24) | ((b[off + 1] & 0xff) << 16) | ((b[off + 2] & 0xff) << 8) | (b[off + 3] & 0xff);
    }

    static void put64(byte[] b, int off, long v) {
        for (int i = 7; i >= 0; i--) {
            b[off + i] = (byte) v;
            v >>>= 8;
        }
    }

    static long get64(byte[] b, int off) {
        long v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | (b[off + i] & 0xff);
        return v;
    }

    /** Coordinata normalizzata 0..65535. */
    static int norm(float v, float size) {
        if (size <= 0) return 0;
        float n = v / size;
        if (n < 0) n = 0;
        if (n > 1) n = 1;
        return Math.round(n * 65535f);
    }
}
