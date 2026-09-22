#include "exec.h"

#include "error.h"

#include <errno.h>
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

static char *read_all(int fd, int *truncated) {
    size_t capacity = 4096, length = 0;
    char *text = malloc(capacity);
    if (text == NULL) return NULL;
    for (;;) {
        if (length + 1 >= capacity) {
            size_t grown = capacity * 2;
            char *bigger = realloc(text, grown);
            if (bigger == NULL) break;
            text = bigger;
            capacity = grown;
        }
        ssize_t got = read(fd, text + length, capacity - length - 1);
        if (got <= 0) break;
        length += (size_t) got;
        if (length >= FR_EXEC_CAPTURE_LIMIT) {
            *truncated = 1;
            /* Keep draining so a child writing more than the bound is never
               killed by a full pipe; the bytes past the bound are discarded. */
            char sink[4096];
            while (read(fd, sink, sizeof sink) > 0) { }
            break;
        }
    }
    text[length] = '\0';
    return text;
}

int fr_exec_run(const fr_exec_request *request, fr_exec_result *out, fr_error *err) {
    out->code = 0;
    out->stdout_text = NULL;
    out->stderr_text = NULL;
    out->truncated = 0;

    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (request->capture && (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)) {
        fr_error_set(err, "\"%s\" could not be started: no pipe", request->program);
        return FR_ERR;
    }

    char **argv = build_argv(request);
    if (argv == NULL) {
        fr_error_set(err, "out of memory running \"%s\"", request->program);
        return FR_ERR;
    }

    pid_t child = fork();
    if (child < 0) {
        free(argv);
        fr_error_set(err, "\"%s\" could not be started: %s", request->program, strerror(errno));
        return FR_ERR;
    }

    if (child == 0) {
        if (request->cwd != NULL && chdir(request->cwd) != 0) _exit(127);
        if (request->capture) {
            dup2(out_pipe[1], 1);
            dup2(err_pipe[1], 2);
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
        }
        execv(request->program, argv);
        _exit(127);
    }

    free(argv);
    if (request->capture) {
        close(out_pipe[1]);
        close(err_pipe[1]);
        out->stdout_text = read_all(out_pipe[0], &out->truncated);
        out->stderr_text = read_all(err_pipe[0], &out->truncated);
        close(out_pipe[0]);
        close(err_pipe[0]);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        fr_exec_result_free(out);
        fr_error_set(err, "\"%s\" could not be waited for", request->program);
        return FR_ERR;
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (code == 127) {
        /* execv failed in the child, which cannot report through errno across
           the fork, so 127 is the only signal available. */
        fr_exec_result_free(out);
        fr_error_set(err, "\"%s\" could not be started", request->program);
        return FR_ERR;
    }
    out->code = code;
    return FR_OK;
}
