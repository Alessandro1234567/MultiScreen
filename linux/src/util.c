#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static FILE *logf_;
static GMutex log_lock;

void ms_log_init(void) {
    const char *env = g_getenv("MULTISCREEN_LOG");
    char *path;
    if (env && *env) {
        path = g_strdup(env);
    } else {
        char *dir = g_build_filename(g_get_user_state_dir(), "multiscreen", NULL);
        g_mkdir_with_parents(dir, 0700);
        path = g_build_filename(dir, "host.log", NULL);
        char *old = g_strconcat(path, ".1", NULL);
        rename(path, old);
        g_free(old);
        g_free(dir);
    }
    logf_ = fopen(path, "a");
    g_free(path);
}

void ms_log(const char *level, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tm);
    g_mutex_lock(&log_lock);
    fprintf(stderr, "%s.%03ld %s %s\n", tbuf, ts.tv_nsec / 1000000, level, msg);
    if (logf_) {
        fprintf(logf_, "%s.%03ld %s %s\n", tbuf, ts.tv_nsec / 1000000, level, msg);
        fflush(logf_);
    }
    g_mutex_unlock(&log_lock);
}

void ms_notify(const char *title, const char *body) {
    const char *argv[] = {"notify-send", "-a", "MultiScreen", "-i", "video-display", title, body, NULL};
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static const char *json_find(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

double json_num(const char *json, const char *key, double def) {
    const char *p = json_find(json, key);
    if (!p) return def;
    if (*p == '"') p++; /* numeri dentro stringhe */
    char *end;
    double v = g_ascii_strtod(p, &end);
    return end == p ? def : v;
}

bool json_str(const char *json, const char *key, char *out, size_t outlen) {
    const char *p = json_find(json, key);
    if (!p || *p != '"' || outlen == 0) return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outlen) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return true;
}

bool json_bool(const char *json, const char *key, bool def) {
    const char *p = json_find(json, key);
    if (!p) return def;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}
