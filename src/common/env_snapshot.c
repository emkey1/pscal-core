#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "common/env_snapshot.h"

#include <stdlib.h>
#include <string.h>

#if defined(PSCAL_TARGET_IOS)
#include "runtime/vproc/vproc.h"
#endif

extern char **environ;

char **pscalEnvironWalkBegin(void **walk) {
#if defined(PSCAL_TARGET_IOS)
    VProcEnv *copy = vprocEnvCapture();
    *walk = copy;
    return vprocEnvEntries(copy);
#else
    size_t count = 0;
    while (environ && environ[count]) {
        count++;
    }
    char **copy = (char **)calloc(count + 1, sizeof(char *));
    for (size_t i = 0; copy && i < count; ++i) {
        copy[i] = strdup(environ[i]);
        if (!copy[i]) {
            break;
        }
    }
    *walk = copy;
    return copy;
#endif
}

void pscalEnvironWalkEnd(void *walk) {
#if defined(PSCAL_TARGET_IOS)
    if (walk) {
        vprocEnvRelease((VProcEnv *)walk);
    }
#else
    char **copy = (char **)walk;
    for (size_t i = 0; copy && copy[i]; ++i) {
        free(copy[i]);
    }
    free(copy);
#endif
}

/* NAME_LEN counts the '=' as well. An entry usually stays where it was, so HINT is tried first. */
static const char *pscalEnvEntryFind(char **entries, size_t count, size_t hint, const char *entry,
                                     size_t name_len) {
    if (hint < count && entries[hint] && strncmp(entries[hint], entry, name_len) == 0) {
        return entries[hint];
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i] && strncmp(entries[i], entry, name_len) == 0) {
            return entries[i];
        }
    }
    return NULL;
}

void pscalEnvSnapshotClear(PscalEnvSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    if (snapshot->entries) {
        for (size_t i = 0; i < snapshot->count; ++i) {
            free(snapshot->entries[i]);
        }
        free(snapshot->entries);
    }
    snapshot->entries = NULL;
    snapshot->count = 0;
}

bool pscalEnvSnapshotTake(PscalEnvSnapshot *snapshot) {
    if (!snapshot) {
        return false;
    }

    pscalEnvSnapshotClear(snapshot);

    void *walk = NULL;
    char **entries = pscalEnvironWalkBegin(&walk);
    size_t count = 0;
    for (char **cursor = entries; cursor && *cursor; ++cursor) {
        count++;
    }

    char **copy = (char **)calloc(count + 1, sizeof(char *));
    if (!copy) {
        pscalEnvironWalkEnd(walk);
        return false;
    }

    size_t written = 0;
    for (char **cursor = entries; cursor && *cursor; ++cursor) {
        copy[written] = strdup(*cursor);
        if (!copy[written]) {
            snapshot->entries = copy;
            snapshot->count = written;
            pscalEnvSnapshotClear(snapshot);
            pscalEnvironWalkEnd(walk);
            return false;
        }
        written++;
    }
    pscalEnvironWalkEnd(walk);

    copy[written] = NULL;
    snapshot->entries = copy;
    snapshot->count = written;
    return true;
}

/* Only what changed. Every $(...) comes through here, and libc's environment is the whole app's on
 * iOS: unsetting all of it and setting it back left other tabs a moment with no PATH or HOME. */
bool pscalEnvSnapshotRestore(PscalEnvSnapshot *snapshot) {
    if (!snapshot || !snapshot->entries) {
        return true;
    }

    bool ok = true;
    void *walk = NULL;
    char **entries = pscalEnvironWalkBegin(&walk);
    size_t count = 0;
    while (entries && entries[count]) {
        count++;
    }

    for (size_t i = 0; i < count; ++i) {
        const char *entry = entries[i];
        const char *eq = strchr(entry, '=');
        if (!eq || entry == eq ||
            pscalEnvEntryFind(snapshot->entries, snapshot->count, i, entry, (size_t)(eq - entry) + 1)) {
            continue;
        }
        char *name = strndup(entry, (size_t)(eq - entry));
        if (!name || unsetenv(name) != 0) {
            ok = false;
        }
        free(name);
    }

    for (size_t i = 0; i < snapshot->count; ++i) {
        const char *entry = snapshot->entries[i];
        const char *eq = entry ? strchr(entry, '=') : NULL;
        if (!eq || entry == eq) {
            ok = false;
            continue;
        }
        const char *current = pscalEnvEntryFind(entries, count, i, entry, (size_t)(eq - entry) + 1);
        if (current && strcmp(current, entry) == 0) {
            continue;
        }
        char *name = strndup(entry, (size_t)(eq - entry));
        if (!name || setenv(name, eq + 1, 1) != 0) {
            ok = false;
        }
        free(name);
    }
    pscalEnvironWalkEnd(walk);

    pscalEnvSnapshotClear(snapshot);
    return ok;
}
