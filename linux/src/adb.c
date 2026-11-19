/* Gestione del tablet via adb: rileva il collegamento USB, installa/aggiorna l'app,
 * attiva "adb reverse" (il tablet raggiunge il PC sul cavo dati) e avvia l'app. */
#include "adb.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char serial[96];
    char state[32];
    char model[96];
    bool handled;     /* gia' preparato */
    bool ignored;     /* non e' un tablet e non ha l'app */
    bool warned;      /* messaggio "autorizza" gia' mostrato */
    gint64 last_check;
} Dev;

static int port_;
static char *token_;
static char *apk_;
static int apk_ver_;
static AdbCallbacks cb_;
static GThread *thread_;
static volatile gint stop_;
static GHashTable *devs_;
static GAsyncQueue *launch_q_;
static char *adb_bin_;

typedef struct {
    int kind; /* 0 ready, 1 gone, 2 message */
    char a[256];
    char b[512];
} Ev;

static gboolean deliver(gpointer data) {
    Ev *e = data;
    if (e->kind == 0 && cb_.on_ready) cb_.on_ready(e->a, e->b, cb_.user);
    else if (e->kind == 1 && cb_.on_gone) cb_.on_gone(e->a, cb_.user);
    else if (e->kind == 2 && cb_.on_message) cb_.on_message(e->a, e->b, cb_.user);
    g_free(e);
    return G_SOURCE_REMOVE;
}

static void post(int kind, const char *a, const char *b) {
    Ev *e = g_new0(Ev, 1);
    e->kind = kind;
    if (a) g_strlcpy(e->a, a, sizeof e->a);
    if (b) g_strlcpy(e->b, b, sizeof e->b);
    g_idle_add(deliver, e);
}

/* esegue adb con timeout; ritorna stdout (da liberare) o NULL */
static char *adb_run(const char *serial, int timeout_s, int *status, const char *first, ...) {
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("timeout"));
    g_ptr_array_add(argv, g_strdup_printf("%d", timeout_s));
    g_ptr_array_add(argv, g_strdup(adb_bin_));
    if (serial) {
        g_ptr_array_add(argv, g_strdup("-s"));
        g_ptr_array_add(argv, g_strdup(serial));
    }
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) g_ptr_array_add(argv, g_strdup(s));
    va_end(ap);
    g_ptr_array_add(argv, NULL);
    char *out = NULL, *err = NULL;
    int st = -1;
    GError *ge = NULL;
    if (!g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &st, &ge)) {
        LOGW("adb: %s", ge->message);
        g_clear_error(&ge);
        out = NULL;
    } else if (err && *err && out) {
        char *both = g_strconcat(out, err, NULL);
        g_free(out);
        out = both;
    }
    g_free(err);
    g_ptr_array_free(argv, TRUE);
    if (status) *status = g_spawn_check_wait_status(st, NULL) ? 0 : 1;
    return out;
}

static int installed_version(const char *serial) {
    char *o = adb_run(serial, 10, NULL, "shell", "dumpsys", "package", MS_PACKAGE, NULL);
    int v = -1;
    if (o) {
        char *p = strstr(o, "versionCode=");
        if (p) v = atoi(p + 12);
        g_free(o);
    }
    return v;
}

static bool is_tablet(const char *serial) {
    char *o = adb_run(serial, 5, NULL, "shell", "getprop", "ro.build.characteristics", NULL);
    bool t = o && strstr(o, "tablet");
    g_free(o);
    return t;
}

static bool install_apk(Dev *d) {
    if (!apk_ || !g_file_test(apk_, G_FILE_TEST_EXISTS)) {
        LOGE("adb: APK non trovato (%s)", apk_ ? apk_ : "?");
        return false;
    }
    LOGI("adb: installo l'app su %s", d->serial);
    post(2, "MultiScreen", "Installo l'app sul tablet… (se il tablet chiede conferma, tocca Installa)");
    int st;
    char *o = adb_run(d->serial, 120, &st, "install", "-r", "-g", apk_, NULL);
    bool ok = st == 0 && o && strstr(o, "Success");
    if (!ok) {
        LOGW("adb install fallito: %s", o ? o : "");
        /* firma diversa (es. reinstallazione): disinstalla e riprova */
        if (o && strstr(o, "INSTALL_FAILED_UPDATE_INCOMPATIBLE")) {
            g_free(adb_run(d->serial, 30, NULL, "uninstall", MS_PACKAGE, NULL));
            g_free(o);
            o = adb_run(d->serial, 120, &st, "install", "-g", apk_, NULL);
            ok = st == 0 && o && strstr(o, "Success");
        }
    }
    if (!ok) {
        /* es. Xiaomi con "Installa via USB" disattivato: copia l'APK per l'installazione manuale */
        g_free(adb_run(d->serial, 60, NULL, "push", apk_, "/sdcard/Download/MultiScreen.apk", NULL));
        char body[400];
        snprintf(body, sizeof body,
                 "Installazione via USB bloccata dal tablet. Attiva 'Installa tramite USB' nelle Opzioni sviluppatore, "
                 "oppure apri Download/MultiScreen.apk sul tablet.");
        post(2, "MultiScreen: app da installare", body);
        LOGW("adb: %s", o ? o : "");
    }
    g_free(o);
    return ok;
}

static bool ensure_reverse(Dev *d) {
    char dev[32], host[32], both[64];
    snprintf(dev, sizeof dev, "tcp:%d", port_);
    snprintf(host, sizeof host, "tcp:%d", cb_.port_for(d->serial, cb_.user));
    snprintf(both, sizeof both, "%s %s", dev, host);
    char *o = adb_run(d->serial, 5, NULL, "reverse", "--list", NULL);
    bool present = o && strstr(o, both);
    g_free(o);
    if (present) return true;
    int st;
    o = adb_run(d->serial, 5, &st, "reverse", dev, host, NULL);
    g_free(o);
    if (st != 0) LOGW("adb reverse fallito su %s", d->serial);
    else LOGI("adb: reverse %s -> %s attivo su %s", dev, host, d->serial);
    return st == 0;
}

static void launch_app(const char *serial) {
    char *o = adb_run(serial, 10, NULL, "shell", "am", "start", "-n", MS_ACTIVITY, "--es", "token", token_, NULL);
    LOGI("adb: avvio app su %s", serial);
    g_free(o);
}

static void prepare(Dev *d) {
    char *m = adb_run(d->serial, 5, NULL, "shell", "getprop", "ro.product.model", NULL);
    if (m) {
        g_strstrip(m);
        g_strlcpy(d->model, m, sizeof d->model);
        g_free(m);
    }
    int ver = installed_version(d->serial);
    LOGI("adb: dispositivo %s (%s), versione app installata %d, disponibile %d", d->serial, d->model, ver, apk_ver_);
    if (ver < 0) {
        if (!is_tablet(d->serial)) {
            LOGI("adb: %s non e' un tablet e non ha l'app: lo ignoro", d->serial);
            d->ignored = true;
            return;
        }
        if (!install_apk(d)) {
            d->ignored = true; /* riprova al prossimo collegamento */
            return;
        }
    } else if (ver < apk_ver_) {
        install_apk(d);
    }
    if (!ensure_reverse(d)) return;
    d->handled = true;
    launch_app(d->serial);
    post(0, d->serial, d->model);
}

static void poll_once(void) {
    char *o = adb_run(NULL, 8, NULL, "devices", NULL);
    if (!o) return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **lines = g_strsplit(o, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *l = g_strstrip(lines[i]);
        if (!*l || g_str_has_prefix(l, "List of") || g_str_has_prefix(l, "*")) continue;
        char **f = g_strsplit_set(l, " \t", -1);
        char *serial = NULL, *state = NULL;
        for (int j = 0; f[j]; j++) {
            if (!*f[j]) continue;
            if (!serial) serial = f[j];
            else if (!state) state = f[j];
        }
        static const char *known[] = {"device", "unauthorized", "offline", "authorizing", "connecting", "recovery",
                                      "sideload", "bootloader", "rescue", "host", "no", NULL};
        bool ok_state = false;
        for (int q = 0; state && known[q]; q++) ok_state |= strcmp(state, known[q]) == 0;
        if (state && strcmp(state, "no") == 0 && !g_hash_table_contains(devs_, serial)) {
            LOGW("adb: %s senza permessi USB (manca la regola udev: installa android-udev)", serial);
            post(2, "MultiScreen", "Il PC non ha i permessi USB per il tablet: installa 'android-udev' e ricollega il cavo.");
        }
        if (serial && state && ok_state) {
            g_hash_table_add(seen, g_strdup(serial));
            Dev *d = g_hash_table_lookup(devs_, serial);
            if (!d) {
                d = g_new0(Dev, 1);
                g_strlcpy(d->serial, serial, sizeof d->serial);
                g_hash_table_insert(devs_, g_strdup(serial), d);
                LOGI("adb: visto %s (%s)", serial, state);
            }
            if (strcmp(d->state, state) != 0) {
                g_strlcpy(d->state, state, sizeof d->state);
                LOGI("adb: %s -> %s", serial, state);
            }
            if (strcmp(state, "unauthorized") == 0 && !d->warned) {
                d->warned = true;
                post(2, "MultiScreen", "Sul tablet tocca 'Consenti' per il debug USB (spunta 'Consenti sempre').");
            }
            if (strcmp(state, "device") == 0 && !d->handled && !d->ignored) prepare(d);
            else if (d->handled && g_get_monotonic_time() - d->last_check > 5 * G_USEC_PER_SEC) {
                d->last_check = g_get_monotonic_time();
                ensure_reverse(d); /* ripristina se adbd si e' riavviato */
            }
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(o);

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, devs_);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (!g_hash_table_contains(seen, k)) {
            Dev *d = v;
            LOGI("adb: %s scollegato", d->serial);
            if (d->handled) post(1, d->serial, NULL);
            g_hash_table_iter_remove(&it);
        }
    }
    g_hash_table_unref(seen);
}

static gpointer thread_fn(gpointer data) {
    while (!g_atomic_int_get(&stop_) && !g_file_test(adb_bin_, G_FILE_TEST_IS_EXECUTABLE)) {
        g_usleep(3 * G_USEC_PER_SEC); /* adb non installato: aspetta */
        char *p = g_find_program_in_path("adb");
        if (p) {
            g_free(adb_bin_);
            adb_bin_ = p;
        }
    }
    g_free(adb_run(NULL, 15, NULL, "start-server", NULL));
    while (!g_atomic_int_get(&stop_)) {
        char *req;
        while ((req = g_async_queue_try_pop(launch_q_))) {
            Dev *d = g_hash_table_lookup(devs_, req);
            if (d && d->handled) {
                ensure_reverse(d);
                launch_app(req);
            }
            g_free(req);
        }
        poll_once();
        req = g_async_queue_timeout_pop(launch_q_, 1000 * 1000);
        if (req) g_async_queue_push_front(launch_q_, req);
    }
    return NULL;
}

void adb_start(int port, const char *apk_path, int apk_version, const char *token, const AdbCallbacks *cb) {
    port_ = port;
    token_ = g_strdup(token);
    apk_ = g_strdup(apk_path);
    apk_ver_ = apk_version;
    cb_ = *cb;
    devs_ = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    launch_q_ = g_async_queue_new_full(g_free);
    adb_bin_ = g_find_program_in_path("adb");
    if (!adb_bin_) {
        LOGE("adb non trovato: installa 'android-tools'");
        ms_notify("MultiScreen", "adb non trovato: installa il pacchetto android-tools");
        adb_bin_ = g_strdup("adb");
    }
    thread_ = g_thread_new("ms-adb", thread_fn, NULL);
}

void adb_stop(void) {
    g_atomic_int_set(&stop_, 1);
    if (thread_) g_thread_join(thread_);
    thread_ = NULL;
}

void adb_request_launch(const char *serial) {
    g_async_queue_push(launch_q_, g_strdup(serial));
}
