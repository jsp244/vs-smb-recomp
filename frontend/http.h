/*
 * http.h - blocking HTTPS GET requests, made by the update checker (updater.c) on its worker
 * thread. WinHTTP on Windows; libcurl on Linux, loaded at run time (HTTP_CURL_DLOPEN) so the
 * program also starts where it is not installed; the Java side's HttpURLConnection on Android.
 * Only https:// addresses are requested, redirects are followed only to https:// addresses, and
 * the server's certificate is checked against the system's trusted ones.
 */
#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

typedef struct {
    const char *url;              /* https:// only */
    const char *accept;           /* Accept header, or NULL */
    const char *user_agent;
    int timeout_ms;               /* for connecting, and for each wait for more data */
    size_t max_len;               /* a longer body fails the request (0: 64 MiB) */
    /* called on the worker thread as the body arrives (total -1 when the server did not say);
       a nonzero return cancels the request */
    int (*progress)(void *ctx, size_t received, long long total);
    void *progress_ctx;
} http_request_t;

typedef struct {
    int status;                   /* the HTTP status, or 0 when no response arrived */
    char *body;                   /* the response, NUL terminated (free() it); NULL with status 0 */
    size_t len;
    char error[256];              /* with status 0: what went wrong */
} http_response_t;

/* Main thread, before requests are made: loads libcurl (Linux) or finds the Java side (Android).
   Returns 1 when requests can be made, else 0 with the reason in *why (when why is not NULL). */
int http_init(const char **why);

/* Makes the request; returns 1 when a response arrived (whatever its status). */
int http_request(const http_request_t *rq, http_response_t *rs);

#endif
