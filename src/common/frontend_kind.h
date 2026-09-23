#pragma once

#include <stdbool.h>

/* These numeric values are ON-DISK FORMAT: they are what the PSB3 container
 * header's frontend field carries (Docs/pscal_vm_manual/pscal_vm_manual_ch2.md
 * §2.1, core/cache.c), which is how a bare `pscalvm chunk.bc` learns which
 * frontend's conventions the chunk was compiled under. Never renumber an
 * existing entry; only append, and bump PSB3_FORMAT_VERSION when you do.
 * 0 stays UNKNOWN so a producer that doesn't track the axis at all (a
 * hand-written .asm, tools/tiny) writes a zero field and gets the
 * Pascal-compatible defaults every predicate below already gives UNKNOWN. */
typedef enum FrontendKind {
    FRONTEND_KIND_UNKNOWN = 0,
    FRONTEND_KIND_PASCAL  = 1,
    FRONTEND_KIND_REA     = 2,
    FRONTEND_KIND_AETHER  = 3,
    FRONTEND_KIND_CLIKE   = 4,
    FRONTEND_KIND_SHELL   = 5
} FrontendKind;

/* Highest value frontendKindIsValid() accepts. A #define rather than an extra
 * enumerator so adding it can never make a switch over FrontendKind warn. */
#define FRONTEND_KIND_LAST FRONTEND_KIND_SHELL

FrontendKind frontendPushKind(FrontendKind kind);
void frontendPopKind(FrontendKind previous);
FrontendKind frontendGetKind(void);

/* Is `value` one of the codes above? Used where a FrontendKind arrives from
 * outside the process (a .bc header, a pscalasm `frontend` directive) and so
 * cannot be trusted to be in range. */
bool frontendKindIsValid(int value);

/* The lowercase spelling of a kind ("aether", "clike", ...), or NULL if it is
 * out of range. UNKNOWN spells "unknown". These names are the pscalasm
 * `frontend` directive's vocabulary, so they are format too. */
const char* frontendKindName(FrontendKind kind);

/* Inverse of frontendKindName(), case-insensitively. Returns false (leaving
 * *out untouched) for a name no frontend answers to. */
bool frontendKindFromName(const char* name, FrontendKind* out);

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
