#pragma once

#include <stdbool.h>

typedef enum FrontendKind {
    FRONTEND_KIND_UNKNOWN = 0,
    FRONTEND_KIND_PASCAL,
    FRONTEND_KIND_REA,
    FRONTEND_KIND_AETHER,
    FRONTEND_KIND_CLIKE,
    FRONTEND_KIND_SHELL
} FrontendKind;

FrontendKind frontendPushKind(FrontendKind kind);
void frontendPopKind(FrontendKind previous);
FrontendKind frontendGetKind(void);

static inline bool frontendIsPascal(void) {
    FrontendKind kind = frontendGetKind();
    return kind == FRONTEND_KIND_PASCAL || kind == FRONTEND_KIND_UNKNOWN;
}

static inline bool frontendIsRea(void) {
    return frontendGetKind() == FRONTEND_KIND_REA;
}

static inline bool frontendIsAether(void) {
    return frontendGetKind() == FRONTEND_KIND_AETHER;
}

static inline bool frontendIsClike(void) {
    return frontendGetKind() == FRONTEND_KIND_CLIKE;
}

static inline bool frontendIsShell(void) {
    return frontendGetKind() == FRONTEND_KIND_SHELL;
}

/* Does this frontend index strings from 0?
 *
 * Pascal (and rea/clike, which inherit its string semantics) index from 1:
 * `s[1]` is the first character, `copy`'s `start` is 1-based, and `pos`
 * returns a 1-based index with 0 meaning "absent". Shell has always indexed
 * from 0, and Aether joined it -- Aether's arrays are 0-based half-open, so a
 * 1-based Text made the same loop idiom mean two different things, silently
 * dropping the last character in `loop i in 1..length(s)`.
 *
 * This is the single place that policy lives; every string-index decision in
 * the VM and the copy/pos builtins keys off this rather than naming frontends
 * individually. Note it also governs whether the Pascal `s[0]`-is-the-length
 * sentinel applies: a 0-based frontend has no room for it, because index 0 is
 * an ordinary character there. */
static inline bool frontendIsZeroBasedStrings(void) {
    FrontendKind kind = frontendGetKind();
    return kind == FRONTEND_KIND_SHELL || kind == FRONTEND_KIND_AETHER;
}
