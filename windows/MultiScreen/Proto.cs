namespace MultiScreen;

/// Protocollo PC <-> tablet (vedi docs/protocol.md). Big endian.
static class Proto
{
    public const int DevicePort = 27183;   // porta usata dall'app sul tablet
    public const int ControlPort = 27182;  // "Invia al tablet" (solo locale, con token)

    public const byte HELLO = 0x01, TOUCH = 0x02, TOUCH_CANCEL = 0x03, POINTER = 0x04, SCROLL = 0x05, PING = 0x06,
        KEYFRAME_REQ = 0x07, CLIPBOARD = 0x08, FILE_BEGIN = 0x09, FILE_DATA = 0x0A, FILE_END = 0x0B, FILE_RESULT = 0x0C,
        BYE = 0x0D, PAUSE = 0x0E;
    public const byte CONFIG = 0x41, VIDEO = 0x42, PONG = 0x43, STATUS = 0x44, CLIPBOARD_OUT = 0x45, FILE_BEGIN_OUT = 0x46,
        FILE_DATA_OUT = 0x47, FILE_END_OUT = 0x48, FILE_RESULT_OUT = 0x49;

    public static ushort U16(byte[] b, int o) => (ushort)((b[o] << 8) | b[o + 1]);
    public static uint U32(byte[] b, int o) => (uint)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]);
    public static void Put32(byte[] b, int o, uint v) { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }
    public static void Put64(byte[] b, int o, ulong v) { for (int i = 7; i >= 0; i--) { b[o + i] = (byte)v; v >>= 8; } }
}
