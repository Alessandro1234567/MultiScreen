#pragma once
#include "common.h"

typedef struct Kde Kde;
typedef struct KdeStream KdeStream;

enum { KDE_STREAM_CREATED, KDE_STREAM_FAILED, KDE_STREAM_CLOSED };
typedef void (*KdeStreamCb)(KdeStream *s, int event, uint32_t node, const char *err, void *user);

Kde *kde_connect(char **err);
void kde_free(Kde *k);
int kde_fd(Kde *k);
/* legge e smista gli eventi; -1 se la connessione a KWin e' persa */
int kde_dispatch(Kde *k);
void kde_flush(Kde *k);
bool kde_has_screencast(Kde *k);
bool kde_has_fake_input(Kde *k);
uint32_t kde_screencast_version(Kde *k);

/* width/height logici, scale fattore di scala dello schermo virtuale */
KdeStream *kde_stream_virtual(Kde *k, const char *name, int lw, int lh, double scale, KdeStreamCb cb, void *user);
void kde_stream_close(KdeStream *s);
/* stream di un'uscita esistente (modalita' duplica): sceglie lo schermo principale, ne scrive il nome */
KdeStream *kde_stream_existing(Kde *k, char *name_out, size_t name_len, KdeStreamCb cb, void *user);

/* geometria logica dell'uscita: nome esatto, oppure "Virtual-<nome>" (come KWin chiama le uscite virtuali);
 * nome vuoto = la prima */
bool kde_output_geometry(Kde *k, const char *name, int *x, int *y, int *w, int *h);
typedef struct {
    char name[96];
    int x, y, w, h;
} KdeOut;
int kde_list_outputs(Kde *k, KdeOut *out, int max);
void kde_log_outputs(Kde *k);

/* input (coordinate logiche globali del desktop) */
void kde_touch_down(Kde *k, uint32_t id, double x, double y);
void kde_touch_motion(Kde *k, uint32_t id, double x, double y);
void kde_touch_up(Kde *k, uint32_t id);
void kde_touch_cancel(Kde *k);
void kde_touch_frame(Kde *k);
void kde_pointer_abs(Kde *k, double x, double y);
void kde_button(Kde *k, uint32_t button, bool pressed);
void kde_axis(Kde *k, uint32_t axis, double value);
