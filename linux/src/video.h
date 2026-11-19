#pragma once
#include "common.h"

typedef struct Video Video;

typedef struct {
    /* chiamato dal thread di streaming per ogni access unit H.264 (Annex-B) */
    void (*on_sample)(const guint8 *data, gsize size, guint64 pts_us, bool key, int w, int h, void *user);
    /* chiamato nel main loop: pipeline pronta (ok=true) o fallita definitivamente */
    void (*on_started)(Video *v, bool ok, const char *info, void *user);
    /* chiamato nel main loop per errori a pipeline avviata */
    void (*on_error)(Video *v, const char *msg, void *user);
    void *user;
} VideoCallbacks;

/* Avvia (in un thread) la cattura del nodo PipeWire e l'encoding; prova piu' encoder. */
Video *video_start(uint32_t node, int bitrate_kbps, int fps, const char *encoder_pref, const VideoCallbacks *cb);
void video_force_keyframe(Video *v);
/* cambia qualita' e fps al volo, senza fermare la cattura */
void video_update(Video *v, int bitrate_kbps, int fps);
void video_stop(Video *v);
const char *video_encoder_name(Video *v);
