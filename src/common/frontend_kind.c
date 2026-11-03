#include "common/frontend_kind.h"

#include <stddef.h>  /* NULL: <strings.h> supplies it on macOS but not glibc */
#include <strings.h>

#include "core/globals.h"

/* On iOS each run is a thread of one process, and this decides how its VM
 * behaves as well as how it compiles. A new thread takes its creator's kind
 * (vprocPthreadCreateShim) and a VM worker takes its run's (ThreadJob). */
static PSCAL_THREAD_LOCAL FrontendKind gFrontendKind = FRONTEND_KIND_PASCAL;

FrontendKind frontendPushKind(FrontendKind kind) {
    FrontendKind previous = gFrontendKind;
    gFrontendKind = kind;
    return previous;
}

void frontendPopKind(FrontendKind previous) {
    gFrontendKind = previous;
}

FrontendKind frontendGetKind(void) {
    return gFrontendKind;
}

/* Kept beside the enum it mirrors: index == FrontendKind value, so adding a
 * frontend without adding its name here is an immediately visible NULL. */
static const char* const kFrontendKindNames[] = {
    "unknown", "pascal", "rea", "aether", "clike", "shell"
};

bool frontendKindIsValid(int value) {
    return value >= FRONTEND_KIND_UNKNOWN && value <= (int)FRONTEND_KIND_LAST;
}

const char* frontendKindName(FrontendKind kind) {
    if (!frontendKindIsValid((int)kind)) return NULL;
    return kFrontendKindNames[(int)kind];
}

bool frontendKindFromName(const char* name, FrontendKind* out) {
    if (!name || !*name) return false;
    for (int i = 0; i <= (int)FRONTEND_KIND_LAST; ++i) {
        if (strcasecmp(name, kFrontendKindNames[i]) == 0) {
            if (out) *out = (FrontendKind)i;
            return true;
        }
    }
    return false;
}
