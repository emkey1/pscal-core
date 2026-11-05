#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **entries;
    size_t count;
} PscalEnvSnapshot;

/* A copy of the environment to walk, NULL-terminated, so whoever walks it can set and unset as it
 * goes. On iOS libc's is the whole app's and another tab can change it, so there the copy is taken
 * under the lock its writers hold, and nothing is locked after that. */
char **pscalEnvironWalkBegin(void **walk);
void pscalEnvironWalkEnd(void *walk);

bool pscalEnvSnapshotTake(PscalEnvSnapshot *snapshot);
bool pscalEnvSnapshotRestore(PscalEnvSnapshot *snapshot);
void pscalEnvSnapshotClear(PscalEnvSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
