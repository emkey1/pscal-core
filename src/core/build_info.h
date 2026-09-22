#ifndef PSCAL_BUILD_INFO_H
#define PSCAL_BUILD_INFO_H

#include <stddef.h>
#include <string.h>

#define PSCAL_STRINGIFY_IMPL(x) #x
#define PSCAL_STRINGIFY(x) PSCAL_STRINGIFY_IMPL(x)

// PSCAL_PROGRAM_VERSION_OVERRIDE lets a frontend that tracks its own
// meaningful version scheme (e.g. Aether's language-version VERSION file)
// take priority over the generic build-timestamp PROGRAM_VERSION that the
// umbrella build defines at directory scope for every frontend. It has to be
// a distinct macro name, not a redefinition of PROGRAM_VERSION itself: umbrella
// builds set PROGRAM_VERSION as a directory-wide compile definition, which in
// CMake's compile-definition ordering always wins over a same-named target-level
// override added later in the same directory scope, so a frontend-specific
// target_compile_definitions(... PROGRAM_VERSION=...) silently loses.
#ifdef PSCAL_PROGRAM_VERSION_OVERRIDE
#define PSCAL_PROGRAM_VERSION_RAW PSCAL_STRINGIFY(PSCAL_PROGRAM_VERSION_OVERRIDE)
#elif defined(PROGRAM_VERSION)
#define PSCAL_PROGRAM_VERSION_RAW PSCAL_STRINGIFY(PROGRAM_VERSION)
#else
#define PSCAL_PROGRAM_VERSION_RAW PSCAL_STRINGIFY("undefined.version_DEV")
#endif

// PSCAL_BUILD_COMMIT_HEADER names a header the umbrella build regenerates on
// every build, defining PSCAL_BUILD_COMMIT as the frontend component's short
// commit ("-dirty" with uncommitted changes). Only frontend targets set it, so
// a new commit recompiles the files that print the version, not the tree --
// the reason the wall-clock stamp above is fixed per build directory.
#ifdef PSCAL_BUILD_COMMIT_HEADER
#include PSCAL_BUILD_COMMIT_HEADER
#endif

#ifdef PSCAL_GIT_TAG
#define PSCAL_GIT_TAG_RAW PSCAL_STRINGIFY(PSCAL_GIT_TAG)
#else
#define PSCAL_GIT_TAG_RAW PSCAL_STRINGIFY("untagged")
#endif

static inline const char *pscal_normalize_define(const char *raw, char *buffer, size_t buffer_len) {
    if (!raw) {
        return "";
    }

    size_t len = strlen(raw);
    if (len >= 2 && raw[0] == '"' && raw[len - 1] == '"') {
        if (!buffer || buffer_len == 0) {
            return "";
        }

        size_t copy_len = len - 2;
        if (copy_len >= buffer_len) {
            copy_len = buffer_len - 1;
        }

        memcpy(buffer, raw + 1, copy_len);
        buffer[copy_len] = '\0';
        return buffer;
    }

    return raw;
}

static inline const char *pscal_program_version_string(void) {
    static int initialized = 0;
#ifdef PSCAL_BUILD_COMMIT
    static char storage[sizeof(PSCAL_PROGRAM_VERSION_RAW) + sizeof(PSCAL_BUILD_COMMIT) + 1];
#else
    static char storage[sizeof(PSCAL_PROGRAM_VERSION_RAW)];
#endif
    static const char *value = NULL;

    if (!initialized) {
#ifdef PSCAL_BUILD_COMMIT
        // "<version>+<commit>": VERSION names the semantics, the commit the build.
        char base[sizeof(PSCAL_PROGRAM_VERSION_RAW)];
        const char *version = pscal_normalize_define(PSCAL_PROGRAM_VERSION_RAW, base, sizeof(base));
        size_t version_len = strlen(version);
        memcpy(storage, version, version_len);
        storage[version_len] = '+';
        memcpy(storage + version_len + 1, PSCAL_BUILD_COMMIT, sizeof(PSCAL_BUILD_COMMIT));
        value = storage;
#else
        value = pscal_normalize_define(PSCAL_PROGRAM_VERSION_RAW, storage, sizeof(storage));
#endif
        initialized = 1;
    }

    return value;
}

static inline const char *pscal_git_tag_string(void) {
    static int initialized = 0;
    static char storage[sizeof(PSCAL_GIT_TAG_RAW)];
    static const char *value = NULL;

    if (!initialized) {
        value = pscal_normalize_define(PSCAL_GIT_TAG_RAW, storage, sizeof(storage));
        initialized = 1;
    }

    return value;
}

#endif // PSCAL_BUILD_INFO_H
