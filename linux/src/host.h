#pragma once
#include "common.h"
#include "files.h"
#include "kde.h"
#include "video.h"

typedef struct Host Host;
typedef struct Session Session;

typedef struct {
    int lw, lh;   /* dimensione logica schermo virtuale */
    double scale;
    int fps;
    int bitrate;  /* kbit/s */
    bool mirror;  /* duplica lo schermo del PC invece di estenderlo */
    char pos[12]; /* auto | right | left | top | bottom */
} Params;

struct Host {
    GMainLoop *loop;
    int base_port;
    char *encoder;
    int grace_s;
    double scroll_factor;
    char *apk;
    int apk_version;
    char token[40];
    bool check_token;
    Kde *kde;
    GList *sessions;      /* Session* */
    GMutex ports_lock;
    GHashTable *ports;    /* seriale -> porta (assegnata dal thread adb) */
    int next_port;
    char *clip_from_pc;
    bool kwin_physical;   /* KWin interpreta width/height come pixel fisici */
    guint64 last_active;  /* id dell'ultima sessione usata (per "Invia al tablet") */
};

Session *session_new(Host *h, const char *serial, const char *model, int port);
void session_free(Session *s);
guint64 session_id(Session *s);
const char *session_serial(Session *s);
const char *session_model(Session *s);
bool session_connected(Session *s);
void session_pc_clipboard(Session *s, const char *text, gsize len);
char *session_send_file(Session *s, const char *path);
void session_log_stats(Session *s);
