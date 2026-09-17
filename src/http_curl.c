#include "http.h"

#include "error.h"
#include "url.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *data; size_t length; int overflowed; } fr_buffer;

typedef enum { FETCH_DONE, FETCH_REDIRECT, FETCH_ERROR } fetch_outcome;

static size_t append(void *chunk, size_t size, size_t count, void *user_data) {
    fr_buffer *buffer = user_data;
    size_t incoming = size * count;
    if (buffer->length + incoming > FR_HTTP_MAX_BODY) {
        buffer->overflowed = 1;
        return 0;
    }
    char *grown = realloc(buffer->data, buffer->length + incoming + 1);
    if (grown == NULL) return 0;
    buffer->data = grown;
    memcpy(buffer->data + buffer->length, chunk, incoming);
    buffer->length += incoming;
    buffer->data[buffer->length] = '\0';
    return incoming;
}

static char *dup_cstr(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static int is_redirect_status(long status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Redirects are driven manually (CURLOPT_FOLLOWLOCATION off) rather than by
   relying on CURLOPT_UNRESTRICTED_AUTH's default: that option only strips
   Authorization and Cookie, not every caller-supplied header that rule 2
   requires dropped on a redirect that leaves the original origin. */
static fetch_outcome fetch_once(const char *current_url, const char *original_url,
                                const fr_http_header *headers, size_t header_count,
                                char **out_body, size_t *out_length,
                                char **out_redirect_url, fr_error *err) {
    static int initialised = 0;
    if (!initialised) { curl_global_init(CURL_GLOBAL_DEFAULT); initialised = 1; }

    fetch_outcome outcome = FETCH_ERROR;
    struct curl_slist *list = NULL;
    CURL *handle = NULL;

    handle = curl_easy_init();
    if (handle == NULL) {
        fr_error_set(err, "could not create an http client for %s", current_url);
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
            struct curl_slist *grown = curl_slist_append(list, line);
            if (grown == NULL) {
                fr_error_set(err, "out of memory building headers for %s", current_url);
                goto cleanup;
            }
            list = grown;
        }
    }

    fr_buffer buffer;
    memset(&buffer, 0, sizeof buffer);

    curl_easy_setopt(handle, CURLOPT_URL, current_url);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "daukle");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if (list != NULL) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);

    CURLcode result = curl_easy_perform(handle);

    if (buffer.overflowed) {
        fr_error_set(err, "%s returned a body larger than %u bytes", current_url, FR_HTTP_MAX_BODY);
        free(buffer.data);
        goto cleanup;
    }
    if (result != CURLE_OK) {
        fr_error_set(err, "%s failed: %s", current_url, curl_easy_strerror(result));
        free(buffer.data);
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

    if (is_redirect_status(status)) {
        char *location = NULL;
        curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &location);
        if (location == NULL) {
            fr_error_set(err, "%s redirected without a location", current_url);
            free(buffer.data);
            goto cleanup;
        }
        char *redirect_url = dup_cstr(location);
        free(buffer.data);
        if (redirect_url == NULL) {
            fr_error_set(err, "out of memory resolving %s", current_url);
            goto cleanup;
        }
        *out_redirect_url = redirect_url;
        outcome = FETCH_REDIRECT;
        goto cleanup;
    }

    if (status < 200 || status > 299) {
        fr_error_set(err, "%s returned status %ld", current_url, status);
        free(buffer.data);
        goto cleanup;
    }

    *out_body = buffer.data;
    *out_length = buffer.length;
    outcome = FETCH_DONE;

cleanup:
    curl_slist_free_all(list);
    if (handle != NULL) curl_easy_cleanup(handle);
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

/* curl_global_cleanup is deliberately never called: a library cannot know it
   is the last user of the process. */
