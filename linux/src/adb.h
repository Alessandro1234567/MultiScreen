#pragma once
#include "common.h"

/* eventi consegnati nel main loop */
typedef struct {
    void (*on_ready)(const char *serial, const char *model, void *user);   /* tablet pronto (app installata, reverse attivo) */
    void (*on_gone)(const char *serial, void *user);                       /* scollegato */
    void (*on_message)(const char *title, const char *body, void *user);    /* da notificare all'utente */
    int (*port_for)(const char *serial, void *user);  /* porta del PC per questo tablet (chiamato dal thread adb) */
    void *user;
} AdbCallbacks;

/* device_port: porta usata dall'app sul tablet; token: segreto passato all'app all'avvio */
void adb_start(int device_port, const char *apk_path, int apk_version, const char *token, const AdbCallbacks *cb);
void adb_stop(void);
/* chiede di (ri)lanciare l'app sul tablet indicato (thread-safe) */
void adb_request_launch(const char *serial);
