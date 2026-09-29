#include "http_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_handle;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_handle;
#define INVALID_SOCKET_HANDLE (-1)
#define close_socket close
#endif

#define MAX_ROUTES 8
#define MAX_REQUEST 4096

typedef struct {
    char path[128];
    int status;
    char location[512];
    char *body;
    size_t body_length;
    int requested;
    char request_head[MAX_REQUEST];
} fr_test_route;

struct fr_test_server {
    socket_handle listener;
    int port;
    fr_test_route routes[MAX_ROUTES];
    size_t route_count;
    volatile int stopping;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
    int thread_started;
#endif
};

static void start_sockets(void) {
#ifdef _WIN32
    static int started = 0;
    if (!started) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        started = 1;
    }
#endif
}

static fr_test_route *find_route(fr_test_server *server, const char *path) {
    for (size_t index = 0; index < server->route_count; index++) {
        if (strcmp(server->routes[index].path, path) == 0) return &server->routes[index];
    }
    return NULL;
}

static fr_test_route *add_route(fr_test_server *server, const char *path) {
    if (server->route_count == MAX_ROUTES) return NULL;
    fr_test_route *entry = &server->routes[server->route_count++];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->path, sizeof entry->path, "%s", path);
    return entry;
}

static const char *find_ignoring_case(const char *haystack, const char *needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) return haystack;
    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        size_t index = 0;
        while (index < needle_length && cursor[index] != '\0'
               && tolower((unsigned char) cursor[index]) == tolower((unsigned char) needle[index])) {
            index++;
        }
        if (index == needle_length) return cursor;
    }
    return NULL;
}

static void send_all(socket_handle client, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
#ifdef _WIN32
        int wrote = send(client, data + sent, (int) (length - sent), 0);
#else
        ssize_t wrote = send(client, data + sent, length - sent, 0);
#endif
        if (wrote <= 0) return;
        sent += (size_t) wrote;
    }
}

/* Reads until the blank line that ends the request head. Every response says
   Connection: close and no request carries a body, so the head is all there
   is to read. */
static int read_request_head(socket_handle client, char *buffer, size_t size) {
    size_t filled = 0;
    while (filled + 1 < size) {
#ifdef _WIN32
        int received = recv(client, buffer + filled, (int) (size - filled - 1), 0);
#else
        ssize_t received = recv(client, buffer + filled, size - filled - 1, 0);
#endif
        if (received <= 0) break;
        filled += (size_t) received;
        buffer[filled] = '\0';
        if (strstr(buffer, "\r\n\r\n") != NULL) return 1;
    }
    buffer[filled] = '\0';
    return filled > 0;
}

static void read_request_path(const char *head, char *out, size_t size) {
    out[0] = '\0';
    const char *start = strchr(head, ' ');
    if (start == NULL) return;
    start++;
    size_t length = strcspn(start, " \r\n");
    if (length >= size) length = size - 1;
    memcpy(out, start, length);
    out[length] = '\0';
}

static void serve_connection(fr_test_server *server, socket_handle client) {
    char head[MAX_REQUEST];
    if (!read_request_head(client, head, sizeof head)) return;

    char path[128];
    read_request_path(head, path, sizeof path);

    fr_test_route *entry = find_route(server, path);
    if (entry == NULL) {
        static const char *not_found =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client, not_found, strlen(not_found));
        return;
    }

    entry->requested = 1;
    snprintf(entry->request_head, sizeof entry->request_head, "%s", head);

    char response[1400];
    int written;
    if (entry->status == 200) {
        written = snprintf(response, sizeof response,
                           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                           entry->body_length);
        if (written > 0 && (size_t) written < sizeof response) {
            send_all(client, response, (size_t) written);
            send_all(client, entry->body, entry->body_length);
        }
        return;
    }

    written = snprintf(response, sizeof response,
                       "HTTP/1.1 %d Found\r\nLocation: %s\r\nContent-Length: 0\r\n"
                       "Connection: close\r\n\r\n",
                       entry->status, entry->location);
    if (written > 0 && (size_t) written < sizeof response) send_all(client, response, (size_t) written);
}

/* Waits for a connection with a timeout so the loop can notice that stop has
   been asked for. Closing the listener under a blocked accept would be
   simpler, but it wakes accept on Windows and BSD and NOT on Linux, where the
   thread would stay blocked and the join would never return. */
static int connection_is_waiting(socket_handle listener) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;

#ifdef _WIN32
    int count = 0;
#else
    int count = (int) listener + 1;
#endif
    return select(count, &readable, NULL, NULL, &timeout) > 0;
}

/* A client that connects and then sends nothing would otherwise hold the one
   server thread in recv for as long as the test runs. */
static void set_receive_timeout(socket_handle client) {
#ifdef _WIN32
    DWORD milliseconds = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *) &milliseconds, sizeof milliseconds);
#else
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
}

static void accept_loop(fr_test_server *server) {
    while (!server->stopping) {
        if (!connection_is_waiting(server->listener)) continue;
        socket_handle client = accept(server->listener, NULL, NULL);
        if (client == INVALID_SOCKET_HANDLE) continue;
        set_receive_timeout(client);
        serve_connection(server, client);
        close_socket(client);
    }
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID argument) {
    accept_loop(argument);
    return 0;
}
#else
static void *thread_main(void *argument) {
    accept_loop(argument);
    return NULL;
}
#endif

fr_test_server *fr_test_server_create(void) {
    start_sockets();

    fr_test_server *server = calloc(1, sizeof *server);
    if (server == NULL) return NULL;

    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener == INVALID_SOCKET_HANDLE) { free(server); return NULL; }

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server->listener, (struct sockaddr *) &address, sizeof address) != 0
        || listen(server->listener, 8) != 0) {
        close_socket(server->listener);
        free(server);
        return NULL;
    }

    struct sockaddr_in bound;
    memset(&bound, 0, sizeof bound);
#ifdef _WIN32
    int bound_size = (int) sizeof bound;
#else
    socklen_t bound_size = sizeof bound;
#endif
    if (getsockname(server->listener, (struct sockaddr *) &bound, &bound_size) != 0) {
        close_socket(server->listener);
        free(server);
        return NULL;
    }
    server->port = ntohs(bound.sin_port);
    return server;
}

int fr_test_server_port(const fr_test_server *server) {
    return server->port;
}

void fr_test_server_add_redirect(fr_test_server *server, const char *path,
                                 int status, const char *location) {
    fr_test_route *entry = add_route(server, path);
    if (entry == NULL) return;
    entry->status = status;
    snprintf(entry->location, sizeof entry->location, "%s", location);
}

void fr_test_server_add_body_bytes(fr_test_server *server, const char *path, const char *body,
                                   size_t length) {
    fr_test_route *entry = add_route(server, path);
    if (entry == NULL) return;
    entry->status = 200;
    entry->body = malloc(length > 0 ? length : 1);
    if (entry->body == NULL) return;
    memcpy(entry->body, body, length);
    entry->body_length = length;
}

void fr_test_server_add_body(fr_test_server *server, const char *path, const char *body) {
    fr_test_server_add_body_bytes(server, path, body, strlen(body));
}

void fr_test_server_start(fr_test_server *server) {
#ifdef _WIN32
    server->thread = CreateThread(NULL, 0, thread_main, server, 0, NULL);
#else
    server->thread_started = pthread_create(&server->thread, NULL, thread_main, server) == 0;
#endif
}

/* The thread is joined BEFORE the listener closes, so it never selects on a
   descriptor this has already closed. */
void fr_test_server_stop(fr_test_server *server) {
    server->stopping = 1;
#ifdef _WIN32
    if (server->thread != NULL) {
        WaitForSingleObject(server->thread, 5000);
        CloseHandle(server->thread);
        server->thread = NULL;
    }
#else
    if (server->thread_started) {
        pthread_join(server->thread, NULL);
        server->thread_started = 0;
    }
#endif
    if (server->listener != INVALID_SOCKET_HANDLE) {
        close_socket(server->listener);
        server->listener = INVALID_SOCKET_HANDLE;
    }
}

void fr_test_server_free(fr_test_server *server) {
    for (size_t index = 0; index < server->route_count; index++) free(server->routes[index].body);
    free(server);
}

int fr_test_server_was_requested(const fr_test_server *server, const char *path) {
    const fr_test_route *entry = find_route((fr_test_server *) server, path);
    return entry != NULL && entry->requested;
}

int fr_test_server_saw_header(const fr_test_server *server, const char *path, const char *name) {
    const fr_test_route *entry = find_route((fr_test_server *) server, path);
    if (entry == NULL || !entry->requested) return 0;

    char needle[128];
    snprintf(needle, sizeof needle, "\n%s:", name);
    return find_ignoring_case(entry->request_head, needle) != NULL;
}
