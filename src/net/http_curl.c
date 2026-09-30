#include "net/http.h"

#include "net/url.h"
#include "util/error.h"
#include "util/sha256.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A streamed download can legitimately run for far longer than any sane
   total-time cap (the toolchain archives this backend exists for are
   ~180 MB), so unlike fr_http_get's 8 MiB body path this one aborts on a
   stalled transfer instead of a slow one. */
#define FR_HTTP_STALL_BYTES_PER_SECOND 1024L
#define FR_HTTP_STALL_SECONDS 30L

typedef struct { char *data; size_t length; int overflowed; char **out_body; size_t *out_length; } fr_body_transfer;

typedef struct { FILE *file; size_t total; size_t max_bytes; fr_sha256 *digest; int exceeded; } fr_file_sink;

typedef enum { FETCH_DONE, FETCH_REDIRECT, FETCH_ERROR } fetch_outcome;

/* What one curl attempt needs beyond the connection setup and redirect
   handling every attempt shares: where bytes go while the transfer runs,
   how a transfer is judged too slow, and what (if anything) still needs
   doing once a 2xx has actually finished. write_data outlives a single
   attempt (a retried redirect reuses it), so begin_attempt resets it before
   every attempt rather than fetch_once allocating it fresh each time. */
typedef struct {
    curl_write_callback write_fn;
    void *write_data;
    int *exceeded;
    size_t exceeded_limit;
    int use_total_timeout;
    int (*begin_attempt)(void *write_data, fr_error *err);
    fetch_outcome (*on_success)(void *write_data, fr_error *err, const char *current_url);
} fr_transfer;

static size_t append_to_buffer(char *chunk, size_t size, size_t count, void *user_data) {
    fr_body_transfer *transfer = user_data;
    size_t incoming = size * count;
    if (transfer->length + incoming > FR_HTTP_MAX_BODY) {
        transfer->overflowed = 1;
        return 0;
    }
    char *grown = realloc(transfer->data, transfer->length + incoming + 1);
    if (grown == NULL) return 0;
    transfer->data = grown;
    memcpy(transfer->data + transfer->length, chunk, incoming);
    transfer->length += incoming;
    transfer->data[transfer->length] = '\0';
    return incoming;
}

/* The ceiling is checked, and the digest fed, only for bytes this actually
   writes to disk: a digest that covered more or less than the file would be
   a pin that proves nothing. */
static size_t write_to_file(char *chunk, size_t size, size_t count, void *user_data) {
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

static int begin_body_attempt(void *write_data, fr_error *err) {
    (void) err;
    fr_body_transfer *transfer = write_data;
    free(transfer->data);
    transfer->data = NULL;
    transfer->length = 0;
    transfer->overflowed = 0;
    return FR_OK;
}

static fetch_outcome finish_body_attempt(void *write_data, fr_error *err, const char *current_url) {
    (void) err; (void) current_url;
    fr_body_transfer *transfer = write_data;
    *transfer->out_body = transfer->data;
    *transfer->out_length = transfer->length;
    return FETCH_DONE;
}

/* A redirect response can stream a body of its own before curl_easy_perform
   returns and the status becomes known, so every attempt starts by rewinding
   the file and the running total: an earlier attempt's bytes must never
   survive into the one that actually succeeds. */
static int begin_file_attempt(void *write_data, fr_error *err) {
    fr_file_sink *sink = write_data;
    if (fseek(sink->file, 0, SEEK_SET) != 0 || ftruncate(fileno(sink->file), 0) != 0) {
        fr_error_set(err, "could not reset the destination file for a retry");
        return FR_ERR;
    }
    sink->total = 0;
    sink->exceeded = 0;
    if (sink->digest != NULL) fr_sha256_init(sink->digest);
    return FR_OK;
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

/* One attempt at one URL: builds the request the way the whole file needs it
   built (headers, protocol pinning, manual redirects) then hands off only
   what genuinely differs between the body fetch and the streamed-to-file
   fetch, namely where the response bytes go, how a stalled transfer is
   judged, and what finishing touch (if any) a 2xx status still needs.
   Redirects are driven manually (CURLOPT_FOLLOWLOCATION off) rather than by
   relying on CURLOPT_UNRESTRICTED_AUTH's default: that option only strips
   Authorization and Cookie, not every caller-supplied header that rule 2
   requires dropped on a redirect that leaves the original origin. */
static fetch_outcome fetch_once(const char *current_url, const char *original_url,
                                const fr_http_header *headers, size_t header_count,
                                const fr_transfer *transfer, char **out_redirect_url, fr_error *err) {
    static int initialised = 0;
    if (!initialised) { curl_global_init(CURL_GLOBAL_DEFAULT); initialised = 1; }

    if (transfer->begin_attempt != NULL && transfer->begin_attempt(transfer->write_data, err) != FR_OK) {
        return FETCH_ERROR;
    }

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
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, transfer->write_fn);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, transfer->write_data);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 60L);
    if (transfer->use_total_timeout) {
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    } else {
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, FR_HTTP_STALL_BYTES_PER_SECOND);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, FR_HTTP_STALL_SECONDS);
    }
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if (list != NULL) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);

    CURLcode result = curl_easy_perform(handle);

    if (transfer->exceeded != NULL && *transfer->exceeded) {
        fr_error_set(err, "%s returned a body larger than %zu bytes", current_url, transfer->exceeded_limit);
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

    outcome = transfer->on_success != NULL
                  ? transfer->on_success(transfer->write_data, err, current_url)
                  : FETCH_DONE;

cleanup:
    curl_slist_free_all(list);
    if (handle != NULL) curl_easy_cleanup(handle);
    return outcome;
}

/* Drives one url through as many redirects as FR_HTTP_MAX_REDIRECTS allows,
   for whichever transfer configuration the caller supplies: the body fetch
   and the streamed-to-file fetch differ only in what they do with the
   bytes, not in how a redirect is followed. */
static int follow_redirects(const char *url, const fr_http_header *headers, size_t header_count,
                            const fr_transfer *transfer, fr_error *err) {
    char *current_url = dup_cstr(url);
    if (current_url == NULL) {
        fr_error_set(err, "out of memory resolving %s", url);
        return FR_ERR;
    }

    fetch_outcome result;
    int attempt_count = 0;
    do {
        char *redirect_url = NULL;
        result = fetch_once(current_url, url, headers, header_count, transfer, &redirect_url, err);
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
    fr_body_transfer body;
    memset(&body, 0, sizeof body);
    body.out_body = out_body;
    body.out_length = out_length;

    fr_transfer transfer;
    memset(&transfer, 0, sizeof transfer);
    transfer.write_fn = append_to_buffer;
    transfer.write_data = &body;
    transfer.exceeded = &body.overflowed;
    transfer.exceeded_limit = FR_HTTP_MAX_BODY;
    transfer.use_total_timeout = 1;
    transfer.begin_attempt = begin_body_attempt;
    transfer.on_success = finish_body_attempt;

    int result = follow_redirects(url, headers, header_count, &transfer, err);
    if (result != FR_OK) {
        free(body.data);
        return FR_ERR;
    }
    return FR_OK;
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

    fr_transfer transfer;
    memset(&transfer, 0, sizeof transfer);
    transfer.write_fn = write_to_file;
    transfer.write_data = &sink;
    transfer.exceeded = &sink.exceeded;
    transfer.exceeded_limit = max_bytes;
    transfer.use_total_timeout = 0;
    transfer.begin_attempt = begin_file_attempt;
    transfer.on_success = NULL;

    int result = follow_redirects(url, headers, header_count, &transfer, err);

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

/* curl_global_cleanup is deliberately never called: a library cannot know it
   is the last user of the process. */
