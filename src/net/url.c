#include "net/url.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    const char *scheme;
    size_t scheme_length;
    const char *host;
    size_t host_length;
    const char *port;
    size_t port_length;
} fr_origin;

static int equal_ignoring_case(const char *left, size_t left_length,
                               const char *right, size_t right_length) {
    if (left_length != right_length) return 0;
    for (size_t index = 0; index < left_length; index++) {
        if (tolower((unsigned char) left[index]) != tolower((unsigned char) right[index])) return 0;
    }
    return 1;
}

static const char *default_port(const char *scheme, size_t length) {
    if (equal_ignoring_case(scheme, length, "http", 4)) return "80";
    if (equal_ignoring_case(scheme, length, "https", 5)) return "443";
    return "";
}

/* Splits the port off an authority that may be a bracketed IPv6 literal, where
   the colons inside the brackets are part of the host. */
static int split_host_and_port(fr_origin *out) {
    if (out->host_length > 0 && out->host[0] == '[') {
        const char *closing = memchr(out->host, ']', out->host_length);
        if (closing == NULL) return 0;
        size_t bracketed = (size_t) (closing - out->host) + 1;
        if (bracketed < out->host_length) {
            if (out->host[bracketed] != ':') return 0;
            out->port = out->host + bracketed + 1;
            out->port_length = out->host_length - bracketed - 1;
        }
        out->host_length = bracketed;
        return 1;
    }

    const char *colon = memchr(out->host, ':', out->host_length);
    if (colon != NULL) {
        out->port = colon + 1;
        out->port_length = out->host_length - (size_t) (colon - out->host) - 1;
        out->host_length = (size_t) (colon - out->host);
    }
    return 1;
}

static int parse_origin(const char *url, fr_origin *out) {
    memset(out, 0, sizeof *out);

    const char *separator = strstr(url, "://");
    if (separator == NULL || separator == url) return 0;
    out->scheme = url;
    out->scheme_length = (size_t) (separator - url);

    const char *authority = separator + 3;
    size_t authority_length = strcspn(authority, "/?#");

    /* Userinfo is not part of an origin, and the LAST "@" ends it, so that
       "http://evil.example@host/" is judged on "host". */
    out->host = authority;
    for (size_t index = 0; index < authority_length; index++) {
        if (authority[index] == '@') out->host = authority + index + 1;
    }
    out->host_length = authority_length - (size_t) (out->host - authority);

    if (out->host_length == 0) return 0;
    if (!split_host_and_port(out)) return 0;
    if (out->host_length == 0) return 0;

    if (out->port_length == 0) {
        out->port = default_port(out->scheme, out->scheme_length);
        out->port_length = strlen(out->port);
    }
    return 1;
}

int fr_url_same_origin(const char *left, const char *right) {
    if (left == NULL || right == NULL) return 0;

    fr_origin left_origin;
    fr_origin right_origin;
    if (!parse_origin(left, &left_origin) || !parse_origin(right, &right_origin)) return 0;

    return equal_ignoring_case(left_origin.scheme, left_origin.scheme_length,
                               right_origin.scheme, right_origin.scheme_length)
        && equal_ignoring_case(left_origin.host, left_origin.host_length,
                               right_origin.host, right_origin.host_length)
        && equal_ignoring_case(left_origin.port, left_origin.port_length,
                               right_origin.port, right_origin.port_length);
}
