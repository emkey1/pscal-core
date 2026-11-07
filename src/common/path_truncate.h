#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns true when PATH_TRUNCATE is set to a non-empty absolute prefix.
 */
bool pathTruncateEnabled(void);

/**
 * Returns true when `prefix` names the real root, however it is spelled: "/",
 * "//", "/./", "/usr/.." or a symlink to it. The root is never provisioned.
 */
bool pathTruncatePrefixIsRoot(const char *prefix);

/**
 * Removes the PATH_TRUNCATE prefix from `absolute_path` and writes the user-facing
 * representation into `out`. When the prefix does not match or PATH_TRUNCATE is
 * disabled the original string is copied verbatim.
 */
bool pathTruncateStrip(const char *absolute_path, char *out, size_t out_size);

/**
 * Expands user supplied paths into the filesystem path by re-applying the
 * PATH_TRUNCATE prefix when necessary. Relative paths and absolute paths that
 * already contain the full prefix are returned unchanged.
 */
bool pathTruncateExpand(const char *input_path, char *out, size_t out_size);

typedef struct PathTruncateMountEntry {
    char source[PATH_MAX];
    char target[PATH_MAX];
    char type[32];
    char options[128];
    unsigned long flags;
} PathTruncateMountEntry;

/*
 * Adds or replaces a virtual mount mapping used by iOS path truncation.
 * `source_path` is resolved to a host path, while `target_path` is a virtual
 * absolute path (for example "/mnt/docs").
 */
bool pathTruncateMountAdd(const char *source_path,
                          const char *target_path,
                          const char *type,
                          const char *options,
                          unsigned long flags);

/*
 * Removes a virtual mount mapping identified by `target_path`.
 */
bool pathTruncateMountRemove(const char *target_path);

/*
 * Copies the current mount table into `out` and returns the total number of
 * active mount entries.
 */
size_t pathTruncateMountSnapshot(PathTruncateMountEntry *out, size_t out_capacity);

/*
 * The mount table the app's processes see, in /proc/mounts order: the root
 * filesystem, /proc, then each virtual mount, with sources and types in the
 * app's own terms (no host path, volume or filesystem name). /proc/mounts,
 * mount and the statfs in path_virtualization.h all read it. Copies up to
 * `out_capacity` entries and returns how many there are.
 */
size_t pathTruncateMountTable(PathTruncateMountEntry *out, size_t out_capacity);

/*
 * The entry of that table whose filesystem holds `host_path` (an expanded
 * path): the longest mount point over the path's virtual form. Every path is
 * on some entry, the root at least.
 */
bool pathTruncateMountForHostPath(const char *host_path, PathTruncateMountEntry *out);

/* Clears all virtual mount mappings. */
void pathTruncateMountClearAll(void);

/**
 * Applies the PATH_TRUNCATE environment variable using the provided prefix.
 * Passing NULL clears PATH_TRUNCATE entirely. The real root is not a prefix:
 * $HOME stands in for it, or nothing when $HOME is the root too. Internal caches
 * are flushed so subsequent path lookups observe the new value immediately.
 */
void pathTruncateApplyEnvironment(const char *prefix);

/* Ensures the emulated /dev directory under the truncated prefix exists and
 * populates symlinks for /dev/null and /dev/zero that point to the real
 * system devices. */
void pathTruncateProvisionDev(const char *prefix);

/* Ensures a minimal /proc tree under the truncated prefix and generates
 * a lightweight cpuinfo file for compatibility. */
void pathTruncateProvisionProc(const char *prefix);

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
/* While set, provisioning passes its prefix to the hook and touches nothing. */
void pathTruncateSetProvisionHookForTests(void (*hook)(const char *prefix));
#endif

#ifdef __cplusplus
}
#endif
