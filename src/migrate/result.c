#include "migrate/result.h"

#include "util/error.h"

#include <stdlib.h>
#include <string.h>

static char *duplicate_text(const char *text) {
    if (text == NULL) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static void blocker_release(fr_blocker *blocker) {
    free(blocker->construct);
    free(blocker->where);
    free(blocker->why);
    free(blocker->surface);
    free(blocker->source);
    memset(blocker, 0, sizeof *blocker);
}

int fr_blockers_add(fr_blockers *blockers, const char *source, const char *construct,
                    const char *where, const char *why, const char *surface, fr_error *err) {
    fr_blocker *grown = realloc(blockers->items, (blockers->count + 1) * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory recording a blocker reported by %s", source);
        return FR_ERR;
    }
    blockers->items = grown;

    fr_blocker blocker = { duplicate_text(construct), duplicate_text(where), duplicate_text(why),
                           duplicate_text(surface), duplicate_text(source) };
    int complete = blocker.construct != NULL && blocker.where != NULL && blocker.why != NULL
                   && blocker.source != NULL && (surface == NULL || blocker.surface != NULL);
    if (!complete) {
        blocker_release(&blocker);
        fr_error_set(err, "out of memory recording a blocker reported by %s", source);
        return FR_ERR;
    }
    blockers->items[blockers->count++] = blocker;
    return FR_OK;
}

void fr_blockers_free(fr_blockers *blockers) {
    for (size_t index = 0; index < blockers->count; index++) {
        blocker_release(&blockers->items[index]);
    }
    free(blockers->items);
    blockers->items = NULL;
    blockers->count = 0;
}

static int source_printed_before(const fr_blockers *blockers, size_t index) {
    for (size_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(blockers->items[earlier].source, blockers->items[index].source) == 0) return 1;
    }
    return 0;
}

static void print_blocker(const fr_blocker *blocker, FILE *out) {
    fprintf(out, "    %s: %s: %s (surface: %s)\n", blocker->where, blocker->construct, blocker->why,
            blocker->surface != NULL ? blocker->surface : "no row yet");
}

void fr_blockers_print(const fr_blockers *blockers, FILE *out) {
    fprintf(out, "daukle migrate: nothing was written, because %zu thing(s) cannot be expressed yet:\n",
            blockers->count);
    for (size_t index = 0; index < blockers->count; index++) {
        if (source_printed_before(blockers, index)) continue;
        const char *source = blockers->items[index].source;
        fprintf(out, "  %s:\n", source);
        for (size_t member = index; member < blockers->count; member++) {
            if (strcmp(blockers->items[member].source, source) == 0) {
                print_blocker(&blockers->items[member], out);
            }
        }
    }
}
