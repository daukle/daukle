#include "http.h"

#include "error.h"
#include "sha256.h"
#include "url.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { char *data; size_t length; int overflowed; } fr_buffer;

typedef struct { char **out_body; size_t *out_length; } fr_body_sink;

typedef struct { FILE *file; size_t total; size_t max_bytes; fr_sha256 *digest; int exceeded; } fr_file_sink;

typedef enum { FETCH_DONE, FETCH_REDIRECT, FETCH_ERROR } fetch_outcome;

typedef fetch_outcome (*fr_attempt_fn)(const char *current_url, const char *original_url,
                                       const fr_http_header *headers, size_t header_count,
                                       void *sink, char **out_redirect_url, fr_error *err);

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

/* The ceiling is checked, and the digest fed, only for bytes this actually
   writes to disk: a digest that covered more or less than the file would be
   a pin that proves nothing. */
static size_t write_to_file(void *chunk, size_t size, size_t count, void *user_data) {
    fr_file_sink *sink = user_data;
    size_t incoming = size * count;
    if (sink->total + incoming > sink->max_bytes) {
        sink->exceeded = 1;
        return 0;
    }
    if (fwrite(chunk, 1, incoming, sink->file) != incoming) return 0;
    if (sink->digest != NULL) fr_sha256_update(sink->digest, chunk, incoming);
    sink->total += incoming;
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

/* Drives one url through as many redirects as FR_HTTP_MAX_REDIRECTS allows,
   for whichever attempt function the caller supplies: the body fetch and the
   streamed-to-file fetch differ only in what they do with the bytes, not in
   how a redirect is followed. */
static int follow_redirects(const char *url, const fr_http_header *headers, size_t header_count,
                            fr_attempt_fn attempt, void *sink, fr_error *err) {
    char *current_url = dup_cstr(url);
    if (current_url == NULL) {
        fr_error_set(err, "out of memory resolving %s", url);
        return FR_ERR;
    }

    fetch_outcome result;
    int attempt_count = 0;
    do {
        char *redirect_url = NULL;
        result = attempt(current_url, url, headers, header_count, sink, &redirect_url, err);
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

/* Redirects are driven manually (CURLOPT_FOLLOWLOCATION off) rather than by
   relying on CURLOPT_UNRESTRICTED_AUTH's default: that option only strips
   Authorization and Cookie, not every caller-supplied header that rule 2
   requires dropped on a redirect that leaves the original origin. */
static fetch_outcome fetch_once(const char *current_url, const char *original_url,
                                const fr_http_header *headers, size_t header_count,
                                void *sink_ptr, char **out_redirect_url, fr_error *err) {
    fr_body_sink *sink = sink_ptr;
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

    *sink->out_body = buffer.data;
    *sink->out_length = buffer.length;
    outcome = FETCH_DONE;

cleanup:
    curl_slist_free_all(list);
    if (handle != NULL) curl_easy_cleanup(handle);
    return outcome;
}

/* A redirect attempt can stream a body of its own before curl_easy_perform
   returns and the status becomes known, so every attempt starts by rewinding
   the file and the running total: an earlier attempt's bytes must never
   survive into the one that actually succeeds. */
static fetch_outcome fetch_once_to_file(const char *current_url, const char *original_url,
                                        const fr_http_header *headers, size_t header_count,
                                        void *sink_ptr, char **out_redirect_url, fr_error *err) {
    fr_file_sink *sink = sink_ptr;
    static int initialised = 0;
    if (!initialised) { curl_global_init(CURL_GLOBAL_DEFAULT); initialised = 1; }

    if (fseek(sink->file, 0, SEEK_SET) != 0 || ftruncate(fileno(sink->file), 0) != 0) {
        fr_error_set(err, "could not reset %s for a retry", current_url);
        return FETCH_ERROR;
    }
    sink->total = 0;
    sink->exceeded = 0;
    if (sink->digest != NULL) fr_sha256_init(sink->digest);

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

    curl_easy_setopt(handle, CURLOPT_URL, current_url);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "daukle");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, sink);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if (list != NULL) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);

    CURLcode result = curl_easy_perform(handle);

    if (sink->exceeded) {
        fr_error_set(err, "%s returned a body larger than %zu bytes", current_url, sink->max_bytes);
        goto cleanup;
    }
    if (result != CURLE_OK) {
        fr_error_set(err, "%s failed: %s", current_url, curl_easy_strerror(result));
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

    if (is_redirect_status(status)) {
        char *location = NULL;
        curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &location);
        if (location == NULL) {
            fr_error_set(err, "%s redirected without a location", current_url);
            goto cleanup;
        }
        char *redirect_url = dup_cstr(location);
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
        goto cleanup;
    }

    outcome = FETCH_DONE;

cleanup:
    curl_slist_free_all(list);
    if (handle != NULL) curl_easy_cleanup(handle);
    return outcome;
}

int fr_http_backend_get(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err) {
    fr_body_sink sink;
    sink.out_body = out_body;
    sink.out_length = out_length;
    return follow_redirects(url, headers, header_count, fetch_once, &sink, err);
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

    int result = follow_redirects(url, headers, header_count, fetch_once_to_file, &sink, err);
    fclose(file);

    if (result != FR_OK) {
        remove(path);
        return FR_ERR;
    }

    *out_length = sink.total;
    return FR_OK;
}

/* curl_global_cleanup is deliberately never called: a library cannot know it
   is the last user of the process. */
