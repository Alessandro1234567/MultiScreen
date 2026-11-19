#include "clip.h"
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>

static ClipCb cb_;
static void *user_;
static GString *acc_;
static bool avail_;
static int restarts_;

static void spawn_watch(void);

static gboolean respawn(gpointer d) {
    spawn_watch();
    return G_SOURCE_REMOVE;
}

static gboolean on_out(gint fd, GIOCondition c, gpointer d) {
    char buf[16384];
    ssize_t r = read(fd, buf, sizeof buf);
    if (r <= 0) {
        close(fd);
        if (restarts_++ < 20) {
            LOGW("appunti: wl-paste terminato, lo riavvio");
            g_timeout_add_seconds(5, respawn, NULL);
        } else {
            LOGW("appunti: wl-paste continua a terminare, condivisione appunti disattivata");
            avail_ = false;
        }
        return G_SOURCE_REMOVE;
    }
    g_string_append_len(acc_, buf, r);
    for (;;) {
        char *z = memchr(acc_->str, 0, acc_->len);
        if (!z) break;
        gsize n = z - acc_->str;
        if (n > 0 && cb_) cb_(acc_->str, n, user_);
        g_string_erase(acc_, 0, n + 1);
    }
    if (acc_->len > 8 * 1024 * 1024) g_string_truncate(acc_, 0);
    return G_SOURCE_CONTINUE;
}

static void spawn_watch(void) {
    /* ad ogni cambio degli appunti wl-paste esegue il comando con il contenuto su stdin */
    const char *argv[] = {"wl-paste", "--type", "text", "--watch", "sh", "-c", "cat; printf '\\0'", NULL};
    int out = -1;
    GError *e = NULL;
    if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                                  NULL, NULL, &out, NULL, &e)) {
        LOGW("appunti: wl-paste non disponibile (installa wl-clipboard): %s", e->message);
        g_clear_error(&e);
        avail_ = false;
        return;
    }
    avail_ = true;
    g_unix_fd_add(out, G_IO_IN | G_IO_HUP | G_IO_ERR, on_out, NULL);
}

void clip_start(ClipCb cb, void *user) {
    cb_ = cb;
    user_ = user;
    acc_ = g_string_new(NULL);
    spawn_watch();
}

bool clip_available(void) { return avail_; }

void clip_set(const char *text, gsize len) {
    const char *argv[] = {"wl-copy", "--type", "text/plain;charset=utf-8", NULL};
    int in = -1;
    GError *e = NULL;
    if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, &in, NULL, NULL, &e)) {
        LOGW("appunti: wl-copy: %s", e->message);
        g_clear_error(&e);
        return;
    }
    gsize off = 0;
    while (off < len) {
        ssize_t w = write(in, text + off, len - off);
        if (w <= 0) break;
        off += w;
    }
    close(in);
}
