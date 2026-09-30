#include "exec/exec.h"

#include "util/error.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char **build_argv(const fr_exec_request *request) {
    char **argv = malloc((request->argv_count + 2) * sizeof *argv);
    if (argv == NULL) return NULL;
    argv[0] = (char *) request->program;
    for (size_t index = 0; index < request->argv_count; index++) {
        argv[index + 1] = (char *) request->argv[index];
    }
    argv[request->argv_count + 1] = NULL;
    return argv;
}

extern char **environ;

static int name_matches(const char *entry, const char *name, size_t name_length) {
    return strncmp(entry, name, name_length) == 0 && entry[name_length] == '=';
}

/* Built in the PARENT and handed to execve, rather than calling setenv between
   fork and exec: only async-signal-safe functions may run there, and setenv
   allocates. Returns NULL with *failed clear when there is nothing to add, in
   which case the caller passes environ through unchanged. */
static char **build_envp(const fr_exec_request *request, char ***owned, int *failed) {
    *owned = NULL;
    *failed = 0;
    if (request->env_count == 0) return environ;

    size_t inherited = 0;
    while (environ[inherited] != NULL) inherited++;

    char **envp = malloc((inherited + request->env_count + 1) * sizeof *envp);
    if (envp == NULL) { *failed = 1; return NULL; }

    size_t count = 0;
    for (size_t index = 0; index < inherited; index++) {
        int overridden = 0;
        for (size_t entry = 0; entry < request->env_count && !overridden; entry++) {
            overridden = name_matches(environ[index], request->env[entry].name,
                                      strlen(request->env[entry].name));
        }
        if (!overridden) envp[count++] = environ[index];
    }

    /* The joined strings are the last `appended` entries, which is what
       free_envp relies on: the kept count is smaller than `inherited`
       whenever an entry overrode one, so the boundary cannot be derived
       from `inherited`. */
    size_t appended = 0;
    for (size_t entry = 0; entry < request->env_count; entry++) {
        const char *name = request->env[entry].name;
        const char *value = request->env[entry].value;
        size_t length = strlen(name) + strlen(value) + 2;
        char *joined = malloc(length);
        if (joined == NULL) {
            for (size_t done = 0; done < appended; done++) free(envp[count - 1 - done]);
            free(envp);
            *failed = 1;
            return NULL;
        }
        snprintf(joined, length, "%s=%s", name, value);
        envp[count++] = joined;
        appended++;
    }
    envp[count] = NULL;
    *owned = envp;
    return envp;
}

/* Frees only the joined strings this file allocated, never the entries
   borrowed from environ, which the process still owns. */
static void free_envp(char **owned, size_t appended) {
    if (owned == NULL) return;
    size_t total = 0;
    while (owned[total] != NULL) total++;
    for (size_t index = total - appended; index < total; index++) free(owned[index]);
    free(owned);
}

static void close_pipe(int pipe_fds[2]) {
    if (pipe_fds[0] >= 0) close(pipe_fds[0]);
    if (pipe_fds[1] >= 0) close(pipe_fds[1]);
}

typedef struct {
    int fd;
    char *text;
    size_t capacity;
    size_t length;
    int truncated;
    /* Set once, permanently, only when the very first allocation for this
       stream failed: there is no partial text to hand back, so the caller
       must see FR_ERR rather than a NULL string under FR_OK. */
    int out_of_memory;
    /* Set once a later growth failed with some text already captured: unlike
       out_of_memory this keeps what was read and is reported through
       truncated, the same as hitting FR_EXEC_CAPTURE_LIMIT. */
    int capped;
    int eof;
} fr_capture_stream;

static void capture_stream_init(fr_capture_stream *stream, int fd) {
    stream->fd = fd;
    stream->capacity = 4096;
    stream->length = 0;
    stream->truncated = 0;
    stream->out_of_memory = 0;
    stream->capped = 0;
    stream->eof = 0;
    stream->text = malloc(stream->capacity);
    if (stream->text == NULL) stream->out_of_memory = 1;
}

/* Reads once from a stream poll() has already reported ready. Never blocks:
   a single read() (retried only across EINTR) and back to the caller's poll
   loop, so a slow or stalled peer on the OTHER stream never delays this one. */
static void capture_stream_pump(fr_capture_stream *stream) {
    if (stream->eof) return;

    if (stream->out_of_memory || stream->capped || stream->length >= FR_EXEC_CAPTURE_LIMIT) {
        char sink[4096];
        ssize_t got;
        do {
            got = read(stream->fd, sink, sizeof sink);
        } while (got < 0 && errno == EINTR);
        if (got == 0) { stream->eof = 1; return; }
        if (got < 0) {
            stream->eof = 1;
            if (!stream->out_of_memory) stream->truncated = 1;
            return;
        }
        if (!stream->out_of_memory) stream->truncated = 1;
        return;
    }

    if (stream->length + 1 >= stream->capacity) {
        size_t grown = stream->capacity * 2;
        char *bigger = realloc(stream->text, grown);
        if (bigger == NULL) {
            stream->capped = 1;
            return;
        }
        stream->text = bigger;
        stream->capacity = grown;
    }

    /* Bounded to what is left under FR_EXEC_CAPTURE_LIMIT, not to the whole
       free buffer, so length never overshoots the documented 1 MiB bound. */
    size_t bound = FR_EXEC_CAPTURE_LIMIT - stream->length;
    size_t room = stream->capacity - stream->length - 1;
    size_t want = bound < room ? bound : room;

    ssize_t got;
    do {
        got = read(stream->fd, stream->text + stream->length, want);
    } while (got < 0 && errno == EINTR);

    if (got == 0) { stream->eof = 1; return; }
    if (got < 0) { stream->eof = 1; stream->truncated = 1; return; }
    stream->length += (size_t) got;
}

static char *capture_stream_finish(fr_capture_stream *stream, int *truncated) {
    if (stream->out_of_memory) {
        free(stream->text);
        return NULL;
    }
    stream->text[stream->length] = '\0';
    if (stream->truncated) *truncated = 1;
    return stream->text;
}

/* Drains both pipes concurrently via poll(), so a child that fills the
   stderr pipe while this were still waiting on stdout to reach EOF (or vice
   versa) can never wedge the parent: whichever fd has data is read, and the
   loop ends only once both have reported EOF. Returns false only when a
   stream's very first allocation failed, the one case the caller must turn
   into FR_ERR rather than a silently short FR_OK result. */
static int drain_both(int out_fd, int err_fd, fr_exec_result *out) {
    fr_capture_stream out_stream, err_stream;
    capture_stream_init(&out_stream, out_fd);
    capture_stream_init(&err_stream, err_fd);

    while (!out_stream.eof || !err_stream.eof) {
        struct pollfd fds[2];
        nfds_t count = 0;
        int out_index = -1, err_index = -1;
        if (!out_stream.eof) {
            out_index = (int) count;
            fds[count].fd = out_fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            count++;
        }
        if (!err_stream.eof) {
            err_index = (int) count;
            fds[count].fd = err_fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            count++;
        }

        int polled = poll(fds, count, -1);
        if (polled < 0) {
            if (errno == EINTR) continue;
            /* Cannot safely poll further; the fd close below unblocks a
               child mid-write with EPIPE instead of leaving it stuck. */
            if (!out_stream.eof) out_stream.truncated = 1;
            if (!err_stream.eof) err_stream.truncated = 1;
            break;
        }

        if (out_index >= 0 && fds[out_index].revents != 0) capture_stream_pump(&out_stream);
        if (err_index >= 0 && fds[err_index].revents != 0) capture_stream_pump(&err_stream);
    }

    int ok = !out_stream.out_of_memory && !err_stream.out_of_memory;
    out->stdout_text = capture_stream_finish(&out_stream, &out->truncated);
    out->stderr_text = capture_stream_finish(&err_stream, &out->truncated);
    return ok;
}

int fr_exec_run(const fr_exec_request *request, fr_exec_result *out, fr_error *err) {
    out->code = 0;
    out->stdout_text = NULL;
    out->stderr_text = NULL;
    out->truncated = 0;

    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (request->capture) {
        if (pipe(out_pipe) != 0) {
            fr_error_set(err, "\"%s\" could not be started: no pipe", request->program);
            return FR_ERR;
        }
        if (pipe(err_pipe) != 0) {
            close_pipe(out_pipe);
            fr_error_set(err, "\"%s\" could not be started: no pipe", request->program);
            return FR_ERR;
        }
    }

    char **argv = build_argv(request);
    if (argv == NULL) {
        if (request->capture) { close_pipe(out_pipe); close_pipe(err_pipe); }
        fr_error_set(err, "out of memory running \"%s\"", request->program);
        return FR_ERR;
    }

    char **owned_envp = NULL;
    int envp_failed = 0;
    char **envp = build_envp(request, &owned_envp, &envp_failed);
    if (envp_failed) {
        free(argv);
        if (request->capture) { close_pipe(out_pipe); close_pipe(err_pipe); }
        fr_error_set(err, "out of memory building the environment for \"%s\"", request->program);
        return FR_ERR;
    }

    pid_t child = fork();
    if (child < 0) {
        free(argv);
        free_envp(owned_envp, request->env_count);
        if (request->capture) { close_pipe(out_pipe); close_pipe(err_pipe); }
        fr_error_set(err, "\"%s\" could not be started: %s", request->program, strerror(errno));
        return FR_ERR;
    }

    if (child == 0) {
        if (request->cwd != NULL && chdir(request->cwd) != 0) _exit(127);
        if (request->capture) {
            if (dup2(out_pipe[1], 1) < 0 || dup2(err_pipe[1], 2) < 0) _exit(127);
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
        }
        execve(request->program, argv, envp);
        _exit(127);
    }

    free(argv);
    free_envp(owned_envp, request->env_count);

    int captured_ok = 1;
    if (request->capture) {
        close(out_pipe[1]);
        close(err_pipe[1]);
        captured_ok = drain_both(out_pipe[0], err_pipe[0], out);
        close(out_pipe[0]);
        close(err_pipe[0]);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        fr_exec_result_free(out);
        fr_error_set(err, "\"%s\" could not be waited for", request->program);
        return FR_ERR;
    }

    if (!captured_ok) {
        fr_exec_result_free(out);
        fr_error_set(err, "out of memory capturing \"%s\"", request->program);
        return FR_ERR;
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (code == 127) {
        /* The shared 127 contract; fr_exec_run's declaration says why. */
        fr_exec_result_free(out);
        fr_error_set(err, "\"%s\" could not be started", request->program);
        return FR_ERR;
    }
    out->code = code;
    return FR_OK;
}
