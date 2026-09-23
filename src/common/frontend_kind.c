#include "common/frontend_kind.h"

#include <strings.h>

static FrontendKind gFrontendKind = FRONTEND_KIND_PASCAL;

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
