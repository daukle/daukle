#include "exec.h"

#include "error.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

static char *read_all(HANDLE pipe, int *truncated) {
    size_t capacity = 4096, length = 0;
    char *text = malloc(capacity);
    if (text == NULL) return NULL;
    for (;;) {
        if (length + 1 >= capacity) {
            char *bigger = realloc(text, capacity * 2);
            if (bigger == NULL) break;
            text = bigger;
            capacity *= 2;
        }
        DWORD got = 0;
        if (!ReadFile(pipe, text + length, (DWORD) (capacity - length - 1), &got, NULL)
            || got == 0) {
            break;
        }
        length += got;
        if (length >= FR_EXEC_CAPTURE_LIMIT) {
            *truncated = 1;
            char sink[4096];
            DWORD drained = 0;
            while (ReadFile(pipe, sink, sizeof sink, &drained, NULL) && drained > 0) { }
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
        if (!CreatePipe(&out_read, &out_write, &inheritable, 0)
            || !CreatePipe(&err_read, &err_write, &inheritable, 0)) {
            free(line);
            fr_error_set(err, "\"%s\" could not be started: no pipe", request->program);
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
    free(line);
    if (!started) {
        if (out_read != NULL) { CloseHandle(out_read); CloseHandle(out_write); }
        if (err_read != NULL) { CloseHandle(err_read); CloseHandle(err_write); }
        fr_error_set(err, "\"%s\" could not be started", request->program);
        return FR_ERR;
    }

    if (request->capture) {
        CloseHandle(out_write);
        CloseHandle(err_write);
        out->stdout_text = read_all(out_read, &out->truncated);
        out->stderr_text = read_all(err_read, &out->truncated);
        CloseHandle(out_read);
        CloseHandle(err_read);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    out->code = (int) code;
    return FR_OK;
}
