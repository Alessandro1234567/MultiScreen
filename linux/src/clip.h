#pragma once
#include "common.h"
/* Appunti del PC tramite wl-clipboard (wl-paste --watch / wl-copy). */
typedef void (*ClipCb)(const char *text, gsize len, void *user);
void clip_start(ClipCb cb, void *user);
void clip_set(const char *text, gsize len);
bool clip_available(void);
