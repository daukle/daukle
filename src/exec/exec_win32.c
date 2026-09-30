#include "exec/exec.h"

#include "util/error.h"

#include <windows.h>

#include <stdio.h>
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

static int entry_name_matches(const char *entry, const char *name) {
    size_t length = strlen(name);
    /* Windows environment variable names are case insensitive, so an override
       of "path" must replace "Path" rather than sit beside it and lose. */
    return _strnicmp(entry, name, length) == 0 && entry[length] == '=';
}

static int compare_block_entries(const void *left, const void *right) {
    return _stricmp(*(const char *const *) left, *(const char *const *) right);
}

/* CreateProcess wants one block of NUL-separated "NAME=VALUE" strings ended by
   a second NUL, sorted case insensitively. Built rather than applied with
   SetEnvironmentVariable around the call, because daukle is a plugin API as
   well as a command and a library may not mutate its host's environment, even
   briefly. NULL out means "no additions": the caller then passes NULL to
   CreateProcess and the child inherits ours wholesale. */
static char *build_environment_block(const fr_exec_request *request) {
    if (request->env_count == 0) return NULL;

    char *inherited = GetEnvironmentStringsA();
    if (inherited == NULL) return NULL;

    size_t inherited_count = 0;
    for (const char *scan = inherited; *scan != '\0'; scan += strlen(scan) + 1) inherited_count++;

    const char **entries = malloc((inherited_count + request->env_count) * sizeof *entries);
    char **joined = malloc(request->env_count * sizeof *joined);
    if (entries == NULL || joined == NULL) {
        free(entries); free(joined);
        FreeEnvironmentStringsA(inherited);
        return NULL;
    }

    size_t count = 0;
    for (const char *scan = inherited; *scan != '\0'; scan += strlen(scan) + 1) {
        int overridden = 0;
        for (size_t entry = 0; entry < request->env_count && !overridden; entry++) {
            overridden = entry_name_matches(scan, request->env[entry].name);
        }
        /* A block from GetEnvironmentStrings can open with "=C:=C:\..." drive
           entries whose name is empty; they are kept as they arrived. */
        if (!overridden) entries[count++] = scan;
    }

    size_t made = 0;
    for (size_t entry = 0; entry < request->env_count; entry++) {
        const char *name = request->env[entry].name;
        const char *value = request->env[entry].value;
        size_t length = strlen(name) + strlen(value) + 2;
        char *text = malloc(length);
        if (text == NULL) break;
        snprintf(text, length, "%s=%s", name, value);
        joined[made++] = text;
        entries[count++] = text;
    }

    char *block = NULL;
    if (made == request->env_count) {
        qsort(entries, count, sizeof *entries, compare_block_entries);
        size_t total = 1;
        for (size_t index = 0; index < count; index++) total += strlen(entries[index]) + 1;
        block = malloc(total);
        if (block != NULL) {
            size_t offset = 0;
            for (size_t index = 0; index < count; index++) {
                size_t length = strlen(entries[index]) + 1;
                memcpy(block + offset, entries[index], length);
                offset += length;
            }
            block[offset] = '\0';
        }
    }

    for (size_t index = 0; index < made; index++) free(joined[index]);
    free(joined);
    free(entries);
    FreeEnvironmentStringsA(inherited);
    return block;
}

/* Windows raises a hard-error box when the program is not a valid image, and a
   session with no desktop has nobody to dismiss it, so CreateProcess never
   returns there while it returns error 193 on a developer's machine. The scope
   is the thread rather than the process because daukle is a plugin API as well
   as a command, and a library may not leave a host's error mode rewritten. */
static BOOL start_process(const fr_exec_request *request, char *line, STARTUPINFOA *startup,
                          PROCESS_INFORMATION *process, DWORD *start_error, char *block) {
    DWORD previous = 0;
    BOOL scoped = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous);
    BOOL started = CreateProcessA(request->program, line, NULL, NULL,
                                  request->capture ? TRUE : FALSE, 0, block, request->cwd, startup,
                                  process);
    *start_error = started ? 0 : GetLastError();
    if (scoped) SetThreadErrorMode(previous, NULL);
    return started;
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
    DWORD start_error = 0;
    char *block = build_environment_block(request);
    if (request->env_count > 0 && block == NULL) {
        free(line);
        if (out_read != NULL) { CloseHandle(out_read); CloseHandle(out_write); }
        if (err_read != NULL) { CloseHandle(err_read); CloseHandle(err_write); }
        fr_error_set(err, "out of memory building the environment for \"%s\"", request->program);
        return FR_ERR;
    }
    BOOL started = start_process(request, line, &startup, &process, &start_error, block);
    free(line);
    free(block);
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
    if (out->code == 127) {
        fr_exec_result_free(out);
        fr_error_set(err, "\"%s\" could not be started", request->program);
        return FR_ERR;
    }
    return FR_OK;
}
