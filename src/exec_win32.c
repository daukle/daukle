#include "exec.h"

#include "error.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    HANDLE pipe;
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

static void capture_stream_init(fr_capture_stream *stream, HANDLE pipe) {
    stream->pipe = pipe;
    stream->capacity = 4096;
    stream->length = 0;
    stream->truncated = 0;
    stream->out_of_memory = 0;
    stream->capped = 0;
    stream->eof = 0;
    stream->text = malloc(stream->capacity);
    if (stream->text == NULL) stream->out_of_memory = 1;
}

/* Peeks how much is waiting, then reads only that much, so this never blocks
   inside ReadFile. Returns 1 if it did anything observable (read bytes or
   reached EOF), 0 if the pipe was simply empty right now, so the caller
   knows whether to keep polling immediately or back off. */
static int capture_stream_pump(fr_capture_stream *stream) {
    if (stream->eof) return 0;

    DWORD available = 0;
    if (!PeekNamedPipe(stream->pipe, NULL, 0, NULL, &available, NULL)) {
        stream->eof = 1;
        if (GetLastError() != ERROR_BROKEN_PIPE && !stream->out_of_memory) stream->truncated = 1;
        return 1;
    }
    if (available == 0) return 0;

    if (stream->out_of_memory || stream->capped || stream->length >= FR_EXEC_CAPTURE_LIMIT) {
        char sink[4096];
        DWORD to_read = available < (DWORD) sizeof sink ? available : (DWORD) sizeof sink;
        DWORD got = 0;
        if (!ReadFile(stream->pipe, sink, to_read, &got, NULL)) {
            stream->eof = 1;
            if (GetLastError() != ERROR_BROKEN_PIPE && !stream->out_of_memory) stream->truncated = 1;
            return 1;
        }
        if (got == 0) { stream->eof = 1; return 1; }
        if (!stream->out_of_memory) stream->truncated = 1;
        return 1;
    }

    if (stream->length + 1 >= stream->capacity) {
        size_t grown = stream->capacity * 2;
        char *bigger = realloc(stream->text, grown);
        if (bigger == NULL) {
            stream->capped = 1;
            return 1;
        }
        stream->text = bigger;
        stream->capacity = grown;
    }

    /* Bounded to what is left under FR_EXEC_CAPTURE_LIMIT and to what Peek
       reported waiting, so length never overshoots the documented 1 MiB
       bound and ReadFile never blocks past what is actually available. */
    size_t bound = FR_EXEC_CAPTURE_LIMIT - stream->length;
    size_t room = stream->capacity - stream->length - 1;
    size_t want_size = bound < room ? bound : room;
    DWORD want = (DWORD) (want_size < available ? want_size : available);

    DWORD got = 0;
    if (!ReadFile(stream->pipe, stream->text + stream->length, want, &got, NULL)) {
        stream->eof = 1;
        if (GetLastError() != ERROR_BROKEN_PIPE) stream->truncated = 1;
        return 1;
    }
    if (got == 0) { stream->eof = 1; return 1; }
    stream->length += got;
    return 1;
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

/* Anonymous pipes cannot be waited on together the way poll() waits on fds,
   so both handles are peeked and read in turn instead of stdout being fully
   drained before stderr is even looked at: a child that fills the stderr
   pipe while this were still blocked reading stdout to EOF (or vice versa)
   would otherwise wedge forever. Sleep(1) only when neither had anything
   waiting, so the loop does not spin. Returns false only when a stream's
   very first allocation failed, the one case the caller must turn into
   FR_ERR rather than a silently short FR_OK result. */
static int drain_both(HANDLE out_pipe, HANDLE err_pipe, fr_exec_result *out) {
    fr_capture_stream out_stream, err_stream;
    capture_stream_init(&out_stream, out_pipe);
    capture_stream_init(&err_stream, err_pipe);

    while (!out_stream.eof || !err_stream.eof) {
        int progressed = 0;
        if (!out_stream.eof) progressed |= capture_stream_pump(&out_stream);
        if (!err_stream.eof) progressed |= capture_stream_pump(&err_stream);
        if (!progressed) Sleep(1);
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

    char *line = NULL;
    if (fr_exec_command_line(request->program, request->argv, request->argv_count, &line, err)
        != FR_OK) {
        return FR_ERR;
    }

    SECURITY_ATTRIBUTES inheritable;
    inheritable.nLength = sizeof inheritable;
    inheritable.lpSecurityDescriptor = NULL;
    inheritable.bInheritHandle = TRUE;

    HANDLE out_read = NULL, out_write = NULL, err_read = NULL, err_write = NULL;
    if (request->capture) {
        if (!CreatePipe(&out_read, &out_write, &inheritable, 0)) {
            DWORD gle = GetLastError();
            free(line);
            fr_error_set(err, "\"%s\" could not be started: no pipe (error %lu)", request->program,
                        (unsigned long) gle);
            return FR_ERR;
        }
        if (!CreatePipe(&err_read, &err_write, &inheritable, 0)) {
            DWORD gle = GetLastError();
            CloseHandle(out_read);
            CloseHandle(out_write);
            free(line);
            fr_error_set(err, "\"%s\" could not be started: no pipe (error %lu)", request->program,
                        (unsigned long) gle);
            return FR_ERR;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA startup;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    if (request->capture) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = out_write;
        startup.hStdError = err_write;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof process);
    BOOL started = CreateProcessA(request->program, line, NULL, NULL, request->capture ? TRUE : FALSE,
                                  0, NULL, request->cwd, &startup, &process);
    DWORD start_error = started ? 0 : GetLastError();
    free(line);
    if (!started) {
        if (out_read != NULL) { CloseHandle(out_read); CloseHandle(out_write); }
        if (err_read != NULL) { CloseHandle(err_read); CloseHandle(err_write); }
        fr_error_set(err, "\"%s\" could not be started: error %lu", request->program,
                    (unsigned long) start_error);
        return FR_ERR;
    }

    int captured_ok = 1;
    if (request->capture) {
        CloseHandle(out_write);
        CloseHandle(err_write);
        captured_ok = drain_both(out_read, err_read, out);
        CloseHandle(out_read);
        CloseHandle(err_read);
    }

    DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    BOOL got_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    if (!captured_ok) {
        fr_exec_result_free(out);
        fr_error_set(err, "out of memory capturing \"%s\"", request->program);
        return FR_ERR;
    }

    if (waited != WAIT_OBJECT_0 || !got_code) {
        fr_exec_result_free(out);
        fr_error_set(err, "\"%s\" could not be waited for", request->program);
        return FR_ERR;
    }

    /* A DWORD exit code above INT_MAX, such as an exception code like
       0xC0000005, is kept bit-identical as a negative int rather than
       clamped or silently reduced, so a caller comparing against a known
       negative constant still matches. */
    out->code = (int) code;
    return FR_OK;
}
