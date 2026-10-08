/*
 * gdrive.h: the Google Drive v3 operations rDrive needs, as blocking C calls
 * for the worker thread (built on rsym_https and cJSON).
 */
#ifndef GDRIVE_H
#define GDRIVE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gdrive_auth {
    char client_id[160];
    char client_secret[96];
    char refresh_token[256];
    char access_token[2048];
    long long expires_ms;        /* mbedtls_ms_time() when access_token expires */
} gdrive_auth;

typedef struct gdrive_file {
    char id[80];
    char name[256];              /* UTF-8 */
    char mime[96];
    long long size;              /* -1 if none (folders, Google Docs) */
    char modified[32];           /* RFC 3339, e.g. 2026-10-08T10:28:00.000Z */
    int is_folder;
} gdrive_file;

typedef struct gdrive_list {
    gdrive_file *files;
    int count;
} gdrive_list;

/* Token file written by env/rdrive-auth.py. Returns 0 or -1 (err filled). */
int gdrive_load_token(const char *path, gdrive_auth *auth, char *err, int errlen);
/* Get a fresh access token if the current one is missing or about to expire.
 * Returns 0, or -1 with err filled (e.g. revoked/expired refresh token). */
int gdrive_ensure_access(gdrive_auth *auth, char *err, int errlen);
/* List a folder ("root" for My Drive): folders first, then by name.
 * Returns 0 or -1; free with gdrive_list_free. */
int gdrive_list_folder(gdrive_auth *auth, const char *folder_id, gdrive_list *out,
                       char *err, int errlen);
void gdrive_list_free(gdrive_list *list);
/* Download a (non-Google-Docs) file to dest_path. *done receives bytes so
 * far while running (may be read from another thread). */
int gdrive_download(gdrive_auth *auth, const char *file_id, const char *dest_path,
                    volatile long long *done, char *err, int errlen);
/* Upload local_path into folder_id under name (UTF-8). */
int gdrive_upload(gdrive_auth *auth, const char *folder_id, const char *local_path,
                  const char *name, char *err, int errlen);

#ifdef __cplusplus
}
#endif

#endif
