#ifndef TIESTONE_TEST_HTTP_SERVER_H
#define TIESTONE_TEST_HTTP_SERVER_H

/* A loopback HTTP server for testing what the real http backends put on the
   wire. Routes are declared before start and observations are read after stop
   has joined the accept thread, so that thread owns the routes for as long as
   it runs and they need no lock. The only value crossing threads live is the
   stop flag. */
typedef struct fr_test_server fr_test_server;

/* Binds 127.0.0.1 on a port the OS picks, so concurrent runs cannot collide.
   Returns NULL if the socket could not be bound. */
fr_test_server *fr_test_server_create(void);
int fr_test_server_port(const fr_test_server *server);

void fr_test_server_add_redirect(fr_test_server *server, const char *path,
                                 int status, const char *location);
void fr_test_server_add_body(fr_test_server *server, const char *path, const char *body);

void fr_test_server_start(fr_test_server *server);
/* Closes the listener, which is what ends the accept thread, then joins it. */
void fr_test_server_stop(fr_test_server *server);
void fr_test_server_free(fr_test_server *server);

/* Both answer for the LAST request that arrived for path, and are only valid
   once fr_test_server_stop has joined the accept thread. */
int fr_test_server_was_requested(const fr_test_server *server, const char *path);
int fr_test_server_saw_header(const fr_test_server *server, const char *path, const char *name);

#endif
