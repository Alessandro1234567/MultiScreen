#define _GNU_SOURCE
/* Una sessione per tablet: porta TCP propria (adb reverse), schermo virtuale, video, input, file, appunti. */
#include "adb.h"
#include "clip.h"
#include "host.h"

#include <errno.h>
#include <limits.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <linux/sockios.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

struct Session {
    Host *h;
    guint64 id;
    char serial[96], model[96];
    int index;           /* 1 = primo tablet */
    char out_name[32];   /* nome dello schermo virtuale in KWin */
    int port, listen_fd;
    guint listen_watch;

    int cfd;
    guint c_watch;
    GByteArray *rbuf;
    GMutex tx;           /* protegge cfd in scrittura e i campi sotto */
    bool config_sent;
    int sent_w, sent_h;
    bool need_key, paused;
    gint64 last_key_req;
    guint64 frames, bytes, dropped, lf, lb, ld;

    bool authed;
    int pointer_buttons;
    int maxw, maxh;      /* limite del decoder del tablet */

    KdeStream *stream;
    Video *video;
    Params params;
    bool have_session, session_ready, size_checked;
    guint grace_id;
    gint64 session_start;
    uint32_t node;
    char map_name[96];
    guint pos_timer;
    int pos_tries, pos_wait, pos_wait_appear;

    char *clip_from_tablet;
    Files *files;
    gint64 last_recreate;
    guint create_timer;   /* attesa che il vecchio schermo sparisca */
    int create_tries;
    Params pending;
};

static guint64 next_id_ = 1;

static void session_end(Session *s, const char *why);
static void client_close(Session *s, const char *why);
static void session_begin(Session *s, const Params *p);

/* le callback differite portano l'id: se la sessione non esiste piu' non fanno nulla */
static Session *lookup(Host *h, guint64 id) {
    for (GList *l = h->sessions; l; l = l->next)
        if (((Session *)l->data)->id == id) return l->data;
    return NULL;
}
typedef struct {
    Host *h;
    guint64 id;
    int what;
} Later;
enum { LATER_CLOSE_BROKEN, LATER_FORCE_KEY, LATER_RESCALE };
static gboolean later_cb(gpointer data);
static void later(Session *s, int what) {
    Later *l = g_new(Later, 1);
    l->h = s->h;
    l->id = s->id;
    l->what = what;
    g_idle_add(later_cb, l);
}

#define SLOG(s, lvl, fmt, ...) ms_log(lvl, "[%s] " fmt, (s)->model[0] ? (s)->model : (s)->serial, ##__VA_ARGS__)

/* ------------------------------------------------------------------ invio */
static bool write_all(int fd, struct iovec *iov, int n) {
    gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (n > 0) {
        ssize_t w = writev(fd, iov, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                gint64 left = deadline - g_get_monotonic_time();
                if (left <= 0) return false;
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                poll(&p, 1, (int)(left / 1000) + 1);
                continue;
            }
            return false;
        }
        while (n > 0 && (size_t)w >= iov->iov_len) {
            w -= iov->iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov->iov_base = (char *)iov->iov_base + w;
            iov->iov_len -= w;
        }
    }
    return true;
}

/* da chiamare con s->tx bloccato */
static bool send_locked(Session *s, int type, const void *a, size_t al, const void *b, size_t bl) {
    if (s->cfd < 0) return false;
    uint32_t len = (uint32_t)(al + bl);
    uint8_t hdr[5] = {(uint8_t)type, len >> 24, len >> 16, len >> 8, len};
    struct iovec iov[3] = {{hdr, 5}, {(void *)a, al}, {(void *)b, bl}};
    if (!write_all(s->cfd, iov, 3)) {
        SLOG(s, "WARN", "invio al tablet fallito: %s", g_strerror(errno));
        shutdown(s->cfd, SHUT_RDWR);
        later(s, LATER_CLOSE_BROKEN);
        return false;
    }
    return true;
}

static bool send2(int type, const void *a, size_t al, const void *b, size_t bl, void *user) {
    Session *s = user;
    g_mutex_lock(&s->tx);
    bool ok = send_locked(s, type, a, al, b, bl);
    g_mutex_unlock(&s->tx);
    return ok;
}

static void send_packet(Session *s, int type, const void *d, size_t dl) { send2(type, d, dl, NULL, 0, s); }

static void send_status(Session *s, const char *msg) {
    char *esc = g_strescape(msg, NULL);
    char *j = g_strdup_printf("{\"msg\":\"%s\"}", esc);
    send_packet(s, P_STATUS, j, strlen(j));
    g_free(j);
    g_free(esc);
}

static void throttle(void *user) {
    Session *s = user;
    for (int i = 0; i < 1000; i++) {
        int pending = 0, fd = s->cfd;
        if (fd < 0 || ioctl(fd, SIOCOUTQ, &pending) != 0 || pending < 128 * 1024) return;
        g_usleep(2000);
    }
}

static bool connected_cb(void *user) { return ((Session *)user)->cfd >= 0 && ((Session *)user)->authed; }

/* ------------------------------------------------------------------ video -> tablet */
static void request_key_locked(Session *s) {
    gint64 now = g_get_monotonic_time();
    if (now - s->last_key_req > 300 * 1000) {
        s->last_key_req = now;
        later(s, LATER_FORCE_KEY);
    }
}

static void on_sample(const guint8 *data, gsize size, guint64 pts_us, bool key, int w, int h, void *user) {
    Session *s = user;
    g_mutex_lock(&s->tx);
    if (!s->size_checked && w > 0) {
        s->size_checked = true;
        double expected = s->params.lw * s->params.scale;
        SLOG(s, "INFO", "video %dx%d (atteso circa %.0f di larghezza)", w, h, expected);
        if (!s->params.mirror && !s->h->kwin_physical && s->params.scale > 1.2 && w < expected * 0.8) {
            later(s, LATER_RESCALE);
            goto out;
        }
    }
    if (s->cfd < 0 || s->paused || !s->authed) goto out;
    if (!s->config_sent || w != s->sent_w || h != s->sent_h) {
        if (!key) {
            request_key_locked(s);
            goto out;
        }
        char j[160];
        snprintf(j, sizeof j, "{\"codec\":\"h264\",\"width\":%d,\"height\":%d,\"fps\":%d}", w, h, s->params.fps);
        if (!send_locked(s, P_CONFIG, NULL, 0, j, strlen(j))) goto out;
        SLOG(s, "INFO", "invio video %dx%d", w, h);
        s->config_sent = true;
        s->sent_w = w;
        s->sent_h = h;
        s->need_key = false;
    }
    if (s->need_key && !key) {
        s->dropped++;
        goto out;
    }
    if (!key) {
        /* se il collegamento USB rallenta non accumulare ritardo: salta frame e riparti da un keyframe */
        int pending = 0;
        if (ioctl(s->cfd, SIOCOUTQ, &pending) == 0 && pending > MAX(512 * 1024, s->params.bitrate * 1000 / 8 / 5)) {
            s->need_key = true;
            s->dropped++;
            request_key_locked(s);
            goto out;
        }
    }
    uint8_t vh[9];
    vh[0] = key ? 1 : 0;
    for (int i = 0; i < 8; i++) vh[1 + i] = (uint8_t)(pts_us >> (56 - 8 * i));
    if (send_locked(s, P_VIDEO, vh, sizeof vh, data, size)) {
        s->need_key = false;
        s->frames++;
        s->bytes += size;
    }
out:
    g_mutex_unlock(&s->tx);
}

static void on_video_started(Video *v, bool ok, const char *info, void *user) {
    Session *s = user;
    if (v != s->video) return;
    if (ok) {
        s->session_ready = true;
        SLOG(s, "INFO", "pronto (encoder %s) in %.1f s", info, (g_get_monotonic_time() - s->session_start) / 1e6);
        kde_log_outputs(s->h->kde);
    } else {
        SLOG(s, "ERRO", "nessun encoder video funziona: %s", info);
        char *m = g_strdup_printf("Errore video sul PC: %s", info);
        send_status(s, m);
        ms_notify("MultiScreen: errore video", info);
        g_free(m);
        session_end(s, "encoder non disponibile");
    }
}

static void on_video_error(Video *v, const char *msg, void *user) {
    Session *s = user;
    if (v != s->video) return;
    session_end(s, msg);
    if (s->cfd >= 0) client_close(s, "errore video"); /* l'app si ricollega e ricrea tutto */
}

static void start_video(Session *s) {
    VideoCallbacks cb = {.on_sample = on_sample, .on_started = on_video_started, .on_error = on_video_error, .user = s};
    s->video = video_start(s->node, s->params.bitrate, s->params.fps, s->h->encoder, &cb);
}

/* ------------------------------------------------------------------ posizione dello schermo */
static bool is_tablet_output(const char *name) { return strstr(name, MS_OUTPUT_NAME) != NULL; }

/* calcola dove mettere lo schermo del tablet e lo applica con kscreen-doctor */
static bool place_output(Session *s, bool apply) {
    KdeOut o[24];
    int n = kde_list_outputs(s->h->kde, o, 24), me = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(o[i].name, s->out_name) == 0 || (g_str_has_prefix(o[i].name, "Virtual-") && strcmp(o[i].name + 8, s->out_name) == 0))
            me = i;
    if (me < 0) return false;
    /* riferimento: lo schermo del PC in 0,0 (o il primo che non e' un tablet) */
    int ref = -1;
    for (int i = 0; i < n; i++)
        if (i != me && !is_tablet_output(o[i].name) && (ref < 0 || (o[i].x == 0 && o[i].y == 0))) ref = i;
    if (ref < 0) return true;
    int l = INT_MAX, t = INT_MAX, r = INT_MIN, b = INT_MIN;
    for (int i = 0; i < n; i++) {
        if (i == me) continue;
        l = MIN(l, o[i].x);
        t = MIN(t, o[i].y);
        r = MAX(r, o[i].x + o[i].w);
        b = MAX(b, o[i].y + o[i].h);
    }
    int x, y;
    const char *p = s->params.pos;
    if (strcmp(p, "left") == 0) {
        x = l - o[me].w;
        y = o[ref].y + (o[ref].h - o[me].h) / 2;
    } else if (strcmp(p, "top") == 0) {
        x = o[ref].x + (o[ref].w - o[me].w) / 2;
        y = t - o[me].h;
    } else if (strcmp(p, "bottom") == 0) {
        x = o[ref].x + (o[ref].w - o[me].w) / 2;
        y = b;
    } else {
        x = r;
        y = o[ref].y + (o[ref].h - o[me].h) / 2;
    }
    int nx[24], ny[24], minx = x, miny = y;
    for (int i = 0; i < n; i++) {
        nx[i] = i == me ? x : o[i].x;
        ny[i] = i == me ? y : o[i].y;
        minx = MIN(minx, nx[i]);
        miny = MIN(miny, ny[i]);
    }
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("kscreen-doctor"));
    for (int i = 0; i < n; i++) {
        nx[i] -= minx;
        ny[i] -= miny; /* KDE vuole coordinate non negative */
        if (nx[i] != o[i].x || ny[i] != o[i].y)
            g_ptr_array_add(argv, g_strdup_printf("output.%s.position.%d,%d", o[i].name, nx[i], ny[i]));
    }
    g_ptr_array_add(argv, NULL);
    bool changed = argv->len > 2;
    if (changed && apply) {
        SLOG(s, "INFO", "posizione schermo: %s (%d,%d)", p, nx[me], ny[me]);
        GError *e = NULL;
        if (!g_spawn_async(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                           NULL, &e)) {
            SLOG(s, "WARN", "kscreen-doctor: %s", e->message);
            g_clear_error(&e);
        }
    }
    g_ptr_array_free(argv, TRUE);
    return !changed;
}

/* ogni 250 ms: aspetta che lo schermo compaia (max 10 s), poi lo mette al suo posto (max 3 tentativi) */
static gboolean pos_tick(gpointer data) {
    Later *l = data;
    Session *s = lookup(l->h, l->id);
    if (!s || !s->have_session) return G_SOURCE_REMOVE;
    if (s->pos_wait > 0) {
        s->pos_wait--; /* lascia a KDE il tempo di applicare */
        return G_SOURCE_CONTINUE;
    }
    int x, y, w, hh;
    if (!kde_output_geometry(s->h->kde, s->out_name, &x, &y, &w, &hh)) {
        if (++s->pos_wait_appear < 40) return G_SOURCE_CONTINUE;
    } else if (!place_output(s, s->pos_tries < 3) && s->pos_tries++ < 3) {
        s->pos_wait = 5; /* ricontrolla: KDE potrebbe rimettere la posizione salvata */
        return G_SOURCE_CONTINUE;
    }
    s->pos_timer = 0;
    return G_SOURCE_REMOVE;
}

static void schedule_position(Session *s) {
    if (s->params.mirror || strcmp(s->params.pos, "auto") == 0) return;
    if (s->pos_timer) g_source_remove(s->pos_timer);
    s->pos_tries = s->pos_wait = s->pos_wait_appear = 0;
    Later *l = g_new(Later, 1);
    l->h = s->h;
    l->id = s->id;
    s->pos_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 250, pos_tick, l, g_free);
}

/* ------------------------------------------------------------------ sessione */
static void on_stream_event(KdeStream *ks, int ev, uint32_t node, const char *err, void *user) {
    Session *s = user;
    if (ks != s->stream) return;
    if (ev == KDE_STREAM_CREATED) {
        SLOG(s, "INFO", "schermo %s, nodo PipeWire %u", s->params.mirror ? "duplicato" : "virtuale creato", node);
        s->node = node;
        start_video(s);
        schedule_position(s);
    } else if (ev == KDE_STREAM_FAILED) {
        SLOG(s, "ERRO", "KWin non ha creato lo schermo virtuale: %s", err ? err : "?");
        char *m = g_strdup_printf("KWin: %s", err ? err : "errore");
        send_status(s, m);
        g_free(m);
        session_end(s, "stream fallito");
    } else {
        SLOG(s, "WARN", "KWin ha chiuso lo schermo");
        Params p = s->params;
        session_end(s, "chiuso da KWin");
        gint64 now = g_get_monotonic_time();
        if (session_connected(s) && now - s->last_recreate > 10 * G_USEC_PER_SEC) {
            s->last_recreate = now; /* lo ricreo (una volta ogni 10 s al massimo) */
            g_mutex_lock(&s->tx);
            s->config_sent = false;
            s->need_key = true;
            g_mutex_unlock(&s->tx);
            session_begin(s, &p);
        } else if (s->cfd >= 0) {
            client_close(s, "schermo chiuso");
        }
    }
}

static gboolean create_tick(gpointer data);

static void session_begin(Session *s, const Params *p) {
    Host *h = s->h;
    int x, y, w, hh;
    /* se lo schermo vecchio con lo stesso nome c'e' ancora, aspetta che KWin lo tolga (max 3 s) */
    if (!p->mirror && !g_getenv("MULTISCREEN_TEST_MIRROR") && h->kde &&
        kde_output_geometry(h->kde, s->out_name, &x, &y, &w, &hh) && s->create_tries < 30) {
        s->pending = *p;
        s->params = *p;
        s->have_session = true; /* sessione "in arrivo" */
        if (!s->create_timer) {
            Later *l = g_new(Later, 1);
            l->h = h;
            l->id = s->id;
            s->create_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, create_tick, l, g_free);
        }
        return;
    }
    s->create_tries = 0;
    s->params = *p;
    s->have_session = true;
    s->session_ready = false;
    s->session_start = g_get_monotonic_time();
    g_mutex_lock(&s->tx);
    s->size_checked = p->mirror || g_getenv("MULTISCREEN_TEST_MIRROR");
    g_mutex_unlock(&s->tx);
    if (p->mirror || g_getenv("MULTISCREEN_TEST_MIRROR")) {
        s->stream = kde_stream_existing(h->kde, s->map_name, sizeof s->map_name, on_stream_event, s);
        SLOG(s, "INFO", "modalita' duplica: schermo '%s'", s->map_name);
    } else {
        int cw = h->kwin_physical ? (int)lround(p->lw * p->scale) : p->lw;
        int ch = h->kwin_physical ? (int)lround(p->lh * p->scale) : p->lh;
        SLOG(s, "INFO", "creo '%s' %dx%d logici, scala %.2f (%dx%d pixel), %d fps, %d kbit/s, posizione %s", s->out_name,
             p->lw, p->lh, p->scale, (int)lround(p->lw * p->scale), (int)lround(p->lh * p->scale), p->fps, p->bitrate,
             p->pos);
        g_strlcpy(s->map_name, s->out_name, sizeof s->map_name);
        s->stream = kde_stream_virtual(h->kde, s->out_name, cw, ch, p->scale, on_stream_event, s);
    }
    if (!s->stream) {
        s->have_session = false;
        SLOG(s, "ERRO", "KWin non permette di creare lo schermo (zkde_screencast assente)");
        send_status(s, "Il PC non permette di creare lo schermo virtuale: riesegui l'installazione");
    }
}

static gboolean create_tick(gpointer data) {
    Later *l = data;
    Session *s = lookup(l->h, l->id);
    if (!s) return G_SOURCE_REMOVE;
    s->create_timer = 0;
    s->create_tries++;
    s->have_session = false;
    Params p = s->pending;
    session_begin(s, &p);
    return G_SOURCE_REMOVE;
}

static void session_end(Session *s, const char *why) {
    if (s->create_timer) {
        g_source_remove(s->create_timer);
        s->create_timer = 0;
        s->create_tries = 0;
    }
    if (s->grace_id) {
        g_source_remove(s->grace_id);
        s->grace_id = 0;
    }
    if (s->pos_timer) {
        g_source_remove(s->pos_timer);
        s->pos_timer = 0;
    }
    if (!s->have_session) return;
    SLOG(s, "INFO", "chiudo lo schermo (%s)", why);
    Video *v = s->video;
    s->video = NULL;
    if (v) video_stop(v);
    KdeStream *ks = s->stream;
    s->stream = NULL;
    if (ks) kde_stream_close(ks);
    s->have_session = false;
    s->session_ready = false;
    s->node = 0;
}

static gboolean grace_expired(gpointer data) {
    Later *l = data;
    Session *s = lookup(l->h, l->id);
    if (s) {
        s->grace_id = 0;
        session_end(s, "tablet non piu' in uso");
    }
    return G_SOURCE_REMOVE;
}

static void start_grace(Session *s) {
    if (!s->have_session || s->grace_id) return;
    Later *l = g_new(Later, 1);
    l->h = s->h;
    l->id = s->id;
    s->grace_id = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, s->h->grace_s, grace_expired, l, g_free);
}

static gboolean later_cb(gpointer data) {
    Later *l = data;
    Session *s = lookup(l->h, l->id);
    if (s) {
        if (l->what == LATER_CLOSE_BROKEN) {
            if (s->cfd >= 0) client_close(s, "invio bloccato");
        } else if (l->what == LATER_FORCE_KEY) {
            video_force_keyframe(s->video);
        } else if (l->what == LATER_RESCALE && s->have_session && !s->h->kwin_physical) {
            s->h->kwin_physical = true;
            SLOG(s, "WARN", "KWin interpreta le dimensioni come pixel fisici: ricreo lo schermo");
            Params p = s->params;
            session_end(s, "correzione dimensioni");
            session_begin(s, &p);
        }
    }
    g_free(l);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ hello */
static void compute_params(const char *hello, Params *p) {
    int tw = (int)json_num(hello, "width", 1920), th = (int)json_num(hello, "height", 1200);
    double dpi = json_num(hello, "dpi", 0), density = json_num(hello, "density", 2);
    int res = CLAMP((int)json_num(hello, "res", 75), 25, 100);
    char ui[16] = "normal";
    json_str(hello, "ui", ui, sizeof ui);
    double div = strcmp(ui, "small") == 0 ? 210 : strcmp(ui, "large") == 0 ? 140 : 170;
    double native = dpi > 60 ? dpi / div : density * 0.9;
    double pw = tw * res / 100.0, ph = th * res / 100.0;
    /* rispetta il limite del decoder del tablet (tablet vecchi: spesso 1920x1080) */
    double maxw = json_num(hello, "maxw", 0), maxh = json_num(hello, "maxh", 0);
    double fit = 1.0;
    if (maxw > 0 && pw > maxw) fit = MIN(fit, maxw / pw);
    if (maxh > 0 && ph > maxh) fit = MIN(fit, maxh / ph);
    pw *= fit;
    ph *= fit;
    double sc = round(native * res / 100.0 * fit * 4) / 4;
    p->scale = CLAMP(sc, 1.0, 4.0);
    p->lw = MAX(320, (int)floor(pw / p->scale / 8) * 8);
    p->lh = MAX(240, (int)floor(ph / p->scale / 8) * 8);
    p->fps = CLAMP((int)json_num(hello, "fps", 60), 10, 60);
    p->bitrate = CLAMP((int)json_num(hello, "bitrate", 20), 2, 150) * 1000;
    char mode[16] = "extend";
    json_str(hello, "mode", mode, sizeof mode);
    p->mirror = strcmp(mode, "mirror") == 0;
    g_strlcpy(p->pos, "right", sizeof p->pos);
    json_str(hello, "pos", p->pos, sizeof p->pos);
}

static bool same_screen(const Params *x, const Params *y) {
    if (x->mirror || y->mirror) return x->mirror == y->mirror;
    return x->lw == y->lw && x->lh == y->lh && fabs(x->scale - y->scale) < 0.01;
}

static void handle_hello(Session *s, const char *json) {
    Host *h = s->h;
    char token[64] = "";
    json_str(json, "token", token, sizeof token);
    json_str(json, "model", s->model, sizeof s->model);
    if (h->check_token && strcmp(token, h->token) != 0) {
        SLOG(s, "WARN", "codice di sicurezza mancante o sbagliato: rilancio l'app con quello giusto");
        s->authed = false;
        send_status(s, "Collegamento in verifica…");
        if (s->serial[0]) adb_request_launch(s->serial);
        return;
    }
    s->authed = true;
    h->last_active = s->id;
    Params p;
    compute_params(json, &p);
    SLOG(s, "INFO", "HELLO: %.0fx%.0f, scala %.2f, %d fps, %d kbit/s, %s, %s", json_num(json, "width", 0),
         json_num(json, "height", 0), p.scale, p.fps, p.bitrate, p.mirror ? "duplica" : "estendi", p.pos);
    s->maxw = (int)json_num(json, "maxw", 0);
    s->maxh = (int)json_num(json, "maxh", 0);
    if (s->grace_id) {
        g_source_remove(s->grace_id);
        s->grace_id = 0;
    }
    if (!h->kde || !kde_has_screencast(h->kde)) {
        send_status(s, "Il PC non e' pronto: controlla che MultiScreen sia installato bene");
        return;
    }
    g_mutex_lock(&s->tx);
    s->config_sent = false;
    s->need_key = true;
    s->paused = false;
    g_mutex_unlock(&s->tx);
    if (s->have_session && same_screen(&p, &s->params)) {
        bool pos_changed = strcmp(p.pos, s->params.pos) != 0;
        /* cambiano solo fps/qualita'/posizione: niente da ricreare, le finestre restano dove sono */
        if (p.fps != s->params.fps || p.bitrate != s->params.bitrate) video_update(s->video, p.bitrate, p.fps);
        else video_force_keyframe(s->video);
        s->params = p;
        if (pos_changed) schedule_position(s);
        return;
    }
    session_end(s, "nuovi parametri");
    send_status(s, "Creo lo schermo…");
    session_begin(s, &p);
}

/* ------------------------------------------------------------------ input */
static bool map_xy(Session *s, unsigned nx, unsigned ny, double *x, double *y) {
    int ox, oy, ow, oh;
    const char *name = g_getenv("MULTISCREEN_TEST_MIRROR") ? "" : s->map_name;
    if (!kde_output_geometry(s->h->kde, name, &ox, &oy, &ow, &oh)) {
        static gint64 last;
        if (g_get_monotonic_time() - last > 5 * G_USEC_PER_SEC) {
            last = g_get_monotonic_time();
            SLOG(s, "WARN", "input: non trovo lo schermo '%s' in KWin", name);
            kde_log_outputs(s->h->kde);
        }
        return false;
    }
    *x = ox + (nx / 65535.0) * (ow - 0.01);
    *y = oy + (ny / 65535.0) * (oh - 0.01);
    return true;
}

static unsigned rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static void set_buttons(Session *s, int b) {
    static const uint32_t codes[3] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
    for (int i = 0; i < 3; i++) {
        int bit = 1 << i;
        if ((b & bit) != (s->pointer_buttons & bit)) kde_button(s->h->kde, codes[i], (b & bit) != 0);
    }
    s->pointer_buttons = b;
}

static void release_input(Session *s) {
    if (!s->h->kde) return;
    kde_touch_cancel(s->h->kde);
    set_buttons(s, 0);
    kde_flush(s->h->kde);
}

static void handle_packet(Session *s, int type, const uint8_t *d, uint32_t len) {
    Host *h = s->h;
    if (type == P_HELLO) {
        char *j = g_strndup((const char *)d, len);
        handle_hello(s, j);
        g_free(j);
        return;
    }
    if (!s->authed) return; /* niente input, file o appunti senza codice valido */
    Kde *k = h->kde;
    switch (type) {
    case P_TOUCH: {
        if (len < 1 || !s->session_ready) break;
        int n = d[0];
        if (len < 1u + n * 6u) break;
        for (int i = 0; i < n; i++) {
            const uint8_t *e = d + 1 + i * 6;
            double x, y;
            if (!map_xy(s, rd16(e + 2), rd16(e + 4), &x, &y)) break;
            /* id distinti per tablet diversi */
            uint32_t id = e[0] + 32 * (s->index - 1);
            if (e[1] == 0) kde_touch_down(k, id, x, y);
            else if (e[1] == 1) kde_touch_motion(k, id, x, y);
            else kde_touch_up(k, id);
        }
        kde_touch_frame(k);
        kde_flush(k);
        h->last_active = s->id;
        break;
    }
    case P_TOUCH_CANCEL:
        if (k) {
            kde_touch_cancel(k);
            kde_flush(k);
        }
        break;
    case P_POINTER: {
        if (len < 5 || !s->session_ready) break;
        double x, y;
        if (map_xy(s, rd16(d + 1), rd16(d + 3), &x, &y)) kde_pointer_abs(k, x, y);
        set_buttons(s, d[0]);
        kde_flush(k);
        h->last_active = s->id;
        break;
    }
    case P_SCROLL: {
        if (len < 4 || !s->session_ready) break;
        int16_t dx = (int16_t)rd16(d), dy = (int16_t)rd16(d + 2);
        if (dy) kde_axis(k, 0, dy / 10.0 * h->scroll_factor);
        if (dx) kde_axis(k, 1, dx / 10.0 * h->scroll_factor);
        kde_flush(k);
        break;
    }
    case P_PING:
        send_packet(s, P_PONG, d, len);
        break;
    case P_KEYFRAME_REQ:
        g_mutex_lock(&s->tx);
        request_key_locked(s);
        g_mutex_unlock(&s->tx);
        break;
    case P_CLIPBOARD:
        if (len == 0) break;
        g_free(s->clip_from_tablet);
        s->clip_from_tablet = g_strndup((const char *)d, len);
        if (h->clip_from_pc && strcmp(h->clip_from_pc, s->clip_from_tablet) == 0) break;
        SLOG(s, "INFO", "appunti: %u byte dal tablet", len);
        clip_set((const char *)d, len);
        break;
    case P_FILE_BEGIN: {
        char *j = g_strndup((const char *)d, len);
        files_on_begin(s->files, j);
        g_free(j);
        break;
    }
    case P_FILE_DATA:
        files_on_data(s->files, d, len);
        break;
    case P_FILE_END:
        files_on_end(s->files, d, len);
        break;
    case P_FILE_RESULT: {
        char *j = g_strndup((const char *)d, len);
        files_on_result(s->files, j);
        g_free(j);
        break;
    }
    case P_PAUSE:
        SLOG(s, "INFO", "app in background: pausa video");
        g_mutex_lock(&s->tx);
        s->paused = true;
        g_mutex_unlock(&s->tx);
        release_input(s);
        start_grace(s);
        break;
    case P_BYE:
        SLOG(s, "INFO", "disconnessione chiesta dal tablet");
        session_end(s, "disconnessione dal tablet");
        client_close(s, "disconnesso dal tablet");
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ socket */
static gboolean on_client_io(gint fd, GIOCondition cond, gpointer data) {
    Session *s = data;
    uint8_t buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r > 0) {
            g_byte_array_append(s->rbuf, buf, r);
            if (s->rbuf->len > 4 * 1024 * 1024) r = -1, errno = EMSGSIZE;
            else continue;
        }
        if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) {
            s->c_watch = 0;
            client_close(s, r == 0 ? "il tablet ha chiuso la connessione" : g_strerror(errno));
            return G_SOURCE_REMOVE;
        }
        if (errno == EAGAIN) break;
    }
    size_t off = 0;
    while (s->rbuf->len - off >= 5) {
        const uint8_t *p = s->rbuf->data + off;
        uint32_t len = ((uint32_t)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4];
        if (len > 1024 * 1024) {
            s->c_watch = 0;
            client_close(s, "pacchetto non valido");
            return G_SOURCE_REMOVE;
        }
        if (s->rbuf->len - off < 5 + len) break;
        int fd_before = s->cfd;
        handle_packet(s, p[0], p + 5, len);
        if (s->cfd != fd_before) return G_SOURCE_REMOVE; /* chiuso durante la gestione */
        off += 5 + len;
    }
    if (off) g_byte_array_remove_range(s->rbuf, 0, off);
    return G_SOURCE_CONTINUE;
}

static void client_close(Session *s, const char *why) {
    if (s->cfd < 0) return;
    SLOG(s, "INFO", "disconnesso: %s", why);
    if (s->c_watch) {
        g_source_remove(s->c_watch);
        s->c_watch = 0;
    }
    g_mutex_lock(&s->tx);
    close(s->cfd);
    s->cfd = -1;
    s->config_sent = false;
    g_mutex_unlock(&s->tx);
    s->authed = false;
    g_byte_array_set_size(s->rbuf, 0);
    files_client_gone(s->files);
    release_input(s);
    start_grace(s);
}

/* accetta solo connessioni da processi dello stesso utente (cioe' dal server adb) */
static bool peer_is_same_user(int fd) {
    struct sockaddr_in peer;
    socklen_t sl = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &sl) != 0) return false;
    char want[16];
    snprintf(want, sizeof want, ":%04X", ntohs(peer.sin_port));
    char *tcp = NULL;
    if (!g_file_get_contents("/proc/net/tcp", &tcp, NULL, NULL)) return true; /* non verificabile: non bloccare */
    bool ok = false;
    char **lines = g_strsplit(tcp, "\n", -1);
    for (int i = 1; lines[i]; i++) {
        char local[64], remote[64];
        unsigned uid;
        if (sscanf(lines[i], " %*d: %63s %63s %*x %*s %*s %*s %u", local, remote, &uid) != 3) continue;
        if (g_str_has_prefix(local, "0100007F") && g_str_has_suffix(local, want)) {
            ok = uid == getuid();
            break;
        }
    }
    g_strfreev(lines);
    g_free(tcp);
    return ok;
}

static gboolean on_accept(gint fd, GIOCondition cond, gpointer data) {
    Session *s = data;
    int c = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) return G_SOURCE_CONTINUE;
    if (!peer_is_same_user(c)) {
        SLOG(s, "WARN", "connessione rifiutata: non viene da adb di questo utente");
        close(c);
        return G_SOURCE_CONTINUE;
    }
    if (s->cfd >= 0) client_close(s, "nuova connessione"); /* l'ultimo vince */
    int one = 1, sb = 1 << 20;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    g_mutex_lock(&s->tx);
    s->cfd = c;
    s->config_sent = false;
    s->need_key = true;
    s->paused = false;
    g_mutex_unlock(&s->tx);
    s->authed = false;
    s->pointer_buttons = 0;
    s->c_watch = g_unix_fd_add(c, G_IO_IN | G_IO_HUP | G_IO_ERR, on_client_io, s);
    SLOG(s, "INFO", "app collegata (porta %d)", s->port);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ API */
Session *session_new(Host *h, const char *serial, const char *model, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0) {
        LOGE("porta %d occupata (MultiScreen e' gia' in esecuzione?): %s", port, g_strerror(errno));
        close(fd);
        return NULL;
    }
    Session *s = g_new0(Session, 1);
    s->h = h;
    s->id = next_id_++;
    g_strlcpy(s->serial, serial, sizeof s->serial);
    g_strlcpy(s->model, model ? model : "", sizeof s->model);
    /* primo indice libero: il primo tablet si chiama sempre "MultiScreen" (KDE ricorda le sue impostazioni) */
    for (s->index = 1;; s->index++) {
        bool used = false;
        for (GList *l = h->sessions; l; l = l->next) used |= ((Session *)l->data)->index == s->index;
        if (!used) break;
    }
    if (s->index == 1) g_strlcpy(s->out_name, MS_OUTPUT_NAME, sizeof s->out_name);
    else snprintf(s->out_name, sizeof s->out_name, MS_OUTPUT_NAME "-%d", s->index);
    s->port = port;
    s->listen_fd = fd;
    s->cfd = -1;
    g_mutex_init(&s->tx);
    s->rbuf = g_byte_array_new();
    FilesIO fio = {.send = send2, .throttle = throttle, .connected = connected_cb, .user = s};
    s->files = files_new(&fio);
    s->listen_watch = g_unix_fd_add(fd, G_IO_IN, on_accept, s);
    h->sessions = g_list_append(h->sessions, s);
    SLOG(s, "INFO", "tablet pronto: porta %d, schermo '%s'", port, s->out_name);
    return s;
}

void session_free(Session *s) {
    if (!s) return;
    if (s->cfd >= 0) client_close(s, "cavo scollegato");
    session_end(s, "cavo scollegato");
    s->h->sessions = g_list_remove(s->h->sessions, s);
    g_source_remove(s->listen_watch);
    close(s->listen_fd);
    files_free(s->files);
    g_byte_array_free(s->rbuf, TRUE);
    g_free(s->clip_from_tablet);
    g_mutex_clear(&s->tx);
    g_free(s);
}

guint64 session_id(Session *s) { return s->id; }
const char *session_serial(Session *s) { return s->serial; }
const char *session_model(Session *s) { return s->model[0] ? s->model : s->serial; }
bool session_connected(Session *s) { return s->cfd >= 0 && s->authed; }

void session_pc_clipboard(Session *s, const char *text, gsize len) {
    if (!session_connected(s)) return;
    if (s->clip_from_tablet && strlen(s->clip_from_tablet) == len && memcmp(s->clip_from_tablet, text, len) == 0) return;
    send_packet(s, P_CLIPBOARD_OUT, text, len);
}

char *session_send_file(Session *s, const char *path) { return files_send_path(s->files, path); }

void session_log_stats(Session *s) {
    g_mutex_lock(&s->tx);
    guint64 f = s->frames, b = s->bytes, d = s->dropped;
    g_mutex_unlock(&s->tx);
    if (s->cfd >= 0 && (f != s->lf || d != s->ld))
        SLOG(s, "INFO", "stat: %.1f fps, %.2f Mbit/s, %llu frame saltati", (f - s->lf) / 10.0,
             (b - s->lb) * 8 / 10.0 / 1e6, (unsigned long long)(d - s->ld));
    s->lf = f;
    s->lb = b;
    s->ld = d;
}
