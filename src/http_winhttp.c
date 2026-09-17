#include "http.h"

#include "error.h"
#include "url.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef enum { FETCH_DONE, FETCH_REDIRECT, FETCH_ERROR } fetch_outcome;

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

/* One attempt at one URL: connects, sends the request with the caller's
   headers attached only when the current url still shares an origin with the
   very first request (rule 2), and either returns the body, a redirect
   location, or an error. Handles are per attempt because the target host can
   change between them. */
static fetch_outcome fetch_once(const char *current_url, const char *original_url,
                                const fr_http_header *headers, size_t header_count,
                                char **out_body, size_t *out_length,
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

    session = WinHttpOpen(L"tiestone", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
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

        *out_redirect_url = redirect_url;
        outcome = FETCH_REDIRECT;
        goto cleanup;
    }

    if (status < 200 || status > 299) {
        fr_error_set(err, "%s returned status %lu", current_url, (unsigned long) status);
        goto cleanup;
    }

    char *body = NULL;
    size_t length = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            fr_error_set(err, "%s failed while reading the response", current_url);
            free(body);
            goto cleanup;
        }
        if (available == 0) break;
        if (length + available > FR_HTTP_MAX_BODY) {
            fr_error_set(err, "%s returned a body larger than %u bytes", current_url, FR_HTTP_MAX_BODY);
            free(body);
            goto cleanup;
        }
        char *grown = realloc(body, length + available + 1);
        if (grown == NULL) {
            fr_error_set(err, "out of memory reading %s", current_url);
            free(body);
            goto cleanup;
        }
        body = grown;
        DWORD chunk_read = 0;
        if (!WinHttpReadData(request, body + length, available, &chunk_read)) {
            fr_error_set(err, "%s failed while reading the response", current_url);
            free(body);
            goto cleanup;
        }
        length += chunk_read;
        body[length] = '\0';
    }

    *out_body = body;
    *out_length = length;
    outcome = FETCH_DONE;

cleanup:
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    free(wide_url);
    free(path);
    free(host);
    return outcome;
}

int fr_http_backend_get(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err) {
    char *current_url = dup_cstr(url);
    if (current_url == NULL) {
        fr_error_set(err, "out of memory resolving %s", url);
        return FR_ERR;
    }

    fetch_outcome result;
    int attempt = 0;
    do {
        char *redirect_url = NULL;
        result = fetch_once(current_url, url, headers, header_count,
                            out_body, out_length, &redirect_url, err);
        if (result == FETCH_REDIRECT) {
            free(current_url);
            current_url = redirect_url;
        }
        attempt++;
    } while (result == FETCH_REDIRECT && attempt <= FR_HTTP_MAX_REDIRECTS);

    if (result == FETCH_REDIRECT) {
        fr_error_set(err, "%s exceeded %d redirects", url, FR_HTTP_MAX_REDIRECTS);
    }

    free(current_url);
    return result == FETCH_DONE ? FR_OK : FR_ERR;
}
