/* Cattura PipeWire -> encoder H.264 hardware (VA-API) -> appsink, con GStreamer.
 * Prova una lista di pipeline in ordine (hardware a bassa latenza prima, software per ultimo). */
#include "video.h"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    const char *id;      /* nome per log/config */
    const char *element; /* elemento encoder richiesto */
    const char *convert; /* conversione prima dell'encoder */
    const char *props;   /* proprieta' "nome=valore" separate da spazio, %B bitrate kbps, %K key-int */
} Candidate;

static const Candidate CANDIDATES[] = {
    {"va-lp", "vah264lpenc", "vapostproc ! video/x-raw(memory:VAMemory),format=NV12",
     "rate-control=cbr bitrate=%B key-int-max=%K b-frames=0 ref-frames=1 target-usage=7"},
    {"va", "vah264enc", "vapostproc ! video/x-raw(memory:VAMemory),format=NV12",
     "rate-control=cbr bitrate=%B key-int-max=%K b-frames=0 ref-frames=1 target-usage=7"},
    {"vaapi", "vaapih264enc", "vaapipostproc ! video/x-raw(memory:VASurface),format=NV12",
     "rate-control=cbr bitrate=%B keyframe-period=%K max-bframes=0 tune=low-power"},
    {"va-sw-upload", "vah264enc", "videoconvert ! video/x-raw,format=NV12",
     "rate-control=cbr bitrate=%B key-int-max=%K b-frames=0 ref-frames=1 target-usage=7"},
    {"x264", "x264enc", "videoconvert ! video/x-raw,format=I420",
     "tune=zerolatency speed-preset=ultrafast bitrate=%B key-int-max=%K bframes=0 threads=4 sliced-threads=true"},
};
#define N_CAND (int)(sizeof CANDIDATES / sizeof CANDIDATES[0])

struct Video {
    uint32_t node;
    int bitrate, fps;
    char *pref;
    VideoCallbacks cb;
    GstElement *pipeline;
    GstElement *sink;
    GstElement *encoder;
    char enc_name[32];
    GThread *thread;
    guint bus_watch;
    volatile gint stopping;
    volatile gint got_sample;
    gint64 last_pass_us;  /* limitatore fps */
    gint64 min_interval_us;
    GMutex lock;
    volatile gint refs;
};

/* encoder che hanno dato errore prima di produrre frame: non riprovarli in questa esecuzione */
static GHashTable *bad_encoders;
static GMutex bad_lock;
static void mark_bad(const char *id) {
    g_mutex_lock(&bad_lock);
    if (!bad_encoders) bad_encoders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(bad_encoders, g_strdup(id));
    g_mutex_unlock(&bad_lock);
    char *cp = g_build_filename(g_get_user_cache_dir(), "multiscreen", "encoder", NULL);
    g_unlink(cp);
    g_free(cp);
}
static bool is_bad(const char *id) {
    g_mutex_lock(&bad_lock);
    bool b = bad_encoders && g_hash_table_contains(bad_encoders, id);
    g_mutex_unlock(&bad_lock);
    return b;
}

static void video_unref(Video *v) {
    if (g_atomic_int_dec_and_test(&v->refs)) {
        g_free(v->pref);
        g_mutex_clear(&v->lock);
        g_free(v);
    }
}

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data) {
    Video *v = data;
    GstSample *s = gst_app_sink_pull_sample(sink);
    if (!s) return GST_FLOW_OK;
    GstBuffer *b = gst_sample_get_buffer(s);
    GstCaps *caps = gst_sample_get_caps(s);
    int w = 0, h = 0;
    if (caps) {
        GstStructure *st = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(st, "width", &w);
        gst_structure_get_int(st, "height", &h);
    }
    GstMapInfo m;
    if (b && gst_buffer_map(b, &m, GST_MAP_READ)) {
        bool key = !GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
        guint64 pts = GST_BUFFER_PTS_IS_VALID(b) ? GST_BUFFER_PTS(b) / 1000 : (guint64)g_get_monotonic_time();
        g_atomic_int_set(&v->got_sample, 1);
        if (!g_atomic_int_get(&v->stopping) && v->cb.on_sample) v->cb.on_sample(m.data, m.size, pts, key, w, h, v->cb.user);
        gst_buffer_unmap(b, &m);
    }
    gst_sample_unref(s);
    return GST_FLOW_OK;
}

/* limita gli fps scartando frame grezzi prima dell'encoder (nessuna latenza aggiunta) */
static GstPadProbeReturn rate_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    Video *v = data;
    if (v->min_interval_us <= 0) return GST_PAD_PROBE_OK;
    gint64 now = g_get_monotonic_time();
    if (now - v->last_pass_us < v->min_interval_us) return GST_PAD_PROBE_DROP;
    v->last_pass_us = now;
    return GST_PAD_PROBE_OK;
}

static void set_props(GstElement *e, const char *props, int bitrate, int keyint) {
    char **kv = g_strsplit(props, " ", -1);
    for (int i = 0; kv[i]; i++) {
        char *eq = strchr(kv[i], '=');
        if (!eq) continue;
        *eq = 0;
        const char *name = kv[i];
        char val[64];
        if (strcmp(eq + 1, "%B") == 0) snprintf(val, sizeof val, "%d", bitrate);
        else if (strcmp(eq + 1, "%K") == 0) snprintf(val, sizeof val, "%d", keyint);
        else g_strlcpy(val, eq + 1, sizeof val);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), name)) gst_util_set_object_arg(G_OBJECT(e), name, val);
        else LOGW("encoder %s: proprieta' '%s' non esiste, ignorata", GST_OBJECT_NAME(e), name);
    }
    g_strfreev(kv);
}

static GstElement *build(Video *v, const Candidate *c, char **err) {
    GstElementFactory *f = gst_element_factory_find(c->element);
    if (!f) {
        *err = g_strdup_printf("elemento %s non installato", c->element);
        return NULL;
    }
    gst_object_unref(f);
    int keepalive = v->fps < 60 ? 100 : 1000;
    const char *test_src = g_getenv("MULTISCREEN_TEST_SOURCE");
    char *srcdesc = test_src ? g_strdup(test_src) /* deve contenere name=src */
                             : g_strdup_printf("pipewiresrc name=src always-copy=false do-timestamp=true keepalive-time=%d", keepalive);
    char *desc = g_strdup_printf(
        "%s ! "
        "queue name=q max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
        "%s ! %s name=enc ! h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! "
        "appsink name=sink sync=false async=false max-buffers=4 drop=false enable-last-sample=false emit-signals=true",
        srcdesc, c->convert, c->element);
    g_free(srcdesc);
    GError *ge = NULL;
    GstElement *p = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &ge);
    g_free(desc);
    if (!p) {
        *err = g_strdup_printf("parse: %s", ge ? ge->message : "?");
        g_clear_error(&ge);
        return NULL;
    }
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src");
    char node[16];
    snprintf(node, sizeof node, "%u", v->node);
    if (test_src) {
    } else if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "path")) g_object_set(src, "path", node, NULL);
    else g_object_set(src, "target-object", node, NULL);
    gst_object_unref(src);

    GstElement *enc = gst_bin_get_by_name(GST_BIN(p), "enc");
    set_props(enc, c->props, v->bitrate, MAX(v->fps, 15) * 5);
    v->encoder = enc; /* ref tenuta */

    GstElement *q = gst_bin_get_by_name(GST_BIN(p), "q");
    GstPad *qsrc = gst_element_get_static_pad(q, "src");
    gst_pad_add_probe(qsrc, GST_PAD_PROBE_TYPE_BUFFER, rate_probe, v, NULL);
    gst_object_unref(qsrc);
    gst_object_unref(q);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), v);
    v->sink = sink;
    return p;
}

static void log_caps(GstElement *p, const char *name, const char *pad) {
    GstElement *e = gst_bin_get_by_name(GST_BIN(p), name);
    if (!e) return;
    GstPad *pd = gst_element_get_static_pad(e, pad);
    GstCaps *c = pd ? gst_pad_get_current_caps(pd) : NULL;
    if (c) {
        char *s = gst_caps_to_string(c);
        LOGI("  caps %s.%s: %s", name, pad, s);
        g_free(s);
        gst_caps_unref(c);
    }
    if (pd) gst_object_unref(pd);
    gst_object_unref(e);
}

/* aspetta il primo frame o un errore */
static bool src_negotiated(GstElement *p) {
    GstElement *e = gst_bin_get_by_name(GST_BIN(p), "src");
    GstPad *pd = gst_element_get_static_pad(e, "src");
    GstCaps *c = gst_pad_get_current_caps(pd);
    bool ok = c != NULL;
    if (c) gst_caps_unref(c);
    gst_object_unref(pd);
    gst_object_unref(e);
    return ok;
}

static bool wait_start(Video *v, GstElement *p, char **err) {
    GstBus *bus = gst_element_get_bus(p);
    gint64 start = g_get_monotonic_time();
    gint64 deadline = start + 6 * G_USEC_PER_SEC;
    bool ok = false;
    while (!g_atomic_int_get(&v->stopping)) {
        if (g_atomic_int_get(&v->got_sample)) {
            ok = true;
            break;
        }
        gint64 now = g_get_monotonic_time();
        if (now - start > 3 * G_USEC_PER_SEC && src_negotiated(p)) {
            /* schermo fermo: KWin manda frame solo quando cambia qualcosa */
            LOGI("video: cattura avviata, nessun frame ancora (schermo fermo)");
            ok = true;
            break;
        }
        if (now > deadline) {
            *err = g_strdup("nessun frame entro 6 secondi");
            break;
        }
        GstMessage *m = gst_bus_timed_pop_filtered(bus, 50 * GST_MSECOND, GST_MESSAGE_ERROR);
        if (m) {
            GError *ge = NULL;
            char *dbg = NULL;
            gst_message_parse_error(m, &ge, &dbg);
            *err = g_strdup_printf("%s (%s)", ge ? ge->message : "?", dbg ? dbg : "");
            g_clear_error(&ge);
            g_free(dbg);
            gst_message_unref(m);
            break;
        }
    }
    gst_object_unref(bus);
    return ok;
}

typedef struct {
    Video *v;
    bool ok;
    char *info;
} StartResult;

static gboolean bus_cb(GstBus *bus, GstMessage *m, gpointer data) {
    Video *v = data;
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
        GError *ge = NULL;
        char *dbg = NULL;
        gst_message_parse_error(m, &ge, &dbg);
        char *msg = g_strdup_printf("%s (%s)", ge ? ge->message : "?", dbg ? dbg : "");
        LOGE("video: %s", msg);
        if (!g_atomic_int_get(&v->got_sample) && v->enc_name[0]) {
            LOGW("video: l'encoder '%s' ha fallito al primo frame, la prossima volta ne provo un altro", v->enc_name);
            mark_bad(v->enc_name);
        }
        if (!g_atomic_int_get(&v->stopping) && v->cb.on_error) v->cb.on_error(v, msg, v->cb.user);
        g_free(msg);
        g_clear_error(&ge);
        g_free(dbg);
    } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_WARNING) {
        GError *ge = NULL;
        gst_message_parse_warning(m, &ge, NULL);
        LOGW("video: %s", ge ? ge->message : "?");
        g_clear_error(&ge);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean deliver_start(gpointer data) {
    StartResult *r = data;
    Video *v = r->v;
    if (!g_atomic_int_get(&v->stopping)) {
        if (r->ok && v->pipeline) {
            GstBus *bus = gst_element_get_bus(v->pipeline);
            v->bus_watch = gst_bus_add_watch(bus, bus_cb, v);
            gst_object_unref(bus);
        }
        if (v->cb.on_started) v->cb.on_started(v, r->ok, r->info, v->cb.user);
    }
    g_free(r->info);
    g_free(r);
    video_unref(v);
    return G_SOURCE_REMOVE;
}

static char *cache_path(void) {
    return g_build_filename(g_get_user_cache_dir(), "multiscreen", "encoder", NULL);
}

static gpointer start_thread(gpointer data) {
    Video *v = data;
    int order[N_CAND], n = 0;
    /* ordine: preferenza utente, poi quello che ha funzionato l'ultima volta, poi gli altri */
    char *cached = NULL;
    char *cp = cache_path();
    g_file_get_contents(cp, &cached, NULL, NULL);
    if (cached) g_strstrip(cached);
    const char *first[2] = {v->pref && strcmp(v->pref, "auto") ? v->pref : NULL, cached};
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < N_CAND; i++)
            if (first[f] && strcmp(CANDIDATES[i].id, first[f]) == 0) {
                bool dup = false;
                for (int j = 0; j < n; j++) dup |= order[j] == i;
                if (!dup) order[n++] = i;
            }
    for (int i = 0; i < N_CAND; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++) dup |= order[j] == i;
        if (!dup) order[n++] = i;
    }

    StartResult *r = g_new0(StartResult, 1);
    r->v = v;
    GString *errs = g_string_new(NULL);
    for (int k = 0; k < n && !g_atomic_int_get(&v->stopping); k++) {
        const Candidate *c = &CANDIDATES[order[k]];
        if (is_bad(c->id)) continue;
        char *err = NULL;
        g_atomic_int_set(&v->got_sample, 0);
        GstElement *p = build(v, c, &err);
        if (p) {
            g_mutex_lock(&v->lock);
            v->pipeline = p;
            g_mutex_unlock(&v->lock);
            LOGI("video: provo encoder '%s' (%s), %d kbit/s, max %d fps", c->id, c->element, v->bitrate, v->fps);
            GstStateChangeReturn sr = gst_element_set_state(p, GST_STATE_PLAYING);
            if (sr != GST_STATE_CHANGE_FAILURE && wait_start(v, p, &err)) {
                log_caps(p, "src", "src");
                log_caps(p, "enc", "sink");
                log_caps(p, "sink", "sink");
                g_strlcpy(v->enc_name, c->id, sizeof v->enc_name);
                r->ok = true;
                r->info = g_strdup(c->id);
                char *dir = g_path_get_dirname(cp);
                g_mkdir_with_parents(dir, 0700);
                g_file_set_contents(cp, c->id, -1, NULL);
                g_free(dir);
                break;
            }
            if (!err) err = g_strdup("avvio fallito");
            g_mutex_lock(&v->lock);
            v->pipeline = NULL;
            g_mutex_unlock(&v->lock);
            gst_element_set_state(p, GST_STATE_NULL);
            if (v->sink) gst_object_unref(v->sink);
            if (v->encoder) gst_object_unref(v->encoder);
            v->sink = v->encoder = NULL;
            gst_object_unref(p);
        }
        LOGW("video: encoder '%s' non va: %s", c->id, err);
        g_string_append_printf(errs, "%s: %s; ", c->id, err);
        g_free(err);
    }
    if (!r->ok) r->info = g_string_free(errs, FALSE);
    else g_string_free(errs, TRUE);
    g_free(cached);
    g_free(cp);
    g_idle_add(deliver_start, r);
    return NULL;
}

Video *video_start(uint32_t node, int bitrate_kbps, int fps, const char *encoder_pref, const VideoCallbacks *cb) {
    Video *v = g_new0(Video, 1);
    v->node = node;
    v->bitrate = bitrate_kbps;
    v->fps = fps;
    v->min_interval_us = fps > 0 && fps < 60 ? (1000000 / fps) - 2000 : 0;
    v->pref = g_strdup(encoder_pref ? encoder_pref : "auto");
    v->cb = *cb;
    v->refs = 2; /* chiamante + thread */
    g_mutex_init(&v->lock);
    v->thread = g_thread_new("ms-video-start", start_thread, v);
    return v;
}

void video_force_keyframe(Video *v) {
    if (!v) return;
    g_mutex_lock(&v->lock);
    GstElement *sink = v->pipeline ? v->sink : NULL;
    if (sink) gst_object_ref(sink);
    g_mutex_unlock(&v->lock);
    if (!sink) return;
    GstEvent *e = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
    gst_element_send_event(sink, e);
    gst_object_unref(sink);
}

void video_update(Video *v, int bitrate_kbps, int fps) {
    if (!v) return;
    v->fps = fps;
    v->min_interval_us = fps > 0 && fps < 60 ? (1000000 / fps) - 2000 : 0;
    g_mutex_lock(&v->lock);
    GstElement *enc = v->pipeline && v->encoder ? gst_object_ref(v->encoder) : NULL;
    GstElement *src = v->pipeline ? gst_bin_get_by_name(GST_BIN(v->pipeline), "src") : NULL;
    g_mutex_unlock(&v->lock);
    if (enc) {
        GParamSpec *ps = g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "bitrate");
        if (ps && (ps->flags & GST_PARAM_MUTABLE_PLAYING)) {
            char b[16];
            snprintf(b, sizeof b, "%d", bitrate_kbps);
            gst_util_set_object_arg(G_OBJECT(enc), "bitrate", b);
            v->bitrate = bitrate_kbps;
            LOGI("video: qualita' %d kbit/s, max %d fps (al volo)", bitrate_kbps, fps);
        } else {
            LOGW("video: l'encoder non cambia la qualita' al volo (vale dal prossimo collegamento)");
        }
        gst_object_unref(enc);
    }
    if (src) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(src), "keepalive-time"))
            g_object_set(src, "keepalive-time", fps < 60 ? 100 : 1000, NULL);
        gst_object_unref(src);
    }
    video_force_keyframe(v);
}

const char *video_encoder_name(Video *v) { return v ? v->enc_name : ""; }

void video_stop(Video *v) {
    if (!v) return;
    g_atomic_int_set(&v->stopping, 1);
    if (v->thread) {
        g_thread_join(v->thread);
        v->thread = NULL;
    }
    if (v->bus_watch) g_source_remove(v->bus_watch);
    g_mutex_lock(&v->lock);
    GstElement *p = v->pipeline;
    v->pipeline = NULL;
    g_mutex_unlock(&v->lock);
    if (p) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
    }
    if (v->sink) gst_object_unref(v->sink);
    if (v->encoder) gst_object_unref(v->encoder);
    v->sink = v->encoder = NULL;
    video_unref(v);
}
