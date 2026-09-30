#include "net/http.h"

#include "util/error.h"
#include "util/sha256.h"
#include "net/url.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct { char **out_body; size_t *out_length; } fr_body_sink;

typedef struct { FILE *file; size_t total; size_t max_bytes; fr_sha256 *digest; } fr_file_sink;

typedef enum { FETCH_DONE, FETCH_REDIRECT, FETCH_ERROR } fetch_outcome;

/* What a successful, non-redirected response is handed to once the request
   handle has a 2xx status: buffering it into memory and streaming it to a
   file differ only here, not in how the connection got opened or how a
   redirect was decided. */
typedef fetch_outcome (*fr_consume_fn)(HINTERNET request, const char *current_url,
                                       void *sink, fr_error *err);

static wchar_t *widen(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t *wide = malloc((size_t) length * sizeof *wide);
    if (wide != NULL) MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
    return wide;
}

static char *dup_cstr(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static wchar_t *dup_component(const wchar_t *text, DWORD length) {
    wchar_t *copy = malloc(((size_t) length + 1) * sizeof *copy);
    if (copy == NULL) return NULL;
    memcpy(copy, text, (size_t) length * sizeof *copy);
    copy[length] = L'\0';
    return copy;
}

static int is_redirect_status(DWORD status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* A Location header is not required to be absolute. curl resolves this for
   us via CURLINFO_REDIRECT_URL; WinHTTP hands back the header as written, so
   an absolute-path redirect (the common case) is resolved against the
   current url's origin here instead. A relative reference that is neither
   absolute nor absolute-path is treated as absolute-path too, which covers
   every redirect this project's toolchain hosts actually send. */
static char *resolve_redirect_url(const char *current_url, const char *location) {
    if (strstr(location, "://") != NULL) return dup_cstr(location);

    const char *authority = strstr(current_url, "://");
    if (authority == NULL) return dup_cstr(location);
    authority += 3;
    const char *path_start = authority + strcspn(authority, "/?#");
    size_t origin_length = (size_t) (path_start - current_url);

    size_t location_length = strlen(location);
    int needs_slash = location_length == 0 || location[0] != '/';
    char *resolved = malloc(origin_length + (size_t) needs_slash + location_length + 1);
    if (resolved == NULL) return NULL;
    memcpy(resolved, current_url, origin_length);
    size_t offset = origin_length;
    if (needs_slash) resolved[offset++] = '/';
    memcpy(resolved + offset, location, location_length);
    resolved[offset + location_length] = '\0';
    return resolved;
}

static fetch_outcome consume_into_memory(HINTERNET request, const char *current_url,
                                         void *sink_ptr, fr_error *err) {
    fr_body_sink *sink = sink_ptr;
    char *body = NULL;
    size_t length = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            fr_error_set(err, "%s failed while reading the response", current_url);
            free(body);
            return FETCH_ERROR;
        }
        if (available == 0) break;
        if (length + available > FR_HTTP_MAX_BODY) {
            fr_error_set(err, "%s returned a body larger than %u bytes", current_url, FR_HTTP_MAX_BODY);
            free(body);
            return FETCH_ERROR;
        }
        char *grown = realloc(body, length + available + 1);
        if (grown == NULL) {
            fr_error_set(err, "out of memory reading %s", current_url);
            free(body);
            return FETCH_ERROR;
        }
        body = grown;
        DWORD chunk_read = 0;
        if (!WinHttpReadData(request, body + length, available, &chunk_read)) {
            fr_error_set(err, "%s failed while reading the response", current_url);
            free(body);
            return FETCH_ERROR;
        }
        length += chunk_read;
        body[length] = '\0';
    }

    *sink->out_body = body;
    *sink->out_length = length;
    return FETCH_DONE;
}

static fetch_outcome consume_into_file(HINTERNET request, const char *current_url,
                                       void *sink_ptr, fr_error *err) {
    fr_file_sink *sink = sink_ptr;
    char chunk[65536];
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            fr_error_set(err, "%s failed while reading the response", current_url);
            return FETCH_ERROR;
        }
        if (available == 0) break;
        DWORD to_read = available < sizeof chunk ? available : (DWORD) sizeof chunk;
        DWORD chunk_read = 0;
        if (!WinHttpReadData(request, chunk, to_read, &chunk_read)) {
            fr_error_set(err, "%s failed while reading the response", current_url);
            return FETCH_ERROR;
        }
        if (sink->total + chunk_read > sink->max_bytes) {
            fr_error_set(err, "%s returned a body larger than %zu bytes", current_url, sink->max_bytes);
            return FETCH_ERROR;
        }
        if (fwrite(chunk, 1, chunk_read, sink->file) != chunk_read) {
            fr_error_set(err, "%s failed while writing to disk", current_url);
            return FETCH_ERROR;
        }
        if (sink->digest != NULL) fr_sha256_update(sink->digest, chunk, chunk_read);
        sink->total += chunk_read;
    }
    return FETCH_DONE;
}

/* One attempt at one URL: connects, sends the request with the caller's
   headers attached only when the current url still shares an origin with the
   very first request (rule 2), decides between a redirect and a 2xx
   response, and for the latter hands the still-open request handle to
   whichever consumer the caller supplied. Handles are per attempt because
   the target host can change between them. */
static fetch_outcome attempt_once(const char *current_url, const char *original_url,
                                  const fr_http_header *headers, size_t header_count,
                                  fr_consume_fn consume, void *sink,
                                  char **out_redirect_url, fr_error *err) {
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    wchar_t *wide_url = NULL;
    wchar_t *host = NULL;
    wchar_t *path = NULL;
    fetch_outcome outcome = FETCH_ERROR;

    wide_url = widen(current_url);
    if (wide_url == NULL) {
        fr_error_set(err, "could not encode %s", current_url);
        return FETCH_ERROR;
    }

    URL_COMPONENTS components;
    memset(&components, 0, sizeof components);
    components.dwStructSize = sizeof components;
    components.dwHostNameLength = (DWORD) -1;
    components.dwUrlPathLength = (DWORD) -1;

    if (!WinHttpCrackUrl(wide_url, 0, 0, &components)) {
        fr_error_set(err, "could not parse %s", current_url);
        free(wide_url);
        return FETCH_ERROR;
    }

    host = dup_component(components.lpszHostName, components.dwHostNameLength);
    path = components.dwUrlPathLength > 0
               ? dup_component(components.lpszUrlPath, components.dwUrlPathLength)
               : dup_component(L"/", 1);
    if (host == NULL || path == NULL) {
        fr_error_set(err, "out of memory resolving %s", current_url);
        goto cleanup;
    }

    session = WinHttpOpen(L"daukle", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        fr_error_set(err, "could not create an http session for %s", current_url);
        goto cleanup;
    }

    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (connection == NULL) {
        fr_error_set(err, "could not connect for %s", current_url);
        goto cleanup;
    }

    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (request == NULL) {
        fr_error_set(err, "could not open a request for %s", current_url);
        goto cleanup;
    }

    DWORD disable_redirects = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redirects, sizeof disable_redirects)) {
        fr_error_set(err, "could not disable automatic redirects for %s", current_url);
        goto cleanup;
    }

    if (fr_url_same_origin(original_url, current_url)) {
        for (size_t index = 0; index < header_count; index++) {
            char line[1024];
            int wanted = snprintf(line, sizeof line, "%s: %s", headers[index].name, headers[index].value);
            if (wanted < 0 || (size_t) wanted >= sizeof line) {
                fr_error_set(err, "header \"%s\" is too long to send", headers[index].name);
                goto cleanup;
            }
            wchar_t *wide_line = widen(line);
            if (wide_line == NULL) {
                fr_error_set(err, "could not encode header \"%s\"", headers[index].name);
                goto cleanup;
            }
            BOOL added = WinHttpAddRequestHeaders(request, wide_line, (DWORD) -1, WINHTTP_ADDREQ_FLAG_ADD);
            free(wide_line);
            if (!added) {
                fr_error_set(err, "could not send header \"%s\" to %s", headers[index].name, current_url);
                goto cleanup;
            }
        }
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        fr_error_set(err, "%s failed", current_url);
        goto cleanup;
    }

    DWORD status = 0;
    DWORD status_size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);

    if (is_redirect_status(status)) {
        DWORD location_size = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            NULL, &location_size, WINHTTP_NO_HEADER_INDEX);
        if (location_size == 0) {
            fr_error_set(err, "%s redirected without a location", current_url);
            goto cleanup;
        }
        wchar_t *location = malloc(location_size);
        if (location == NULL) {
            fr_error_set(err, "out of memory resolving %s", current_url);
            goto cleanup;
        }
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                 location, &location_size, WINHTTP_NO_HEADER_INDEX)) {
            fr_error_set(err, "%s redirected without a readable location", current_url);
            free(location);
            goto cleanup;
        }

        int needed = WideCharToMultiByte(CP_UTF8, 0, location, -1, NULL, 0, NULL, NULL);
        if (needed <= 0) {
            fr_error_set(err, "could not decode the redirect from %s", current_url);
            free(location);
            goto cleanup;
        }
        char *redirect_url = malloc((size_t) needed);
        if (redirect_url == NULL) {
            fr_error_set(err, "out of memory resolving %s", current_url);
            free(location);
            goto cleanup;
        }
        WideCharToMultiByte(CP_UTF8, 0, location, -1, redirect_url, needed, NULL, NULL);
        free(location);

        char *resolved_url = resolve_redirect_url(current_url, redirect_url);
        free(redirect_url);
        if (resolved_url == NULL) {
            fr_error_set(err, "out of memory resolving %s", current_url);
            goto cleanup;
        }

        *out_redirect_url = resolved_url;
        outcome = FETCH_REDIRECT;
        goto cleanup;
    }

    if (status < 200 || status > 299) {
        fr_error_set(err, "%s returned status %lu", current_url, (unsigned long) status);
        goto cleanup;
    }

    outcome = consume(request, current_url, sink, err);

cleanup:
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    free(wide_url);
    free(path);
    free(host);
    return outcome;
}

/* Drives one url through as many redirects as FR_HTTP_MAX_REDIRECTS allows,
   for whichever consumer the caller supplies: the body fetch and the
   streamed-to-file fetch differ only in what they do with the bytes, not in
   how a redirect is followed. */
static int follow_redirects(const char *url, const fr_http_header *headers, size_t header_count,
                            fr_consume_fn consume, void *sink, fr_error *err) {
    char *current_url = dup_cstr(url);
    if (current_url == NULL) {
        fr_error_set(err, "out of memory resolving %s", url);
        return FR_ERR;
    }

    fetch_outcome result;
    int attempt_count = 0;
    do {
        char *redirect_url = NULL;
        result = attempt_once(current_url, url, headers, header_count, consume, sink, &redirect_url, err);
        if (result == FETCH_REDIRECT) {
            free(current_url);
            current_url = redirect_url;
        }
        attempt_count++;
    } while (result == FETCH_REDIRECT && attempt_count <= FR_HTTP_MAX_REDIRECTS);

    if (result == FETCH_REDIRECT) {
        fr_error_set(err, "%s exceeded %d redirects", url, FR_HTTP_MAX_REDIRECTS);
    }

    free(current_url);
    return result == FETCH_DONE ? FR_OK : FR_ERR;
}

int fr_http_backend_get(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err) {
    fr_body_sink sink;
    sink.out_body = out_body;
    sink.out_length = out_length;
    return follow_redirects(url, headers, header_count, consume_into_memory, &sink, err);
}

int fr_http_get_to_file(const char *url, const fr_http_header *headers, size_t header_count,
                        const char *path, size_t max_bytes, fr_sha256 *digest,
                        size_t *out_length, fr_error *err) {
    *out_length = 0;

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fr_error_set(err, "could not create %s", path);
        return FR_ERR;
    }

    fr_file_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.file = file;
    sink.max_bytes = max_bytes;
    sink.digest = digest;
    if (digest != NULL) fr_sha256_init(digest);

    int result = follow_redirects(url, headers, header_count, consume_into_file, &sink, err);

    /* fwrite succeeding only means stdio accepted the bytes into its buffer,
       not that they reached the disk; a full disk can still fail silently
       until the flush inside fclose. */
    int write_failed = ferror(file);
    int close_failed = fclose(file) != 0;
    if (result == FR_OK && (write_failed || close_failed)) {
        fr_error_set(err, "could not save %s to disk", path);
        result = FR_ERR;
    }

    if (result != FR_OK) {
        remove(path);
        return FR_ERR;
    }

    *out_length = sink.total;
    return FR_OK;
}
