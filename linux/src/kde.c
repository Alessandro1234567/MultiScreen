/* Connessione a KWin (Plasma 6, Wayland): schermo virtuale via zkde_screencast e input via org_kde_kwin_fake_input.
 * KWin concede questi protocolli solo ai programmi elencati in un file .desktop con
 * X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input  (lo installa install.sh). */
#include "kde.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "zkde-screencast-unstable-v1-client-protocol.h"
#include "fake-input-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

typedef struct Output {
    struct wl_list link;
    Kde *kde;
    uint32_t global;
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    char name[96];
    int x, y, w, h;
    bool has_logical;
} Output;

struct Kde {
    struct wl_display *display;
    struct wl_registry *registry;
    struct zkde_screencast_unstable_v1 *screencast;
    uint32_t screencast_ver;
    struct org_kde_kwin_fake_input *fake;
    uint32_t fake_ver;
    struct zxdg_output_manager_v1 *xdg_mgr;
    struct wl_list outputs;
};

struct KdeStream {
    Kde *kde;
    struct zkde_screencast_stream_unstable_v1 *stream;
    KdeStreamCb cb;
    void *user;
};

/* ------------------------------------------------------------------ outputs */
static void xo_pos(void *d, struct zxdg_output_v1 *o, int32_t x, int32_t y) {
    Output *out = d;
    out->x = x;
    out->y = y;
    out->has_logical = true;
}
static void xo_size(void *d, struct zxdg_output_v1 *o, int32_t w, int32_t h) {
    Output *out = d;
    out->w = w;
    out->h = h;
    out->has_logical = true;
}
static void xo_done(void *d, struct zxdg_output_v1 *o) {}
static void xo_name(void *d, struct zxdg_output_v1 *o, const char *name) {
    Output *out = d;
    g_strlcpy(out->name, name, sizeof out->name);
}
static void xo_desc(void *d, struct zxdg_output_v1 *o, const char *desc) {}
static const struct zxdg_output_v1_listener xo_listener = {
    .logical_position = xo_pos, .logical_size = xo_size, .done = xo_done, .name = xo_name, .description = xo_desc,
};

static void wo_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
                        const char *make, const char *model, int32_t tr) {
    Output *out = d;
    if (!out->has_logical) {
        out->x = x;
        out->y = y;
    }
}
static void wo_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {}
static void wo_done(void *d, struct wl_output *o) {}
static void wo_scale(void *d, struct wl_output *o, int32_t s) {}
static void wo_name(void *d, struct wl_output *o, const char *name) {
    Output *out = d;
    g_strlcpy(out->name, name, sizeof out->name);
}
static void wo_desc(void *d, struct wl_output *o, const char *desc) {}
static const struct wl_output_listener wo_listener = {
    .geometry = wo_geometry, .mode = wo_mode, .done = wo_done, .scale = wo_scale, .name = wo_name, .description = wo_desc,
};

static void output_attach_xdg(Kde *k, Output *o) {
    if (o->xdg || !k->xdg_mgr) return;
    o->xdg = zxdg_output_manager_v1_get_xdg_output(k->xdg_mgr, o->wl);
    zxdg_output_v1_add_listener(o->xdg, &xo_listener, o);
}

/* ------------------------------------------------------------------ registry */
static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    Kde *k = data;
    if (strcmp(iface, zkde_screencast_unstable_v1_interface.name) == 0) {
        /* versione 3: ha stream_virtual_output e gli eventi che gestiamo (le versioni nuove aggiungono eventi) */
        k->screencast_ver = MIN(ver, MIN(3u, (uint32_t)zkde_screencast_unstable_v1_interface.version));
        k->screencast = wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, k->screencast_ver);
    } else if (strcmp(iface, org_kde_kwin_fake_input_interface.name) == 0) {
        k->fake_ver = MIN(ver, (uint32_t)org_kde_kwin_fake_input_interface.version);
        k->fake = wl_registry_bind(reg, name, &org_kde_kwin_fake_input_interface, k->fake_ver);
        org_kde_kwin_fake_input_authenticate(k->fake, "MultiScreen", "Input touch e penna dal tablet");
    } else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
        k->xdg_mgr = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, MIN(ver, 3u));
        Output *o;
        wl_list_for_each(o, &k->outputs, link) output_attach_xdg(k, o);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        Output *o = g_new0(Output, 1);
        o->kde = k;
        o->global = name;
        o->wl = wl_registry_bind(reg, name, &wl_output_interface, MIN(ver, 4u));
        wl_output_add_listener(o->wl, &wo_listener, o);
        wl_list_insert(&k->outputs, &o->link);
        output_attach_xdg(k, o);
    }
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
    Kde *k = data;
    Output *o, *tmp;
    wl_list_for_each_safe(o, tmp, &k->outputs, link) {
        if (o->global == name) {
            wl_list_remove(&o->link);
            if (o->xdg) zxdg_output_v1_destroy(o->xdg);
            if (wl_output_get_version(o->wl) >= 3) wl_output_release(o->wl); else wl_output_destroy(o->wl);
            g_free(o);
        }
    }
}

static const struct wl_registry_listener reg_listener = {.global = reg_global, .global_remove = reg_remove};

Kde *kde_connect(char **err) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d && !g_getenv("WAYLAND_DISPLAY")) d = wl_display_connect("wayland-0");
    if (!d) {
        *err = g_strdup_printf("impossibile collegarsi a Wayland (WAYLAND_DISPLAY=%s): %s",
                               g_getenv("WAYLAND_DISPLAY") ? g_getenv("WAYLAND_DISPLAY") : "(vuoto)", g_strerror(errno));
        return NULL;
    }
    Kde *k = g_new0(Kde, 1);
    k->display = d;
    wl_list_init(&k->outputs);
    k->registry = wl_display_get_registry(d);
    wl_registry_add_listener(k->registry, &reg_listener, k);
    wl_display_roundtrip(d); /* globali */
    wl_display_roundtrip(d); /* eventi delle uscite */
    return k;
}

void kde_free(Kde *k) {
    if (!k) return;
    wl_display_disconnect(k->display);
    g_free(k);
}

int kde_fd(Kde *k) { return wl_display_get_fd(k->display); }

int kde_dispatch(Kde *k) {
    if (wl_display_dispatch(k->display) < 0) return -1;
    return 0;
}

void kde_flush(Kde *k) {
    wl_display_flush(k->display);
}

bool kde_has_screencast(Kde *k) { return k->screencast && k->screencast_ver >= 2; }
bool kde_has_fake_input(Kde *k) { return k->fake != NULL; }
uint32_t kde_screencast_version(Kde *k) { return k->screencast_ver; }

/* ------------------------------------------------------------------ screencast */
static void st_closed(void *d, struct zkde_screencast_stream_unstable_v1 *s) {
    KdeStream *ks = d;
    if (ks->cb) ks->cb(ks, KDE_STREAM_CLOSED, 0, NULL, ks->user);
}
static void st_created(void *d, struct zkde_screencast_stream_unstable_v1 *s, uint32_t node) {
    KdeStream *ks = d;
    if (ks->cb) ks->cb(ks, KDE_STREAM_CREATED, node, NULL, ks->user);
}
static void st_failed(void *d, struct zkde_screencast_stream_unstable_v1 *s, const char *error) {
    KdeStream *ks = d;
    if (ks->cb) ks->cb(ks, KDE_STREAM_FAILED, 0, error, ks->user);
}
static const struct zkde_screencast_stream_unstable_v1_listener st_listener = {
    .closed = st_closed, .created = st_created, .failed = st_failed,
};

KdeStream *kde_stream_virtual(Kde *k, const char *name, int lw, int lh, double scale, KdeStreamCb cb, void *user) {
    if (!kde_has_screencast(k)) return NULL;
    KdeStream *ks = g_new0(KdeStream, 1);
    ks->kde = k;
    ks->cb = cb;
    ks->user = user;
    ks->stream = zkde_screencast_unstable_v1_stream_virtual_output(
        k->screencast, name, lw, lh, wl_fixed_from_double(scale), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    zkde_screencast_stream_unstable_v1_add_listener(ks->stream, &st_listener, ks);
    wl_display_flush(k->display);
    return ks;
}

KdeStream *kde_stream_existing(Kde *k, char *name_out, size_t name_len, KdeStreamCb cb, void *user) {
    if (!k->screencast || wl_list_empty(&k->outputs)) return NULL;
    Output *o = NULL, *it;
    wl_list_for_each(it, &k->outputs, link) {
        if (strstr(it->name, MS_OUTPUT_NAME)) continue; /* non duplicare un altro tablet */
        if (!o || (it->x == 0 && it->y == 0)) o = it;
    }
    if (!o) return NULL;
    if (name_out) g_strlcpy(name_out, o->name, name_len);
    KdeStream *ks = g_new0(KdeStream, 1);
    ks->kde = k;
    ks->cb = cb;
    ks->user = user;
    ks->stream = zkde_screencast_unstable_v1_stream_output(k->screencast, o->wl, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    zkde_screencast_stream_unstable_v1_add_listener(ks->stream, &st_listener, ks);
    wl_display_flush(k->display);
    return ks;
}

void kde_stream_close(KdeStream *ks) {
    if (!ks) return;
    zkde_screencast_stream_unstable_v1_close(ks->stream);
    wl_display_flush(ks->kde->display);
    g_free(ks);
}

static bool name_matches(const char *have, const char *want) {
    if (!*want) return true;
    return strcmp(have, want) == 0 || (g_str_has_prefix(have, "Virtual-") && strcmp(have + 8, want) == 0);
}

bool kde_output_geometry(Kde *k, const char *name, int *x, int *y, int *w, int *h) {
    Output *o;
    wl_list_for_each(o, &k->outputs, link) {
        if (o->has_logical && o->w > 0 && name_matches(o->name, name)) {
            *x = o->x;
            *y = o->y;
            *w = o->w;
            *h = o->h;
            return true;
        }
    }
    return false;
}

int kde_list_outputs(Kde *k, KdeOut *out, int max) {
    int n = 0;
    Output *o;
    wl_list_for_each(o, &k->outputs, link) {
        if (n >= max || !o->has_logical || o->w <= 0) continue;
        g_strlcpy(out[n].name, o->name, sizeof out[n].name);
        out[n].x = o->x;
        out[n].y = o->y;
        out[n].w = o->w;
        out[n].h = o->h;
        n++;
    }
    return n;
}

void kde_log_outputs(Kde *k) {
    Output *o;
    wl_list_for_each(o, &k->outputs, link) {
        LOGI("  uscita '%s' pos %d,%d dimensione logica %dx%d", o->name, o->x, o->y, o->w, o->h);
    }
}

/* ------------------------------------------------------------------ input */
#define FX(v) wl_fixed_from_double(v)
void kde_touch_down(Kde *k, uint32_t id, double x, double y) {
    if (k->fake && k->fake_ver >= 2) org_kde_kwin_fake_input_touch_down(k->fake, id, FX(x), FX(y));
}
void kde_touch_motion(Kde *k, uint32_t id, double x, double y) {
    if (k->fake && k->fake_ver >= 2) org_kde_kwin_fake_input_touch_motion(k->fake, id, FX(x), FX(y));
}
void kde_touch_up(Kde *k, uint32_t id) {
    if (k->fake && k->fake_ver >= 2) org_kde_kwin_fake_input_touch_up(k->fake, id);
}
void kde_touch_cancel(Kde *k) {
    if (k->fake && k->fake_ver >= 2) org_kde_kwin_fake_input_touch_cancel(k->fake);
}
void kde_touch_frame(Kde *k) {
    if (k->fake && k->fake_ver >= 2) org_kde_kwin_fake_input_touch_frame(k->fake);
}
void kde_pointer_abs(Kde *k, double x, double y) {
    if (k->fake && k->fake_ver >= 3) org_kde_kwin_fake_input_pointer_motion_absolute(k->fake, FX(x), FX(y));
}
void kde_button(Kde *k, uint32_t button, bool pressed) {
    if (k->fake) org_kde_kwin_fake_input_button(k->fake, button, pressed ? 1 : 0);
}
void kde_axis(Kde *k, uint32_t axis, double value) {
    if (k->fake) org_kde_kwin_fake_input_axis(k->fake, axis, FX(value));
}
