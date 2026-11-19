#define _GNU_SOURCE
/* MultiScreen host per Linux (KDE Plasma Wayland).
 * Ogni tablet collegato via USB diventa uno schermo in piu': schermo virtuale in KWin, video H.264 hardware,
 * touch/penna, appunti e file. Una sessione per tablet (session.c). */
#include "adb.h"
#include "clip.h"
#include "host.h"

#include <errno.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static Host host_;

/* ------------------------------------------------------------------ KWin */
static gboolean on_kde_io(gint fd, GIOCondition cond, gpointer data) {
    Host *h = data;
    if (kde_dispatch(h->kde) < 0) {
        LOGE("connessione a KWin persa: esco (il servizio mi riavvia)");
        exit(3);
    }
    return G_SOURCE_CONTINUE;
}

static void explain_permission(void) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    LOGE("KWin non concede i protocolli zkde_screencast/fake_input a '%s'.", exe ? exe : "?");
    LOGE("Serve ~/.local/share/applications/com.zago.multiscreen.desktop con Exec=%s e "
         "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input (lo crea l'installazione), "
         "poi 'kbuildsycoca6' ed eventualmente logout/login.", exe ? exe : "?");
    g_free(exe);
}

static bool kde_init(Host *h, bool quiet) {
    char *err = NULL;
    h->kde = kde_connect(&err);
    if (!h->kde) {
        if (!quiet) LOGW("%s", err);
        g_free(err);
        return false;
    }
    LOGI("KWin: screencast v%u %s, fake_input %s", kde_screencast_version(h->kde),
         kde_has_screencast(h->kde) ? "OK" : "NON DISPONIBILE", kde_has_fake_input(h->kde) ? "OK" : "NON DISPONIBILE");
    if (!kde_has_screencast(h->kde) || !kde_has_fake_input(h->kde)) explain_permission();
    g_unix_fd_add(kde_fd(h->kde), G_IO_IN | G_IO_HUP | G_IO_ERR, on_kde_io, h);
    return true;
}

static gboolean kde_retry(gpointer data) {
    static int tries;
    return kde_init(data, ++tries % 15 != 1) ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ tablet (adb) */
static Session *by_serial(Host *h, const char *serial) {
    for (GList *l = h->sessions; l; l = l->next)
        if (strcmp(session_serial(l->data), serial) == 0) return l->data;
    return NULL;
}

/* thread adb: porta del PC per un tablet (stabile finche' il programma gira) */
static int port_for(const char *serial, void *user) {
    Host *h = user;
    g_mutex_lock(&h->ports_lock);
    int port = GPOINTER_TO_INT(g_hash_table_lookup(h->ports, serial));
    if (!port) {
        port = h->next_port++;
        g_hash_table_insert(h->ports, g_strdup(serial), GINT_TO_POINTER(port));
    }
    g_mutex_unlock(&h->ports_lock);
    return port;
}

static void on_adb_ready(const char *serial, const char *model, void *user) {
    Host *h = user;
    if (by_serial(h, serial)) return;
    if (!session_new(h, serial, model, port_for(serial, h))) ms_notify("MultiScreen", "Porta occupata: riavvia il PC");
}

static void on_adb_gone(const char *serial, void *user) {
    Session *s = by_serial(user, serial);
    if (s) session_free(s);
}

static void on_adb_message(const char *title, const char *body, void *user) {
    LOGI("%s: %s", title, body);
    ms_notify(title, body);
}

/* ------------------------------------------------------------------ appunti del PC -> tutti i tablet */
static void on_pc_clip(const char *text, gsize len, void *user) {
    Host *h = user;
    if (len > 1024 * 1024 - 16) return;
    char *t = g_strndup(text, len);
    bool same = h->clip_from_pc && strcmp(h->clip_from_pc, t) == 0;
    g_free(h->clip_from_pc);
    h->clip_from_pc = t;
    if (same) return;
    for (GList *l = h->sessions; l; l = l->next) session_pc_clipboard(l->data, text, len);
}

/* ------------------------------------------------------------------ socket di controllo ("Invia al tablet") */
static char *ctl_path(void) { return g_build_filename(g_get_user_runtime_dir(), "multiscreen.sock", NULL); }

/* il tablet usato per ultimo, altrimenti il primo collegato */
static Session *send_target(Host *h) {
    Session *first = NULL;
    for (GList *l = h->sessions; l; l = l->next) {
        Session *s = l->data;
        if (!session_connected(s)) continue;
        if (session_id(s) == h->last_active) return s;
        if (!first) first = s;
    }
    return first;
}

static gboolean on_ctl_client(gint fd, GIOCondition c, gpointer data) {
    Host *h = data;
    GString *in = g_string_new(NULL);
    char buf[4096];
    for (int i = 0; i < 2000; i++) { /* legge fino a EOF */
        ssize_t r = read(fd, buf, sizeof buf);
        if (r > 0) g_string_append_len(in, buf, r);
        else if (r < 0 && errno == EAGAIN) {
            struct pollfd p = {.fd = fd, .events = POLLIN};
            if (poll(&p, 1, 1000) <= 0) break;
        } else break;
    }
    GString *out = g_string_new(NULL);
    char **lines = g_strsplit(in->str, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "SEND ")) {
            Session *s = send_target(h);
            char *err = s ? session_send_file(s, lines[i] + 5) : g_strdup("nessun tablet collegato");
            if (err) g_string_append_printf(out, "ERR %s\n", err);
            else g_string_append_printf(out, "OK %s -> %s\n", lines[i] + 5, session_model(s));
            g_free(err);
        } else if (strcmp(lines[i], "STATUS") == 0) {
            if (!h->sessions) g_string_append(out, "nessun tablet\n");
            for (GList *l = h->sessions; l; l = l->next)
                g_string_append_printf(out, "%s: %s\n", session_model(l->data),
                                       session_connected(l->data) ? "in uso" : "collegato, app chiusa");
        }
    }
    g_strfreev(lines);
    if (write(fd, out->str, out->len) < 0) {
    }
    g_string_free(out, TRUE);
    g_string_free(in, TRUE);
    close(fd);
    return G_SOURCE_REMOVE;
}

static gboolean on_ctl_accept(gint fd, GIOCondition c, gpointer data) {
    int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd >= 0) g_unix_fd_add(cfd, G_IO_IN | G_IO_HUP, on_ctl_client, data);
    return G_SOURCE_CONTINUE;
}

static void ctl_listen(Host *h) {
    char *path = ctl_path();
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    g_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(fd, 8) == 0) {
        chmod(path, 0600);
        g_unix_fd_add(fd, G_IO_IN, on_ctl_accept, h);
    } else {
        LOGW("socket di controllo %s: %s", path, g_strerror(errno));
        close(fd);
    }
    g_free(path);
}

/* client: multiscreen-host --send FILE... */
static int ctl_command(const char *req) {
    char *path = ctl_path();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    g_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    g_free(path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "MultiScreen non e' in esecuzione (multiscreen start)\n");
        ms_notify("MultiScreen", "Il programma non e' in esecuzione");
        return 1;
    }
    size_t n = strlen(req), off = 0;
    while (off < n) {
        ssize_t w = write(fd, req + off, n - off);
        if (w <= 0) break;
        off += w;
    }
    shutdown(fd, SHUT_WR);
    char buf[4096];
    GString *resp = g_string_new(NULL);
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) g_string_append_len(resp, buf, r);
    close(fd);
    fputs(resp->str, stdout);
    int rc = 0;
    char *e = strstr(resp->str, "ERR ");
    if (e) {
        rc = 1;
        char *nl = strchr(e, '\n');
        if (nl) *nl = 0;
        ms_notify("Invio al tablet non riuscito", e + 4);
    }
    g_string_free(resp, TRUE);
    return rc;
}

static int send_files(int argc, char **argv) {
    GString *req = g_string_new(NULL);
    for (int i = 1; i < argc; i++) {
        char *abs = g_canonicalize_filename(argv[i], NULL);
        if (g_file_test(abs, G_FILE_TEST_IS_DIR)) { /* cartella: i file che contiene (primo livello) */
            GDir *dir = g_dir_open(abs, 0, NULL);
            const char *n;
            while (dir && (n = g_dir_read_name(dir))) {
                char *f = g_build_filename(abs, n, NULL);
                if (g_file_test(f, G_FILE_TEST_IS_REGULAR)) g_string_append_printf(req, "SEND %s\n", f);
                g_free(f);
            }
            if (dir) g_dir_close(dir);
        } else {
            g_string_append_printf(req, "SEND %s\n", abs);
        }
        g_free(abs);
    }
    if (argc < 2) g_string_append(req, "STATUS\n");
    int rc = ctl_command(req->str);
    g_string_free(req, TRUE);
    return rc;
}

/* ------------------------------------------------------------------ config */
static void load_token(Host *h) {
    char *dir = g_build_filename(g_get_user_config_dir(), "multiscreen", NULL);
    char *path = g_build_filename(dir, "token", NULL);
    char *t = NULL;
    if (g_file_get_contents(path, &t, NULL, NULL) && strlen(g_strstrip(t)) >= 16) {
        g_strlcpy(h->token, t, sizeof h->token);
    } else {
        for (int i = 0; i < 16; i++) snprintf(h->token + 2 * i, 3, "%02x", g_random_int_range(0, 256));
        g_mkdir_with_parents(dir, 0700);
        g_file_set_contents(path, h->token, -1, NULL);
        chmod(path, 0600);
    }
    g_free(t);
    g_free(path);
    g_free(dir);
}

static void load_config(Host *h) {
    h->base_port = 27183;
    h->encoder = g_strdup("auto");
    h->grace_s = 30;
    h->scroll_factor = 1.0;
    h->apk = g_build_filename(g_get_user_data_dir(), "multiscreen", "multiscreen.apk", NULL);
    char *vf = g_build_filename(g_get_user_data_dir(), "multiscreen", "apk-version", NULL);
    char *vs = NULL;
    if (g_file_get_contents(vf, &vs, NULL, NULL)) h->apk_version = atoi(vs);
    g_free(vs);
    g_free(vf);
    char *path = g_build_filename(g_get_user_config_dir(), "multiscreen", "host.conf", NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, 0, NULL)) {
        GError *e = NULL;
        int v = g_key_file_get_integer(kf, "host", "port", &e);
        if (!e && v > 0) h->base_port = v;
        g_clear_error(&e);
        char *s = g_key_file_get_string(kf, "host", "encoder", NULL);
        if (s) {
            g_free(h->encoder);
            h->encoder = g_strstrip(s);
        }
        v = g_key_file_get_integer(kf, "host", "grace_seconds", &e);
        if (!e && v > 0) h->grace_s = v;
        g_clear_error(&e);
        double d = g_key_file_get_double(kf, "host", "scroll_factor", &e);
        if (!e && d > 0) h->scroll_factor = d;
        g_clear_error(&e);
    }
    g_key_file_free(kf);
    g_free(path);
    load_token(h);
    h->check_token = !g_getenv("MULTISCREEN_NO_TOKEN");
}

/* ------------------------------------------------------------------ --selftest */
typedef struct {
    Host *h;
    Video *video;
    int frames;
    guint64 bytes;
    FILE *out;
    int result;
} SelfTest;

static void st_sample(const guint8 *d, gsize n, guint64 pts, bool key, int w, int hh, void *u) {
    SelfTest *t = u;
    if (t->frames == 0) LOGI("selftest: primo frame %dx%d", w, hh);
    t->frames++;
    t->bytes += n;
    if (t->out) fwrite(d, 1, n, t->out);
}
static gboolean st_finish(gpointer data) {
    SelfTest *t = data;
    LOGI("selftest: %d frame, %.2f MB in 5 s", t->frames, t->bytes / 1e6);
    t->result = t->frames > 0 ? 0 : 1;
    g_main_loop_quit(t->h->loop);
    return G_SOURCE_REMOVE;
}
static void st_started(Video *v, bool ok, const char *info, void *u) {
    SelfTest *t = u;
    LOGI("selftest: encoder %s -> %s", ok ? "OK" : "FALLITO", info);
    kde_log_outputs(t->h->kde);
    if (!ok) g_main_loop_quit(t->h->loop);
    else g_timeout_add_seconds(5, st_finish, t);
}
static void st_stream(KdeStream *ks, int ev, uint32_t node, const char *err, void *u) {
    SelfTest *t = u;
    if (ev == KDE_STREAM_CREATED) {
        LOGI("selftest: nodo %u", node);
        VideoCallbacks cb = {.on_sample = st_sample, .on_started = st_started, .user = t};
        t->video = video_start(node, 15000, 60, t->h->encoder, &cb);
    } else {
        LOGE("selftest: stream %s %s", ev == KDE_STREAM_FAILED ? "fallito" : "chiuso", err ? err : "");
        g_main_loop_quit(t->h->loop);
    }
}

static int selftest(Host *h) {
    if (!kde_has_screencast(h->kde)) return 1;
    SelfTest t = {.h = h, .result = 1};
    char *dir = g_build_filename(g_get_user_state_dir(), "multiscreen", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "selftest.h264", NULL);
    t.out = fopen(path, "wb");
    LOGI("selftest: salvo il video in %s", path);
    g_free(path);
    g_free(dir);
    KdeStream *ks = g_getenv("MULTISCREEN_TEST_MIRROR")
                        ? kde_stream_existing(h->kde, NULL, 0, st_stream, &t)
                        : kde_stream_virtual(h->kde, MS_OUTPUT_NAME "-test", 1280, 800, 1.0, st_stream, &t);
    g_main_loop_run(h->loop);
    if (t.video) video_stop(t.video);
    if (ks) kde_stream_close(ks);
    if (t.out) fclose(t.out);
    kde_flush(h->kde);
    LOGI("selftest: %s", t.result == 0 ? "RIUSCITO" : "FALLITO");
    return t.result;
}

/* ------------------------------------------------------------------ main */
static gboolean stats_tick(gpointer data) {
    Host *h = data;
    for (GList *l = h->sessions; l; l = l->next) session_log_stats(l->data);
    return G_SOURCE_CONTINUE;
}

static gboolean on_signal(gpointer data) {
    Host *h = data;
    LOGI("uscita richiesta");
    g_main_loop_quit(h->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    Host *h = &host_;
    bool opt_selftest = false, opt_check = false, opt_send = false;
    char *opt_encoder = NULL;
    GOptionEntry entries[] = {
        {"selftest", 0, 0, G_OPTION_ARG_NONE, &opt_selftest, "Prova schermo virtuale ed encoder per 5 s (senza tablet)", NULL},
        {"check", 0, 0, G_OPTION_ARG_NONE, &opt_check, "Controlla i permessi KWin ed esce", NULL},
        {"send", 0, 0, G_OPTION_ARG_NONE, &opt_send, "Invia al tablet i file indicati", NULL},
        {"encoder", 0, 0, G_OPTION_ARG_STRING, &opt_encoder, "Forza encoder: va-lp, va, vaapi, va-sw-upload, x264", "ID"},
        {NULL}};
    GOptionContext *ctx = g_option_context_new("- tablet come secondo schermo via USB");
    g_option_context_add_main_entries(ctx, entries, NULL);
    gst_init(&argc, &argv);
    GError *ge = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &ge)) {
        fprintf(stderr, "%s\n", ge->message);
        return 2;
    }
    if (opt_send) return send_files(argc, argv);

    ms_log_init();
    load_config(h);
    if (opt_encoder) {
        g_free(h->encoder);
        h->encoder = opt_encoder;
    }
    LOGI("MultiScreen host %s avviato (porta %d, encoder %s, %s)", MS_VERSION, h->base_port, h->encoder,
         gst_version_string());
    h->loop = g_main_loop_new(NULL, FALSE);

    if (opt_check || opt_selftest) {
        if (!kde_init(h, false)) return 1;
        if (opt_check) {
            kde_log_outputs(h->kde);
            return kde_has_screencast(h->kde) && kde_has_fake_input(h->kde) ? 0 : 1;
        }
        return selftest(h);
    }

    if (!kde_init(h, false)) g_timeout_add_seconds(2, kde_retry, h);
    g_mutex_init(&h->ports_lock);
    h->ports = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    h->next_port = h->base_port;
    clip_start(on_pc_clip, h);
    ctl_listen(h);

    if (g_getenv("MULTISCREEN_NO_ADB")) {
        /* test senza adb: N tablet "finti" sulle porte base, base+1, ... */
        int n = MAX(1, atoi(g_getenv("MULTISCREEN_NO_ADB")));
        for (int i = 0; i < n; i++) {
            char name[16];
            snprintf(name, sizeof name, "test%d", i + 1);
            if (!session_new(h, name, name, port_for(name, h))) return 1;
        }
    } else {
        AdbCallbacks acb = {.on_ready = on_adb_ready, .on_gone = on_adb_gone, .on_message = on_adb_message,
                            .port_for = port_for, .user = h};
        adb_start(27183, h->apk, h->apk_version, h->token, &acb);
    }

    g_unix_signal_add(SIGTERM, on_signal, h);
    g_unix_signal_add(SIGINT, on_signal, h);
    g_timeout_add_seconds(10, stats_tick, h);
    g_main_loop_run(h->loop);

    while (h->sessions) session_free(h->sessions->data);
    if (h->kde) kde_flush(h->kde);
    adb_stop();
    char *cp = ctl_path();
    unlink(cp);
    g_free(cp);
    LOGI("chiuso");
    return 0;
}
