#include "files.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#define CHUNK (64 * 1024)

typedef struct {
    uint32_t id;
    FILE *f;
    char *tmp, *final, *name;
    guint64 size, got;
} Incoming;

struct Files {
    FilesIO io;
    GHashTable *incoming; /* id -> Incoming */
    GAsyncQueue *outq;
    GThread *sender;
};

static char *dldir_;
static volatile gint out_id_ = 1;
static char STOP[] = "";
/* notifiche raggruppate per i file ricevuti */
static int recv_count_;
static char *recv_last_;
static guint recv_timer_;

const char *files_download_dir(void) {
    if (!dldir_) {
        const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        dldir_ = g_build_filename(d ? d : g_get_home_dir(), "MultiScreen", NULL);
    }
    return dldir_;
}

static void incoming_free(gpointer p) {
    Incoming *in = p;
    if (in->f) {
        fclose(in->f);
        g_unlink(in->tmp);
    }
    g_free(in->tmp);
    g_free(in->final);
    g_free(in->name);
    g_free(in);
}

static char *safe_name(const char *n) {
    char *b = g_path_get_basename(n && *n ? n : "file");
    for (char *p = b; *p; p++)
        if (*p == '/' || *p == '\\' || (unsigned char)*p < 32) *p = '_';
    if (strcmp(b, ".") == 0 || strcmp(b, "..") == 0) {
        g_free(b);
        b = g_strdup("file");
    }
    return b;
}

static char *unique_path(const char *dir, const char *name) {
    char *p = g_build_filename(dir, name, NULL);
    if (!g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    g_free(p);
    const char *dot = strrchr(name, '.');
    char *stem = dot && dot != name ? g_strndup(name, dot - name) : g_strdup(name);
    const char *ext = dot && dot != name ? dot : "";
    for (int i = 1;; i++) {
        char *n = g_strdup_printf("%s (%d)%s", stem, i, ext);
        p = g_build_filename(dir, n, NULL);
        g_free(n);
        if (!g_file_test(p, G_FILE_TEST_EXISTS)) break;
        g_free(p);
    }
    g_free(stem);
    return p;
}

static gboolean recv_notify(gpointer d) {
    recv_timer_ = 0;
    char *title = recv_count_ == 1 ? g_strdup_printf("Ricevuto dal tablet: %s", recv_last_)
                                   : g_strdup_printf("Ricevuti %d file dal tablet", recv_count_);
    /* notifica con pulsante per aprire la cartella */
    char *dir = g_shell_quote(files_download_dir());
    char *qt = g_shell_quote(title);
    char *cmd = g_strdup_printf("a=$(notify-send -a MultiScreen -i folder-download -A open='Apri cartella' %s %s) && "
                                "[ \"$a\" = open ] && xdg-open %s", qt, dir, dir);
    const char *argv[] = {"sh", "-c", cmd, NULL};
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    g_free(cmd);
    g_free(qt);
    g_free(dir);
    g_free(title);
    recv_count_ = 0;
    return G_SOURCE_REMOVE;
}

void files_on_begin(Files *fs, const char *json) {
    uint32_t id = (uint32_t)json_num(json, "id", 0);
    char name[512] = "file";
    json_str(json, "name", name, sizeof name);
    Incoming *in = g_new0(Incoming, 1);
    in->id = id;
    in->size = (guint64)json_num(json, "size", 0);
    in->name = safe_name(name);
    g_mkdir_with_parents(files_download_dir(), 0755);
    in->final = unique_path(files_download_dir(), in->name);
    in->tmp = g_strconcat(in->final, ".part", NULL);
    in->f = fopen(in->tmp, "wb");
    if (!in->f) LOGE("file: impossibile scrivere %s: %s", in->tmp, g_strerror(errno));
    LOGI("file: ricevo '%s' (%llu byte)", in->name, (unsigned long long)in->size);
    g_hash_table_replace(fs->incoming, GUINT_TO_POINTER(id), in);
}

static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

void files_on_data(Files *fs, const uint8_t *d, uint32_t len) {
    if (len < 4) return;
    Incoming *in = g_hash_table_lookup(fs->incoming, GUINT_TO_POINTER(rd32(d)));
    if (!in || !in->f) return;
    if (fwrite(d + 4, 1, len - 4, in->f) != len - 4) {
        LOGE("file: errore di scrittura su %s", in->tmp);
        fclose(in->f);
        in->f = NULL;
        g_unlink(in->tmp);
    }
    in->got += len - 4;
}

void files_on_end(Files *fs, const uint8_t *d, uint32_t len) {
    if (len < 5) return;
    uint32_t id = rd32(d);
    bool ok = d[4] != 0;
    Incoming *in = g_hash_table_lookup(fs->incoming, GUINT_TO_POINTER(id));
    if (!in) return;
    bool saved = false;
    if (in->f) {
        saved = fclose(in->f) == 0 && ok;
        in->f = NULL;
        if (saved && g_rename(in->tmp, in->final) == 0) {
            LOGI("file: salvato %s", in->final);
        } else {
            saved = false;
            g_unlink(in->tmp);
            LOGW("file: '%s' non completato", in->name);
        }
    }
    char *base = g_path_get_basename(in->final);
    char *esc = g_strescape(base, NULL);
    const char *home = g_get_home_dir();
    const char *dl = files_download_dir();
    const char *where = g_str_has_prefix(dl, home) ? dl + strlen(home) + (dl[strlen(home)] == '/') : dl;
    char *wesc = g_strescape(where, NULL);
    char *j = g_strdup_printf("{\"id\":%u,\"name\":\"%s\",\"ok\":%s,\"where\":\"%s\"}", id, esc,
                              saved ? "true" : "false", wesc);
    g_free(wesc);
    fs->io.send(P_FILE_RESULT_OUT, j, strlen(j), NULL, 0, fs->io.user);
    g_free(j);
    g_free(esc);
    if (saved) {
        recv_count_++;
        g_free(recv_last_);
        recv_last_ = base;
        base = NULL;
        if (recv_timer_) g_source_remove(recv_timer_);
        recv_timer_ = g_timeout_add(1200, recv_notify, NULL);
    }
    g_free(base);
    g_hash_table_remove(fs->incoming, GUINT_TO_POINTER(id));
}

void files_on_result(Files *fs, const char *json) {
    char name[512] = "";
    json_str(json, "name", name, sizeof name);
    bool ok = json_bool(json, "ok", false);
    LOGI("file: il tablet ha %s '%s'", ok ? "salvato" : "NON salvato", name);
    char *t = g_strdup_printf(ok ? "Inviato al tablet: %s" : "Invio non riuscito: %s", name);
    ms_notify(t, ok ? "Sul tablet: Download/MultiScreen" : "Riprova con il tablet collegato");
    g_free(t);
}

void files_client_gone(Files *fs) {
    g_hash_table_remove_all(fs->incoming); /* elimina i .part incompleti */
}

static const char *guess_mime(const char *path) {
    char *ct = g_content_type_guess(path, NULL, 0, NULL);
    char *m = ct ? g_content_type_get_mime_type(ct) : NULL;
    g_free(ct);
    static char buf[128];
    g_strlcpy(buf, m ? m : "application/octet-stream", sizeof buf);
    g_free(m);
    return buf;
}

static gpointer sender_fn(gpointer data) {
    Files *fs = data;
    guint8 *buf = g_malloc(CHUNK + 4);
    for (;;) {
        char *path = g_async_queue_pop(fs->outq);
        if (path == STOP) break;
        FILE *f = fopen(path, "rb");
        struct stat st;
        if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
            LOGW("file: non leggibile %s", path);
            if (f) fclose(f);
            g_free(path);
            continue;
        }
        uint32_t id = (uint32_t)g_atomic_int_add(&out_id_, 1);
        char *base = g_path_get_basename(path);
        char *esc = g_strescape(base, NULL);
        char *j = g_strdup_printf("{\"id\":%u,\"name\":\"%s\",\"size\":%lld,\"mime\":\"%s\"}", id, esc,
                                  (long long)st.st_size, guess_mime(path));
        LOGI("file: invio '%s' al tablet (%lld byte)", base, (long long)st.st_size);
        bool ok = fs->io.send(P_FILE_BEGIN_OUT, j, strlen(j), NULL, 0, fs->io.user);
        g_free(j);
        g_free(esc);
        uint8_t idb[4] = {id >> 24, id >> 16, id >> 8, id};
        while (ok) {
            size_t n = fread(buf, 1, CHUNK, f);
            if (n == 0) break;
            fs->io.throttle(fs->io.user);
            ok = fs->io.send(P_FILE_DATA_OUT, idb, 4, buf, n, fs->io.user);
        }
        bool complete = ok && !ferror(f);
        uint8_t end[5] = {idb[0], idb[1], idb[2], idb[3], complete ? 1 : 0};
        if (ok) fs->io.send(P_FILE_END_OUT, end, 5, NULL, 0, fs->io.user);
        if (!ok) {
            char *t = g_strdup_printf("Invio non riuscito: %s", base);
            ms_notify(t, "Il tablet si e' scollegato");
            g_free(t);
        }
        fclose(f);
        g_free(base);
        g_free(path);
    }
    g_free(buf);
    return NULL;
}

Files *files_new(const FilesIO *io) {
    Files *fs = g_new0(Files, 1);
    fs->io = *io;
    fs->incoming = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, incoming_free);
    fs->outq = g_async_queue_new();
    fs->sender = g_thread_new("ms-files", sender_fn, fs);
    return fs;
}

void files_free(Files *fs) {
    if (!fs) return;
    char *p;
    while ((p = g_async_queue_try_pop(fs->outq))) g_free(p); /* annulla quelli in coda */
    g_async_queue_push(fs->outq, STOP);
    g_thread_join(fs->sender);
    g_async_queue_unref(fs->outq);
    g_hash_table_destroy(fs->incoming);
    g_free(fs);
}

char *files_send_path(Files *fs, const char *path) {
    if (!fs->io.connected(fs->io.user)) return g_strdup("tablet non collegato");
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) return g_strdup_printf("non e' un file: %s", path);
    g_async_queue_push(fs->outq, g_strdup(path));
    return NULL;
}
