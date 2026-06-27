/* Weak NULL fallback for the optional `smallclueGetApplets` hook referenced by
 * path_truncate.c. clike's "smallclue" provides the real applet table in the
 * umbrella build; the standalone aether(+SDL) build has no applets. Without a
 * definition the macOS linker (with the SDL frameworks pulled into the link)
 * fails to resolve the weak_import reference instead of binding it to NULL, so
 * provide a weak NULL stub here. A real definition, if ever linked in, overrides
 * this one. Only compiled into the optional SDL build (PSCAL_SDL). */
#include <stddef.h>

typedef struct PathTruncateSmallclueApplet {
    const char *name;
    int (*entry)(int argc, char **argv);
    const char *description;
} PathTruncateSmallclueApplet;

__attribute__((weak))
const PathTruncateSmallclueApplet *smallclueGetApplets(size_t *count) {
    if (count) *count = 0;
    return NULL;
}
