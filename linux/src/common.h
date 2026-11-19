#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MS_VERSION "1.3"
#define MS_PACKAGE "com.zago.multiscreen"
#define MS_ACTIVITY MS_PACKAGE "/.MainActivity"
#define MS_OUTPUT_NAME "MultiScreen"

/* protocollo, vedi docs/protocol.md */
enum {
    P_HELLO = 0x01, P_TOUCH = 0x02, P_TOUCH_CANCEL = 0x03, P_POINTER = 0x04,
    P_SCROLL = 0x05, P_PING = 0x06, P_KEYFRAME_REQ = 0x07,
    /* bidirezionali: stesso codice +0x40 da PC a tablet */
    P_CLIPBOARD = 0x08, P_FILE_BEGIN = 0x09, P_FILE_DATA = 0x0A, P_FILE_END = 0x0B, P_FILE_RESULT = 0x0C,
    P_BYE = 0x0D, P_PAUSE = 0x0E,
    P_CONFIG = 0x41, P_VIDEO = 0x42, P_PONG = 0x43, P_STATUS = 0x44,
    P_CLIPBOARD_OUT = 0x45, P_FILE_BEGIN_OUT = 0x46, P_FILE_DATA_OUT = 0x47, P_FILE_END_OUT = 0x48,
    P_FILE_RESULT_OUT = 0x49,
};

void ms_log_init(void);
void ms_log(const char *level, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
#define LOGI(...) ms_log("INFO", __VA_ARGS__)
#define LOGW(...) ms_log("WARN", __VA_ARGS__)
#define LOGE(...) ms_log("ERRO", __VA_ARGS__)
void ms_notify(const char *title, const char *body);

/* json minimale (solo chiavi uniche, valori semplici) */
double json_num(const char *json, const char *key, double def);
bool json_str(const char *json, const char *key, char *out, size_t outlen);
bool json_bool(const char *json, const char *key, bool def);
