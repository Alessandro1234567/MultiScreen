#pragma once
#include "common.h"

/* Trasferimento file PC <-> tablet sul collegamento di una sessione. */
typedef struct Files Files;
typedef struct {
    /* invia un pacchetto (thread-safe); false se il tablet non e' collegato */
    bool (*send)(int type, const void *a, size_t al, const void *b, size_t bl, void *user);
    /* attende che il collegamento non sia congestionato (per non disturbare il video) */
    void (*throttle)(void *user);
    bool (*connected)(void *user);
    void *user;
} FilesIO;

Files *files_new(const FilesIO *io);
void files_free(Files *f); /* attende la fine del thread di invio */
/* pacchetti dal tablet (main loop) */
void files_on_begin(Files *f, const char *json);
void files_on_data(Files *f, const uint8_t *d, uint32_t len);
void files_on_end(Files *f, const uint8_t *d, uint32_t len);
void files_on_result(Files *f, const char *json);
void files_client_gone(Files *f);
/* accoda un file da mandare al tablet; ritorna messaggio d'errore o NULL */
char *files_send_path(Files *f, const char *path);
const char *files_download_dir(void);
