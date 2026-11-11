#include "runtime/vproc/vproc.h"

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h> // for vsnprintf
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <termios.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include <poll.h>
#include <dlfcn.h>
#undef umask
#undef mkfifo
#undef mknod
#include <pwd.h>
#include <grp.h>
#include <libgen.h>
#include <locale.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <sys/xattr.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <xlocale.h>
#include <sys/syscall.h>
#include <sys/event.h>
#include <os/log.h>
#endif
#include "common/path_truncate.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#endif
#include "common/frontend_kind.h"
#include "common/runtime_tty.h"
#include "runtime/vproc/tty/pscal_compat.h"
#include "runtime/vproc/tty/pscal_pty.h"
#include "runtime/vproc/tty/pscal_tty_host.h"

typedef struct PSCALRuntimeContext PSCALRuntimeContext;
typedef enum {
    VPROC_RESOURCE_GENERIC = 0,
    VPROC_RESOURCE_SOCKET = 1,
    VPROC_RESOURCE_PIPE = 2,
} VProcResourceKind;

static void vprocResourceTrack(VProc *vp, int host_fd, VProcResourceKind kind);
static void vprocResourceRemove(VProc *vp, int host_fd);

#if defined(PSCAL_TARGET_IOS)
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#undef PATH_VIRTUALIZATION_NO_MACROS
#endif

#if defined(PSCAL_TARGET_IOS)
__attribute__((weak)) void pscalRuntimeRequestSigint(void);
/* The runtime's (builtin.c) replaces this; a binary without a VM has no flag to raise. */
__attribute__((weak)) void pscalRuntimeNoteSigint(void) {
}
__attribute__((weak)) void pscalRuntimeRequestSigtstp(void);
__attribute__((weak)) void pscalRuntimeDebugLog(const char *message);
__attribute__((weak)) PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void);
__attribute__((weak)) void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx);
__attribute__((weak)) int pscalRuntimeCurrentForegroundPgid(void);
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void __attribute__((weak)) pscalRuntimeRequestSigtstp(void) {
}
#endif
#endif

#if defined(__APPLE__)
extern ssize_t __read_nocancel(int, void *, size_t) __attribute__((weak_import));
extern ssize_t __write_nocancel(int, const void *, size_t) __attribute__((weak_import));
extern int __close_nocancel(int) __attribute__((weak_import));
/* The kernel's stubs, not libc's open: not variadic, and they read the mode from a register. Called
 * as variadic, arm64 puts it on the stack, and a new file gets whatever the register holds. */
extern int __open(const char *, int, int) __attribute__((weak_import));
extern int __open_nocancel(const char *, int, int) __attribute__((weak_import));
#endif

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)
#define VPROC_UNUSED __attribute__((unused))
static const void *vprocSelfBase(void) {
    static const void *self_base = NULL;
    if (!self_base) {
        Dl_info info;
        if (dladdr((void *)&vprocSelfBase, &info) != 0) {
            self_base = info.dli_fbase;
        }
    }
    return self_base;
}

/* ----------------------------------------------------------------------
 * passwd/group overrides for iOS container
 * ---------------------------------------------------------------------- */
#if defined(PSCAL_TARGET_IOS)
typedef struct {
    struct passwd pw;
    char *name;
    char *passwd;
    char *gecos;
    char *dir;
    char *shell;
} PscalPasswdEntry;

typedef struct {
    struct group gr;
    char *name;
    char *passwd;
    gid_t *members_gids; /* unused; placeholder */
} PscalGroupEntry;

/* Every thread of the app reads these, and what getpwuid returned is still in a caller's hands when
 * the file changes. So a table is never changed or freed once installed: a reload installs a new one
 * under the lock, and the old one stays for whoever holds an answer from it, at the cost of one table
 * per edit of the file. A file is read outside the lock, since it could block. */
static pthread_mutex_t gPscalUserDbLock = PTHREAD_MUTEX_INITIALIZER;

static PscalPasswdEntry *gPscalPasswd = NULL;
static size_t gPscalPasswdCount = 0;
static bool gPscalPasswdLoaded = false;
static char gPscalPasswdPath[PATH_MAX] = {0};
static dev_t gPscalPasswdDev = 0;
static ino_t gPscalPasswdIno = 0;
static time_t gPscalPasswdMtime = 0;
static off_t gPscalPasswdSize = 0;

static PscalGroupEntry *gPscalGroup = NULL;
static size_t gPscalGroupCount = 0;
static bool gPscalGroupLoaded = false;
static char gPscalGroupPath[PATH_MAX] = {0};
static dev_t gPscalGroupDev = 0;
static ino_t gPscalGroupIno = 0;
static time_t gPscalGroupMtime = 0;
static off_t gPscalGroupSize = 0;

static void pscalFreePasswdEntries(PscalPasswdEntry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].name);
        free(entries[i].passwd);
        free(entries[i].gecos);
        free(entries[i].dir);
        free(entries[i].shell);
    }
}

static void pscalFreeGroupEntries(PscalGroupEntry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].name);
        free(entries[i].passwd);
        free(entries[i].members_gids);
    }
}

static bool pscalEnsurePasswdArrayCapacity(PscalPasswdEntry **entries,
                                           size_t *capacity,
                                           size_t needed) {
    if (!entries || !capacity) {
        return false;
    }
    if (needed <= *capacity) {
        return true;
    }
    size_t new_cap = *capacity ? *capacity : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            return false;
        }
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(PscalPasswdEntry)) {
        return false;
    }
    PscalPasswdEntry *resized = realloc(*entries, new_cap * sizeof(PscalPasswdEntry));
    if (!resized) {
        return false;
    }
    *entries = resized;
    *capacity = new_cap;
    return true;
}

static bool pscalEnsureGroupArrayCapacity(PscalGroupEntry **entries,
                                          size_t *capacity,
                                          size_t needed) {
    if (!entries || !capacity) {
        return false;
    }
    if (needed <= *capacity) {
        return true;
    }
    size_t new_cap = *capacity ? *capacity : 16;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            return false;
        }
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(PscalGroupEntry)) {
        return false;
    }
    PscalGroupEntry *resized = realloc(*entries, new_cap * sizeof(PscalGroupEntry));
    if (!resized) {
        return false;
    }
    *entries = resized;
    *capacity = new_cap;
    return true;
}

static const char *pscalEtcPath(const char *leaf, char *buf, size_t buf_len) {
    if (!leaf || !buf || buf_len == 0) {
        return NULL;
    }
    const char *direct = getenv("PSCALI_ETC_ROOT");
    if (direct && direct[0] == '/' &&
        snprintf(buf, buf_len, "%s/%s", direct, leaf) < (int)buf_len &&
        access(buf, R_OK) == 0) {
        return buf;
    }

    const char *container = getenv("PSCALI_CONTAINER_ROOT");
    const char *home = getenv("HOME");
    const char *roots[][3] = {
        { container, "Documents/etc", leaf },
        { container, "etc", leaf },
        { home,       "Documents/etc", leaf },
        { home,       "etc", leaf },
    };
    for (size_t i = 0; i < sizeof(roots)/sizeof(roots[0]); ++i) {
        const char *base = roots[i][0];
        const char *sub = roots[i][1];
        if (!base || base[0] != '/') {
            continue;
        }
        if (snprintf(buf, buf_len, "%s/%s/%s", base, sub, leaf) >= (int)buf_len) {
            continue;
        }
        if (access(buf, R_OK) == 0) {
            return buf;
        }
    }
    return NULL;
}

static bool pscalContainerUserDbAvailable(const char *leaf) {
    char path[PATH_MAX];
    return pscalEtcPath(leaf, path, sizeof(path)) != NULL;
}

static void pscalLoadPasswd(void) {
    char path[PATH_MAX];
    const char *passwd_path = pscalEtcPath("passwd", path, sizeof(path));
    struct stat st;
    if (!passwd_path || stat(passwd_path, &st) != 0) {
        return;
    }
    pthread_mutex_lock(&gPscalUserDbLock);
    bool needs_reload = !gPscalPasswdLoaded ||
                        strcmp(gPscalPasswdPath, passwd_path) != 0 ||
                        st.st_mtime != gPscalPasswdMtime ||
                        st.st_size != gPscalPasswdSize ||
                        st.st_ino != gPscalPasswdIno ||
                        st.st_dev != gPscalPasswdDev;
    pthread_mutex_unlock(&gPscalUserDbLock);
    if (!needs_reload) {
        return;
    }
    FILE *fp = fopen(passwd_path, "r");
    if (!fp) {
        return;
    }
    PscalPasswdEntry *new_entries = NULL;
    size_t new_count = 0;
    size_t new_capacity = 0;
    bool load_ok = true;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        char *saveptr = NULL;
        char *tok_name   = strtok_r(line, ":\n", &saveptr);
        char *tok_passwd = strtok_r(NULL, ":\n", &saveptr);
        char *tok_uid    = strtok_r(NULL, ":\n", &saveptr);
        char *tok_gid    = strtok_r(NULL, ":\n", &saveptr);
        char *tok_gecos  = strtok_r(NULL, ":\n", &saveptr);
        char *tok_dir    = strtok_r(NULL, ":\n", &saveptr);
        char *tok_shell  = strtok_r(NULL, ":\n", &saveptr);
        if (!tok_name || !tok_passwd || !tok_uid || !tok_gid) {
            continue;
        }
        if (!pscalEnsurePasswdArrayCapacity(&new_entries, &new_capacity, new_count + 1)) {
            load_ok = false;
            break;
        }
        char *name = strdup(tok_name);
        char *passwd = strdup(tok_passwd);
        char *gecos = tok_gecos ? strdup(tok_gecos) : strdup("");
        char *dir = tok_dir ? strdup(tok_dir) : strdup("/");
        char *shell = tok_shell ? strdup(tok_shell) : strdup("/bin/sh");
        if (!name || !passwd || !gecos || !dir || !shell) {
            free(name);
            free(passwd);
            free(gecos);
            free(dir);
            free(shell);
            load_ok = false;
            break;
        }
        PscalPasswdEntry *entry = &new_entries[new_count];
        memset(entry, 0, sizeof(*entry));
        entry->name = name;
        entry->passwd = passwd;
        entry->gecos = gecos;
        entry->dir = dir;
        entry->shell = shell;
        entry->pw.pw_name   = entry->name;
        entry->pw.pw_passwd = entry->passwd;
        entry->pw.pw_uid    = (uid_t)strtoul(tok_uid, NULL, 10);
        entry->pw.pw_gid    = (gid_t)strtoul(tok_gid, NULL, 10);
        entry->pw.pw_gecos  = entry->gecos;
        entry->pw.pw_dir    = entry->dir;
        entry->pw.pw_shell  = entry->shell;
        new_count++;
    }
    free(line);
    fclose(fp);
    if (!load_ok) {
        pscalFreePasswdEntries(new_entries, new_count);
        free(new_entries);
        return;
    }
    pthread_mutex_lock(&gPscalUserDbLock);
    gPscalPasswd = new_entries;
    gPscalPasswdCount = new_count;
    gPscalPasswdLoaded = true;
    strlcpy(gPscalPasswdPath, passwd_path, sizeof(gPscalPasswdPath));
    gPscalPasswdDev = st.st_dev;
    gPscalPasswdIno = st.st_ino;
    gPscalPasswdMtime = st.st_mtime;
    gPscalPasswdSize = st.st_size;
    pthread_mutex_unlock(&gPscalUserDbLock);
}

static void pscalLoadGroup(void) {
    char path[PATH_MAX];
    const char *group_path = pscalEtcPath("group", path, sizeof(path));
    struct stat st;
    if (!group_path || stat(group_path, &st) != 0) {
        return;
    }
    pthread_mutex_lock(&gPscalUserDbLock);
    bool needs_reload = !gPscalGroupLoaded ||
                        strcmp(gPscalGroupPath, group_path) != 0 ||
                        st.st_mtime != gPscalGroupMtime ||
                        st.st_size != gPscalGroupSize ||
                        st.st_ino != gPscalGroupIno ||
                        st.st_dev != gPscalGroupDev;
    pthread_mutex_unlock(&gPscalUserDbLock);
    if (!needs_reload) {
        return;
    }
    FILE *fp = fopen(group_path, "r");
    if (!fp) {
        return;
    }
    PscalGroupEntry *new_entries = NULL;
    size_t new_count = 0;
    size_t new_capacity = 0;
    bool load_ok = true;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        char *saveptr = NULL;
        char *tok_name   = strtok_r(line, ":\n", &saveptr);
        char *tok_passwd = strtok_r(NULL, ":\n", &saveptr);
        char *tok_gid    = strtok_r(NULL, ":\n", &saveptr);
        if (!tok_name || !tok_passwd || !tok_gid) {
            continue;
        }
        if (!pscalEnsureGroupArrayCapacity(&new_entries, &new_capacity, new_count + 1)) {
            load_ok = false;
            break;
        }
        char *name = strdup(tok_name);
        char *passwd = strdup(tok_passwd);
        if (!name || !passwd) {
            free(name);
            free(passwd);
            load_ok = false;
            break;
        }
        PscalGroupEntry *entry = &new_entries[new_count];
        memset(entry, 0, sizeof(*entry));
        entry->name = name;
        entry->passwd = passwd;
        entry->gr.gr_name   = entry->name;
        entry->gr.gr_passwd = entry->passwd;
        entry->gr.gr_gid    = (gid_t)strtoul(tok_gid, NULL, 10);
        entry->gr.gr_mem    = NULL;
        new_count++;
    }
    free(line);
    fclose(fp);
    if (!load_ok) {
        pscalFreeGroupEntries(new_entries, new_count);
        free(new_entries);
        return;
    }
    pthread_mutex_lock(&gPscalUserDbLock);
    gPscalGroup = new_entries;
    gPscalGroupCount = new_count;
    gPscalGroupLoaded = true;
    strlcpy(gPscalGroupPath, group_path, sizeof(gPscalGroupPath));
    gPscalGroupDev = st.st_dev;
    gPscalGroupIno = st.st_ino;
    gPscalGroupMtime = st.st_mtime;
    gPscalGroupSize = st.st_size;
    pthread_mutex_unlock(&gPscalUserDbLock);
}

static struct passwd *pscalGetpwuid(uid_t uid) {
    pscalLoadPasswd();
    struct passwd *found = NULL;
    pthread_mutex_lock(&gPscalUserDbLock);
    for (size_t i = 0; i < gPscalPasswdCount; ++i) {
        if (gPscalPasswd[i].pw.pw_uid == uid) {
            found = &gPscalPasswd[i].pw;
            break;
        }
    }
    pthread_mutex_unlock(&gPscalUserDbLock);
    return found;
}

static struct passwd *pscalGetpwnam(const char *name) {
    if (!name) return NULL;
    pscalLoadPasswd();
    struct passwd *found = NULL;
    pthread_mutex_lock(&gPscalUserDbLock);
    for (size_t i = 0; i < gPscalPasswdCount; ++i) {
        if (strcmp(gPscalPasswd[i].pw.pw_name, name) == 0) {
            found = &gPscalPasswd[i].pw;
            break;
        }
    }
    pthread_mutex_unlock(&gPscalUserDbLock);
    return found;
}

static struct group *pscalGetgrgid(gid_t gid) {
    pscalLoadGroup();
    struct group *found = NULL;
    pthread_mutex_lock(&gPscalUserDbLock);
    for (size_t i = 0; i < gPscalGroupCount; ++i) {
        if (gPscalGroup[i].gr.gr_gid == gid) {
            found = &gPscalGroup[i].gr;
            break;
        }
    }
    pthread_mutex_unlock(&gPscalUserDbLock);
    return found;
}

static struct group *pscalGetgrnam(const char *name) {
    if (!name) return NULL;
    pscalLoadGroup();
    struct group *found = NULL;
    pthread_mutex_lock(&gPscalUserDbLock);
    for (size_t i = 0; i < gPscalGroupCount; ++i) {
        if (strcmp(gPscalGroup[i].gr.gr_name, name) == 0) {
            found = &gPscalGroup[i].gr;
            break;
        }
    }
    pthread_mutex_unlock(&gPscalUserDbLock);
    return found;
}

static bool pscalAllowHostPasswdFallbackForUid(uid_t uid) {
    if (!pscalContainerUserDbAvailable("passwd")) {
        return true;
    }
    return uid == getuid() || uid == geteuid();
}

static bool pscalAllowHostPasswdFallbackForName(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (!pscalContainerUserDbAvailable("passwd")) {
        return true;
    }
    const char *user = getenv("USER");
    if (user && strcmp(user, name) == 0) {
        return true;
    }
    const char *logname = getenv("LOGNAME");
    if (logname && strcmp(logname, name) == 0) {
        return true;
    }
    return false;
}

static bool pscalAllowHostGroupFallbackForGid(gid_t gid) {
    if (!pscalContainerUserDbAvailable("group")) {
        return true;
    }
    return gid == getgid() || gid == getegid();
}

static bool pscalAllowHostGroupFallbackForName(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (!pscalContainerUserDbAvailable("group")) {
        return true;
    }
    struct group *self = pscalGetgrgid(getgid());
    if (self && self->gr_name && strcmp(self->gr_name, name) == 0) {
        return true;
    }
    return false;
}

__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwuid(uid_t uid);
__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwnam(const char *name);
__attribute__((weak)) struct group *pscalRuntimeHostGetgrgid(gid_t gid);
__attribute__((weak)) struct group *pscalRuntimeHostGetgrnam(const char *name);

__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwuid(uid_t uid) {
    static _Thread_local struct passwd pw;
    static _Thread_local char storage[16384];
    struct passwd *result = NULL;
    if (getpwuid_r(uid, &pw, storage, sizeof(storage), &result) != 0) {
        return NULL;
    }
    return result;
}
__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwnam(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    static _Thread_local struct passwd pw;
    static _Thread_local char storage[16384];
    struct passwd *result = NULL;
    if (getpwnam_r(name, &pw, storage, sizeof(storage), &result) != 0) {
        return NULL;
    }
    return result;
}
__attribute__((weak)) struct group *pscalRuntimeHostGetgrgid(gid_t gid) {
    static _Thread_local struct group gr;
    static _Thread_local char storage[16384];
    struct group *result = NULL;
    if (getgrgid_r(gid, &gr, storage, sizeof(storage), &result) != 0) {
        return NULL;
    }
    return result;
}
__attribute__((weak)) struct group *pscalRuntimeHostGetgrnam(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    static _Thread_local struct group gr;
    static _Thread_local char storage[16384];
    struct group *result = NULL;
    if (getgrnam_r(name, &gr, storage, sizeof(storage), &result) != 0) {
        return NULL;
    }
    return result;
}

struct passwd *getpwuid(uid_t uid) {
    struct passwd *pw = pscalGetpwuid(uid);
    if (pw) return pw;
    if (!pscalAllowHostPasswdFallbackForUid(uid)) return NULL;
    if (pscalRuntimeHostGetpwuid) return pscalRuntimeHostGetpwuid(uid);
    return NULL;
}

struct passwd *getpwnam(const char *name) {
    struct passwd *pw = pscalGetpwnam(name);
    if (pw) return pw;
    if (!pscalAllowHostPasswdFallbackForName(name)) return NULL;
    if (pscalRuntimeHostGetpwnam) return pscalRuntimeHostGetpwnam(name);
    return NULL;
}

struct group *getgrgid(gid_t gid) {
    struct group *gr = pscalGetgrgid(gid);
    if (gr) return gr;
    if (!pscalAllowHostGroupFallbackForGid(gid)) return NULL;
    if (pscalRuntimeHostGetgrgid) return pscalRuntimeHostGetgrgid(gid);
    return NULL;
}

struct group *getgrnam(const char *name) {
    struct group *gr = pscalGetgrnam(name);
    if (gr) return gr;
    if (!pscalAllowHostGroupFallbackForName(name)) return NULL;
    if (pscalRuntimeHostGetgrnam) return pscalRuntimeHostGetgrnam(name);
    return NULL;
}

int vprocLookupPasswdName(uid_t uid, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = '\0';

    struct passwd *pw = pscalGetpwuid(uid);
    if (!pw) {
        if (!pscalAllowHostPasswdFallbackForUid(uid)) {
            errno = ENOENT;
            return -1;
        }
        if (pscalRuntimeHostGetpwuid) {
            pw = pscalRuntimeHostGetpwuid(uid);
        }
    }
    if (!pw || !pw->pw_name || pw->pw_name[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    size_t name_len = strlen(pw->pw_name);
    if (name_len >= buffer_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, pw->pw_name, name_len + 1);
    return 0;
}

int vprocLookupGroupName(gid_t gid, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = '\0';

    struct group *gr = pscalGetgrgid(gid);
    if (!gr) {
        if (!pscalAllowHostGroupFallbackForGid(gid)) {
            errno = ENOENT;
            return -1;
        }
        if (pscalRuntimeHostGetgrgid) {
            gr = pscalRuntimeHostGetgrgid(gid);
        }
    }
    if (!gr || !gr->gr_name || gr->gr_name[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    size_t name_len = strlen(gr->gr_name);
    if (name_len >= buffer_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, gr->gr_name, name_len + 1);
    return 0;
}
#endif /* PSCAL_TARGET_IOS */

static void *vprocFilterSelfSymbol(void *sym) {
    if (!sym) {
        return NULL;
    }
    const void *self_base = vprocSelfBase();
    if (self_base) {
        Dl_info info;
        if (dladdr(sym, &info) != 0 && info.dli_fbase == self_base) {
            return NULL;
        }
    }
    return sym;
}

static bool vprocSymbolIsLogRedirect(void *sym) {
    if (!sym) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(sym, &info) == 0) return false;
    if (info.dli_sname) {
        if (strstr(info.dli_sname, "LogRedirect") != NULL) return true;
        if (strstr(info.dli_sname, "logredirect") != NULL) return true;
    }
    if (info.dli_fname) {
        if (strstr(info.dli_fname, "LogRedirect") != NULL) return true;
        if (strstr(info.dli_fname, "logredirect") != NULL) return true;
    }
    return false;
}

static void *vprocOpenLibSystemHandle(void) {
    static void *handle = NULL;
    static int tried = 0;
    if (tried) {
        return handle;
    }
    tried = 1;
    handle = dlopen("libSystem.B.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        handle = dlopen("libSystem.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/libSystem.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/system/libsystem_c.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/system/libsystem_kernel.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/system/libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
}

static void *vprocResolveSymbol(const char *name) {
    void *sym = NULL;
    void *libsystem = vprocOpenLibSystemHandle();
    if (libsystem) {
        sym = vprocFilterSelfSymbol(dlsym(libsystem, name));
    }
    if (!sym) {
        sym = vprocFilterSelfSymbol(dlsym(RTLD_NEXT, name));
    }
    if (!sym) {
        sym = vprocFilterSelfSymbol(dlsym(RTLD_DEFAULT, name));
    }
    return sym;
}

static pthread_once_t vprocHostReadOnce = PTHREAD_ONCE_INIT;
static void *vprocHostReadDirect = NULL;
static ssize_t (*vprocHostReadFn)(int, void *, size_t) = NULL;

static void vprocHostReadInit(void) {
#if defined(__APPLE__)
    if (&__read_nocancel) {
        vprocHostReadDirect = vprocFilterSelfSymbol((void *)&__read_nocancel);
    }
#endif
    vprocHostReadFn = (ssize_t (*)(int, void *, size_t))vprocResolveSymbol("__read_nocancel");
    if (!vprocHostReadFn) {
        vprocHostReadFn = (ssize_t (*)(int, void *, size_t))vprocResolveSymbol("read");
    }
    if (!vprocHostReadFn) {
        vprocHostReadFn = (ssize_t (*)(int, void *, size_t))vprocResolveSymbol("read$NOCANCEL");
    }
}

static VPROC_UNUSED ssize_t vprocHostReadRaw(int fd, void *buf, size_t count) {
    pthread_once(&vprocHostReadOnce, vprocHostReadInit);
#if defined(__APPLE__)
    if (vprocHostReadDirect) {
        ssize_t (*direct_fn)(int, void *, size_t) = (ssize_t (*)(int, void *, size_t))vprocHostReadDirect;
        vprocInterposeBypassEnter();
        ssize_t res = direct_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (vprocHostReadFn) {
        ssize_t (*read_fn)(int, void *, size_t) = vprocHostReadFn;
        vprocInterposeBypassEnter();
        ssize_t res = read_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
    vprocInterposeBypassEnter();
    ssize_t res = read(fd, buf, count);
    vprocInterposeBypassExit();
    return res;
}

static pthread_once_t vprocHostWriteOnce = PTHREAD_ONCE_INIT;
static void *vprocHostWriteDirect = NULL;
static ssize_t (*vprocHostWriteFallback)(int, const void *, size_t) = NULL;
static ssize_t (*vprocHostWriteFn)(int, const void *, size_t) = NULL;

static void vprocHostWriteInit(void) {
#if defined(__APPLE__)
    if (&__write_nocancel) {
        void *direct = vprocFilterSelfSymbol((void *)&__write_nocancel);
        if (vprocSymbolIsLogRedirect(direct)) {
            vprocHostWriteFallback = (ssize_t (*)(int, const void *, size_t))direct;
        } else {
            vprocHostWriteDirect = direct;
        }
    }
#endif
    ssize_t (*fn)(int, const void *, size_t) = NULL;
    fn = (ssize_t (*)(int, const void *, size_t))vprocResolveSymbol("__write_nocancel");
    if (vprocSymbolIsLogRedirect((void *)fn)) {
        if (!vprocHostWriteFallback) {
            vprocHostWriteFallback = fn;
        }
        fn = NULL;
    }
    if (!fn) {
        fn = (ssize_t (*)(int, const void *, size_t))vprocResolveSymbol("write");
        if (vprocSymbolIsLogRedirect((void *)fn)) {
            if (!vprocHostWriteFallback) {
                vprocHostWriteFallback = fn;
            }
            fn = NULL;
        }
    }
    if (!fn) {
        fn = (ssize_t (*)(int, const void *, size_t))vprocResolveSymbol("write$NOCANCEL");
        if (vprocSymbolIsLogRedirect((void *)fn)) {
            if (!vprocHostWriteFallback) {
                vprocHostWriteFallback = fn;
            }
            fn = NULL;
        }
    }
    if (!fn && vprocHostWriteFallback && !vprocSymbolIsLogRedirect((void *)vprocHostWriteFallback)) {
        fn = vprocHostWriteFallback;
    }
    vprocHostWriteFn = fn;
}

static ssize_t vprocHostWriteRaw(int fd, const void *buf, size_t count) {
    pthread_once(&vprocHostWriteOnce, vprocHostWriteInit);
#if defined(__APPLE__)
    if (vprocHostWriteDirect) {
        ssize_t (*direct_fn)(int, const void *, size_t) = (ssize_t (*)(int, const void *, size_t))vprocHostWriteDirect;
        vprocInterposeBypassEnter();
        ssize_t res = direct_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (vprocHostWriteFn) {
        ssize_t (*write_fn)(int, const void *, size_t) = vprocHostWriteFn;
        vprocInterposeBypassEnter();
        ssize_t res = write_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
    vprocInterposeBypassEnter();
    ssize_t res = write(fd, buf, count);
    vprocInterposeBypassExit();
    return res;
}

static bool vprocHostFdLooksLikeKqueue(int fd) {
#if defined(__APPLE__)
    if (fd < 0) {
        return false;
    }
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
    vprocInterposeBypassEnter();
    errno = 0;
    int rc = kevent(fd, NULL, 0, NULL, 0, &timeout);
    int err = errno;
    vprocInterposeBypassExit();
    if (rc >= 0) {
        return true;
    }
    /* kevent(2) returns EINVAL for non-kqueue descriptors and EBADF for
     * invalid descriptors. Any other outcome is treated conservatively. */
    return err != EINVAL && err != EBADF;
#else
    (void)fd;
    return false;
#endif
}

static int vprocHostCloseRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (vprocProtectKqueueCloseActive() &&
        fd > STDERR_FILENO &&
        vprocHostFdLooksLikeKqueue(fd)) {
        return 0;
    }
#if defined(__APPLE__)
    if (&__close_nocancel) {
        vprocInterposeBypassEnter();
        int res = __close_nocancel(fd);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("__close_nocancel");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("close");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("close$NOCANCEL");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostDupRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("dup");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static VPROC_UNUSED int vprocHostDup2Raw(int fd, int target) {
    static int (*fn)(int, int) = NULL;
    if (!fn) {
        fn = (int (*)(int, int))vprocResolveSymbol("dup2");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, target);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostPipeRaw(int pipefd[2]) {
    static int (*fn)(int[2]) = NULL;
    if (!fn) {
        fn = (int (*)(int[2]))vprocResolveSymbol("pipe");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(pipefd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSocketRaw(int domain, int type, int protocol) {
    static int (*fn)(int, int, int) = NULL;
    if (!fn) {
        fn = (int (*)(int, int, int))vprocResolveSymbol("socket");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(domain, type, protocol);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostAcceptRaw(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    static int (*fn)(int, struct sockaddr *, socklen_t *) = NULL;
    if (!fn) {
        fn = (int (*)(int, struct sockaddr *, socklen_t *))vprocResolveSymbol("accept");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, addr, addrlen);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSocketpairRaw(int domain, int type, int protocol, int sv[2]) {
    static int (*fn)(int, int, int, int[2]) = NULL;
    if (!fn) {
        fn = (int (*)(int, int, int, int[2]))vprocResolveSymbol("socketpair");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(domain, type, protocol, sv);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostIoctlRaw(int fd, unsigned long request, void *arg) {
    static int (*fn)(int, unsigned long, ...) = NULL;
    if (!fn) {
        fn = (int (*)(int, unsigned long, ...))vprocResolveSymbol("ioctl");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, request, arg);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

#if defined(__APPLE__)
typedef struct {
    const void *replacement;
    const void *replacee;
} VprocDyldInterposePair;

#define VPROC_DYLD_INTERPOSE(_replacement, _replacee)                                         \
    __attribute__((used)) static const VprocDyldInterposePair                                  \
        _vproc_interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {        \
            (const void *)(uintptr_t)&(_replacement),                                           \
            (const void *)(uintptr_t)&(_replacee)                                               \
        }

static int vprocIoctlInterposed(int fd, unsigned long request, ...) {
    void *arg = NULL;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (!vprocInterposeReady() ||
        vprocInterposeBypassActive() ||
        vprocThreadIsInterposeBypassed(pthread_self())) {
        return vprocHostIoctlRaw(fd, request, arg);
    }
    return vprocIoctlShim(fd, request, arg);
}

static ssize_t vprocReadInterposed(int fd, void *buf, size_t count) {
    if (!vprocInterposeReady() ||
        vprocInterposeBypassActive() ||
        vprocThreadIsInterposeBypassed(pthread_self())) {
        return vprocHostReadRaw(fd, buf, count);
    }
    return vprocReadShim(fd, buf, count);
}

static ssize_t vprocWriteInterposed(int fd, const void *buf, size_t count) {
    if (!vprocInterposeReady() ||
        vprocInterposeBypassActive() ||
        vprocThreadIsInterposeBypassed(pthread_self())) {
        return vprocHostWriteRaw(fd, buf, count);
    }
    /* Host code sets its signal dispositions on the host, not on a task. */
    return vprocWriteShimNoSignal(fd, buf, count);
}

static ssize_t vprocReadNoCancelInterposed(int fd, void *buf, size_t count) {
    return vprocReadInterposed(fd, buf, count);
}

static ssize_t vprocWriteNoCancelInterposed(int fd, const void *buf, size_t count) {
    return vprocWriteInterposed(fd, buf, count);
}

VPROC_DYLD_INTERPOSE(vprocIoctlInterposed, ioctl);
VPROC_DYLD_INTERPOSE(vprocReadInterposed, read);
VPROC_DYLD_INTERPOSE(vprocWriteInterposed, write);
VPROC_DYLD_INTERPOSE(vprocReadNoCancelInterposed, __read_nocancel);
VPROC_DYLD_INTERPOSE(vprocWriteNoCancelInterposed, __write_nocancel);
#undef VPROC_DYLD_INTERPOSE
#endif

static VPROC_UNUSED off_t vprocHostLseekRaw(int fd, off_t offset, int whence) {
    static off_t (*fn)(int, off_t, int) = NULL;
    if (!fn) {
        fn = (off_t (*)(int, off_t, int))vprocResolveSymbol("lseek");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        off_t res = fn(fd, offset, whence);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (off_t)-1;
}

static int vprocHostFsyncRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("__fsync");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("fsync");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("fsync$NOCANCEL");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostFstatRaw(int fd, struct stat *st) {
    static int (*fn)(int, struct stat *) = NULL;
    /* Not __fstat: that is the old syscall, with a 32-bit inode and a different
     * layout. st_mode came back as the inode's low bits, so one file in sixteen
     * looked like a directory, and st_size came back as 0. */
    if (!fn) {
        fn = (int (*)(int, struct stat *))vprocResolveSymbol("fstat64");
    }
    if (!fn) {
        fn = (int (*)(int, struct stat *))vprocResolveSymbol("fstat");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, st);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostStatRaw(const char *path, struct stat *st) {
#if defined(AT_FDCWD)
    vprocInterposeBypassEnter();
    int res = fstatat(AT_FDCWD, path, st, 0);
    vprocInterposeBypassExit();
    return res;
#else
    static int (*fn)(const char *, struct stat *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, struct stat *))vprocResolveSymbol("stat");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, st);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
#endif
}

static int vprocHostLstatRaw(const char *path, struct stat *st) {
#if defined(AT_FDCWD)
    vprocInterposeBypassEnter();
    int res = fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
    vprocInterposeBypassExit();
    return res;
#else
    static int (*fn)(const char *, struct stat *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, struct stat *))vprocResolveSymbol("lstat");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, st);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
#endif
}

static int vprocHostChdirRaw(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("chdir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static char *vprocHostGetcwdRaw(char *buf, size_t size) {
    static char *(*fn)(char *, size_t) = NULL;
    if (!fn) {
        fn = (char *(*)(char *, size_t))vprocResolveSymbol("getcwd");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        char *res = fn(buf, size);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return NULL;
}

static int vprocHostAccessRaw(const char *path, int mode) {
    static int (*fn)(const char *, int) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, int))vprocResolveSymbol("__access");
    }
    if (!fn) {
        fn = (int (*)(const char *, int))vprocResolveSymbol("access");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, mode);
        vprocInterposeBypassExit();
        return res;
    }
#if defined(AT_FDCWD)
    vprocInterposeBypassEnter();
    int res = faccessat(AT_FDCWD, path, mode, 0);
    vprocInterposeBypassExit();
    return res;
#endif
    errno = ENOSYS;
    return -1;
}

static int vprocHostChmodRaw(const char *path, mode_t mode) {
    if (vprocHostPathPermit(path, VPROC_PERM_WRITE) != 0) {
        return -1;
    }
    static int (*fn)(const char *, mode_t) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, mode_t))vprocResolveSymbol("chmod");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, mode);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostFchmodRaw(int fd, mode_t mode) {
    static int (*fn)(int, mode_t) = NULL;
    if (!fn) {
        fn = (int (*)(int, mode_t))vprocResolveSymbol("fchmod");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, mode);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostChownRaw(const char *path, uid_t uid, gid_t gid) {
    static int (*fn)(const char *, uid_t, gid_t) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, uid_t, gid_t))vprocResolveSymbol("chown");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, uid, gid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostFchownRaw(int fd, uid_t uid, gid_t gid) {
    static int (*fn)(int, uid_t, gid_t) = NULL;
    if (!fn) {
        fn = (int (*)(int, uid_t, gid_t))vprocResolveSymbol("fchown");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, uid, gid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostUtimesRaw(const char *path, const struct timeval times[2]) {
    if (vprocHostPathPermit(path, VPROC_PERM_WRITE) != 0) {
        return -1;
    }
    static int (*fn)(const char *, const struct timeval *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, const struct timeval *))vprocResolveSymbol("utimes");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, times);
        vprocInterposeBypassExit();
        return res;
    }
#if defined(AT_FDCWD)
    struct timespec ts[2];
    struct timespec *tsp = NULL;
    if (times) {
        ts[0].tv_sec = times[0].tv_sec;
        ts[0].tv_nsec = (long)times[0].tv_usec * 1000L;
        ts[1].tv_sec = times[1].tv_sec;
        ts[1].tv_nsec = (long)times[1].tv_usec * 1000L;
        tsp = ts;
    }
    vprocInterposeBypassEnter();
    int res = utimensat(AT_FDCWD, path, tsp, 0);
    vprocInterposeBypassExit();
    return res;
#endif
    errno = ENOSYS;
    return -1;
}

static int vprocHostFutimesRaw(int fd, const struct timeval times[2]) {
    static int (*fn)(int, const struct timeval *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const struct timeval *))vprocResolveSymbol("futimes");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, times);
        vprocInterposeBypassExit();
        return res;
    }
#if defined(HAVE_FUTIMENS) || defined(__APPLE__)
    struct timespec ts[2];
    struct timespec *tsp = NULL;
    if (times) {
        ts[0].tv_sec = times[0].tv_sec;
        ts[0].tv_nsec = (long)times[0].tv_usec * 1000L;
        ts[1].tv_sec = times[1].tv_sec;
        ts[1].tv_nsec = (long)times[1].tv_usec * 1000L;
        tsp = ts;
    }
    vprocInterposeBypassEnter();
    int res = futimens(fd, tsp);
    vprocInterposeBypassExit();
    return res;
#endif
    errno = ENOSYS;
    return -1;
}

static int vprocHostMkdirRaw(const char *path, mode_t mode) {
    if (vprocHostPathPermit(path, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    static int (*fn)(const char *, mode_t) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, mode_t))vprocResolveSymbol("mkdir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, mode);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRmdirRaw(const char *path) {
    if (vprocHostPathPermit(path, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("rmdir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostUnlinkRaw(const char *path) {
    if (vprocHostPathPermit(path, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("unlink");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRemoveRaw(const char *path) {
    if (vprocHostPathPermit(path, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("remove");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRenameRaw(const char *oldpath, const char *newpath) {
    if (vprocHostPathPermit(oldpath, VPROC_PERM_ENTRY) != 0 ||
        vprocHostPathPermit(newpath, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    static int (*fn)(const char *, const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, const char *))vprocResolveSymbol("rename");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(oldpath, newpath);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static DIR *vprocHostOpendirRaw(const char *name) {
    static DIR *(*fn)(const char *) = NULL;
    if (!fn) {
        fn = (DIR *(*)(const char *))vprocResolveSymbol("opendir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        DIR *res = fn(name);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return NULL;
}

static int vprocHostSymlinkRaw(const char *target, const char *linkpath) {
    if (vprocHostPathPermit(linkpath, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    static int (*fn)(const char *, const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, const char *))vprocResolveSymbol("symlink");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(target, linkpath);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static ssize_t vprocHostReadlinkRaw(const char *path, char *buf, size_t size) {
    static ssize_t (*fn)(const char *, char *, size_t) = NULL;
    if (!fn) {
        fn = (ssize_t (*)(const char *, char *, size_t))vprocResolveSymbol("readlink");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        ssize_t res = fn(path, buf, size);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static char *vprocHostRealpathRaw(const char *path, char *resolved_path) {
    static char *(*fn)(const char *, char *) = NULL;
    if (!fn) {
        fn = (char *(*)(const char *, char *))vprocResolveSymbol("realpath");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        char *res = fn(path, resolved_path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return NULL;
}

static int vprocHostStatVirtualized(const char *path, struct stat *st) {
#if defined(PSCAL_TARGET_IOS)
    if (pathTruncateEnabled()) {
        char expanded[PATH_MAX];
        if (pathTruncateExpand(path, expanded, sizeof(expanded))) {
            return vprocHostStatRaw(expanded, st);
        }
    }
#endif
    return vprocHostStatRaw(path, st);
}

static int vprocHostLstatVirtualized(const char *path, struct stat *st) {
#if defined(PSCAL_TARGET_IOS)
    if (pathTruncateEnabled()) {
        char expanded[PATH_MAX];
        if (pathTruncateExpand(path, expanded, sizeof(expanded))) {
            return vprocHostLstatRaw(expanded, st);
        }
    }
#endif
    return vprocHostLstatRaw(path, st);
}

static int vprocHostPollRaw(struct pollfd *fds, nfds_t nfds, int timeout) {
    static int (*fn)(struct pollfd *, nfds_t, int) = NULL;
    if (!fn) {
        fn = (int (*)(struct pollfd *, nfds_t, int))vprocResolveSymbol("poll");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fds, nfds, timeout);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSelectRaw(int nfds,
                              fd_set *readfds,
                              fd_set *writefds,
                              fd_set *exceptfds,
                              struct timeval *timeout) {
    static int (*fn)(int, fd_set *, fd_set *, fd_set *, struct timeval *) = NULL;
    if (!fn) {
        fn = (int (*)(int, fd_set *, fd_set *, fd_set *, struct timeval *))vprocResolveSymbol("select");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(nfds, readfds, writefds, exceptfds, timeout);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static VPROC_UNUSED int vprocHostIsattyRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("isatty");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return 0;
}

#if defined(PSCAL_TARGET_IOS)
static _Thread_local bool gVprocFifoOpenDirect = false;
static int vprocHostOpenFifoKillable(const char *path, int flags, mode_t mode, bool has_mode);
#endif

/* A task's file mask is the VM's, kept with its directory, and a task asks the host for what is
 * left of a mode after it. The host still takes away what its own mask says from anything, so
 * that follows the session shell's: what does not come through here obeys that one. */
static int gVprocHostMask = -1;

static mode_t vprocHostMaskSet(mode_t mask) {
    mode_t previous = umask(mask);
    __atomic_store_n(&gVprocHostMask, (int)mask, __ATOMIC_RELAXED);
    return previous;
}

static mode_t vprocHostMaskGet(void) {
    int mask = __atomic_load_n(&gVprocHostMask, __ATOMIC_RELAXED);
    if (mask < 0) {
        mode_t current = vprocHostMaskSet(0);
        vprocHostMaskSet(current);
        mask = (int)current;
    }
    return (mode_t)mask;
}

/* It can only be read by setting it: do that before there is another thread to see it. */
__attribute__((constructor)) static void vprocHostMaskInit(void) {
    (void)vprocHostMaskGet();
}

/* True when the host would take away some of what is left: the caller then puts it back, on
 * what it made and nothing else. */
static bool vprocCreateModeApply(mode_t *mode);

static int vprocHostOpenCall(const char *path, int flags, mode_t mode, bool has_mode);

static int vprocHostOpenRawInternal(const char *path, int flags, mode_t mode, bool has_mode) {
    if (vprocHostPathPermitOpen(path, flags) != 0) {
        return -1;
    }
#if defined(PSCAL_TARGET_IOS)
    if (!gVprocFifoOpenDirect && !(flags & O_NONBLOCK) && (flags & O_ACCMODE) != O_RDWR) {
        struct stat fifo_st;
        if (vprocHostStatRaw(path, &fifo_st) == 0 && S_ISFIFO(fifo_st.st_mode)) {
            return vprocHostOpenFifoKillable(path, flags, mode, has_mode);
        }
    }
#endif
    if (has_mode && (flags & O_CREAT) && vprocCreateModeApply(&mode)) {
        /* Only O_EXCL says the file is new. */
        for (int tries = 0; tries < 4; ++tries) {
            int fd = vprocHostOpenCall(path, flags | O_EXCL, mode, true);
            if (fd >= 0) {
                (void)vprocHostFchmodRaw(fd, mode);
                return fd;
            }
            if (errno != EEXIST || (flags & O_EXCL)) {
                return -1;
            }
            fd = vprocHostOpenCall(path, flags & ~O_CREAT, 0, false);
            if (fd >= 0 || errno != ENOENT) {
                return fd;
            }
        }
    }
    return vprocHostOpenCall(path, flags, mode, has_mode);
}

static int vprocHostOpenCall(const char *path, int flags, mode_t mode, bool has_mode) {
    static int (*fn)(const char *, int, ...) = NULL;
    static int (*openat_fn)(int, const char *, int, ...) = NULL;
#if defined(__APPLE__)
    if (&__open_nocancel) {
        vprocInterposeBypassEnter();
        int res = __open_nocancel(path, flags, has_mode ? (int)mode : 0);
        vprocInterposeBypassExit();
        return res;
    }
    if (&__open) {
        vprocInterposeBypassEnter();
        int res = __open(path, flags, has_mode ? (int)mode : 0);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("__open_nocancel");
    }
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("__open");
    }
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("open");
    }
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("open$NOCANCEL");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = has_mode ? fn(path, flags, mode) : fn(path, flags);
        vprocInterposeBypassExit();
        return res;
    }
#if defined(AT_FDCWD)
    if (!openat_fn) {
        openat_fn = (int (*)(int, const char *, int, ...))vprocResolveSymbol("openat");
    }
    if (openat_fn) {
        vprocInterposeBypassEnter();
        int res = has_mode ? openat_fn(AT_FDCWD, path, flags, mode) : openat_fn(AT_FDCWD, path, flags);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    errno = ENOSYS;
    return -1;
}

int pscalHostOpenRaw(const char *path, int flags, mode_t mode) {
    return vprocHostOpenRawInternal(path, flags, mode, (flags & O_CREAT) != 0);
}

static pid_t vprocHostWaitpidRaw(pid_t pid, int *status_out, int options) {
    static pid_t (*fn)(pid_t, int *, int) = NULL;
    if (!fn) {
        fn = (pid_t (*)(pid_t, int *, int))vprocResolveSymbol("waitpid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(pid, status_out, options);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetpidRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("getpid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetppidRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("getppid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetpgrpRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("getpgrp");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetpgidRaw(pid_t pid) {
    static pid_t (*fn)(pid_t) = NULL;
    if (!fn) {
        fn = (pid_t (*)(pid_t))vprocResolveSymbol("getpgid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(pid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static int vprocHostSetpgidRaw(pid_t pid, pid_t pgid) {
    static int (*fn)(pid_t, pid_t) = NULL;
    if (!fn) {
        fn = (int (*)(pid_t, pid_t))vprocResolveSymbol("setpgid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(pid, pgid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static pid_t vprocHostGetsidRaw(pid_t pid) {
    static pid_t (*fn)(pid_t) = NULL;
    if (!fn) {
        fn = (pid_t (*)(pid_t))vprocResolveSymbol("getsid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(pid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostSetsidRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("setsid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostTcgetpgrpRaw(int fd) {
    static pid_t (*fn)(int) = NULL;
    if (!fn) {
        fn = (pid_t (*)(int))vprocResolveSymbol("tcgetpgrp");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static int vprocHostTcsetpgrpRaw(int fd, pid_t pgid) {
    static int (*fn)(int, pid_t) = NULL;
    if (!fn) {
        fn = (int (*)(int, pid_t))vprocResolveSymbol("tcsetpgrp");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, pgid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostPthreadCreateRaw(pthread_t *thread,
                                     const pthread_attr_t *attr,
                                     void *(*start_routine)(void *),
                                     void *arg) {
    static int (*fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
    if (!fn) {
        fn = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))vprocResolveSymbol("pthread_create");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(thread, attr, start_routine, arg);
        vprocInterposeBypassExit();
        return res;
    }
    return EINVAL;
}

static int vprocHostSigactionRaw(int sig, const struct sigaction *act, struct sigaction *oldact) {
    static int (*fn)(int, const struct sigaction *, struct sigaction *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const struct sigaction *, struct sigaction *))vprocResolveSymbol("sigaction");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(sig, act, oldact);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSigprocmaskRaw(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*fn)(int, const sigset_t *, sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const sigset_t *, sigset_t *))vprocResolveSymbol("sigprocmask");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(how, set, oldset);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSigpendingRaw(sigset_t *set) {
    static int (*fn)(sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(sigset_t *))vprocResolveSymbol("sigpending");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(set);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSigsuspendRaw(const sigset_t *mask) {
    static int (*fn)(const sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(const sigset_t *))vprocResolveSymbol("sigsuspend");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(mask);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRaiseRaw(int sig) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("raise");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(sig);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostPthreadSigmaskRaw(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*fn)(int, const sigset_t *, sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const sigset_t *, sigset_t *))vprocResolveSymbol("pthread_sigmask");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(how, set, oldset);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}
#undef VPROC_UNUSED
#endif

/* -- Compatibility Macros -- */
#ifndef W_EXITCODE
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#endif

#ifndef W_STOPCODE
#define W_STOPCODE(sig) ((sig) << 8 | 0x7f)
#endif

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
static int vprocHostOpenVirtualized(const char *path, int flags, int mode) {
    if (flags & O_CREAT) {
        return pscalPathVirtualized_open(path, flags, mode);
    }
    return pscalPathVirtualized_open(path, flags);
}
#elif defined(PSCAL_TARGET_IOS)
#define vprocHostOpenVirtualized(path, flags, mode) pscalPathVirtualized_open((path), (flags), (mode))
#else
#define vprocHostOpenVirtualized(path, flags, mode) open((path), (flags), (mode))
#endif

/* -- Undefine host macros that might conflict -- */
#ifdef waitpid
#undef waitpid
#endif
#ifdef kill
#undef kill
#endif
#ifdef getpid
#undef getpid
#endif
#ifdef getppid
#undef getppid
#endif
#ifdef getpgrp
#undef getpgrp
#endif
#ifdef getpgid
#undef getpgid
#endif
#ifdef setpgid
#undef setpgid
#endif
#ifdef getsid
#undef getsid
#endif
#ifdef setsid
#undef setsid
#endif
#ifdef tcgetpgrp
#undef tcgetpgrp
#endif
#ifdef tcsetpgrp
#undef tcsetpgrp
#endif
#ifdef sigaction
#undef sigaction
#endif
#ifdef sigprocmask
#undef sigprocmask
#endif
#ifdef sigpending
#undef sigpending
#endif
#ifdef sigsuspend
#undef sigsuspend
#endif
#ifdef signal
#undef signal
#endif
#ifdef raise
#undef raise
#endif
#ifdef pthread_sigmask
#undef pthread_sigmask
#endif

#ifndef VPROC_INITIAL_CAPACITY
#define VPROC_INITIAL_CAPACITY 16
#endif

/* -- Data Structures -- */

typedef enum {
    VPROC_FD_NONE = 0,
    VPROC_FD_HOST = 1,
    VPROC_FD_PSCAL = 2,
} VProcFdKind;

typedef struct {
    int host_fd;
    struct pscal_fd *pscal_fd;
    VProcFdKind kind;
    bool inherit; /* Above stdio: asked for by a redirection, so what this vproc starts gets it. */
} VProcFdEntry;

typedef struct {
    int host_fd;
    VProcResourceKind kind;
} VProcResourceEntry;

#define VPROC_NGROUPS_MAX 32
static void vprocCredInherit(VProc *vp, const VProc *parent);
static int vprocChownRecord(const char *host_path, uid_t uid, gid_t gid, bool follow);

struct VProc {
    pthread_mutex_t mu;     // ADDED: Protects the FD table shared by threads
    VProcFdEntry *entries;
    size_t capacity;
    int next_fd;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int stdin_host_fd;      /* stable handle for "controlling" stdin */
    int stdout_host_fd;
    int stderr_host_fd;
    bool stdin_from_session;
    pthread_t creator;
    VProcWinsize winsize;
    int pid;
    char cwd[PATH_MAX];
    mode_t file_mask;
    /* Who the task is, as a process's credentials: real, effective and saved. */
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;
    int ngroups;
    gid_t groups[VPROC_NGROUPS_MAX];
    bool cred_provisional;  /* taken before /etc/passwd could be read, as at a first launch */
    struct rlimit limits[RLIM_NLIMITS];
    VProcResourceEntry *resources;
    size_t resource_count;
    size_t resource_capacity;
    int pins;               // Threads borrowing this vproc for a moment, under gVProcRegistryMu
};

static int vprocSessionHostFdForStd(int std_fd);
static bool vprocSessionStdioMatchFd(int session_fd, int std_fd);
static int vprocHostIsatty(int fd);
static int vprocSetCompatErrno(int err);
static void vprocPtyTrace(const char *format, ...);
static int vprocForegroundPgidLocked(int sid);
bool vprocGetShellJobControlState(int *shell_pid_out,
                                  int *shell_pgid_out,
                                  int *sid_out,
                                  int *fg_pgid_out);

static __thread VProc *gVProcCurrent = NULL;
/* What gVProcCurrent replaced on this thread. Each eval, source or nested shell adds one, so a
 * recursion has no limit here: the first ones stay in place, and only a deep one allocates. */
#define VPROC_STACK_INLINE 16
static __thread VProc *gVProcStackInline[VPROC_STACK_INLINE] = {0};
static __thread VProc **gVProcStackSpill = NULL;
static __thread size_t gVProcStackSpillCapacity = 0;
static __thread size_t gVProcStackDepth = 0;
static __thread size_t gVProcStackLost = 0;
static __thread unsigned long long gVProcRegistrySeenVersion = 0;
static VProc **gVProcRegistry = NULL;
static size_t gVProcRegistryCount = 0;
static size_t gVProcRegistryCapacity = 0;
static VProc *gVProcRegistryHint = NULL;
static pthread_mutex_t gVProcRegistryMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gVProcPinsCv = PTHREAD_COND_INITIALIZER;
static unsigned long long gVProcRegistryVersion = 1;
static int gNextSyntheticPid = 0;
static _Thread_local int gShellSelfPid = 0;
static int gShellSelfPidGlobal = 0;
static _Thread_local int gKernelPid = 0;
static int gKernelPidGlobal = 0;
static volatile sig_atomic_t gVProcInterposeReady = 0;
static volatile sig_atomic_t gVProcTlsReady = 0; // Avoid TLV access before TLS is initialized.
static _Thread_local int gVProcInterposeBypassDepth = 0;
static volatile sig_atomic_t gVProcProtectKqueueCloseDepth = 0;
typedef struct {
    pthread_t *items;
    size_t count;
    size_t capacity;
    bool hint_valid;
    pthread_t hint_tid;
    size_t hint_index;
    pthread_mutex_t mu;
} VProcInterposeBypassRegistry;

static VProcInterposeBypassRegistry gVProcInterposeBypassRegistry = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .hint_valid = false,
    .hint_tid = 0,
    .hint_index = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t gPathTruncateMu = PTHREAD_MUTEX_INITIALIZER;
static bool gPathTruncateInit = false;
static pthread_mutex_t gFstabMountsMu = PTHREAD_MUTEX_INITIALIZER;
static bool gFstabMountsLoaded = false;
static VProc *gKernelVproc = NULL;
static pthread_mutex_t gKernelVprocMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gKernelThread;
static pthread_mutex_t gKernelThreadMu = PTHREAD_MUTEX_INITIALIZER;
static bool gKernelThreadStarted = false;
static pthread_cond_t gKernelThreadCv = PTHREAD_COND_INITIALIZER;
static bool gKernelThreadReady = false;
typedef enum {
    VPROC_KERNEL_EVENT_CONTROL_SIGNAL = 0,
    VPROC_KERNEL_EVENT_CONTINUE_PGID,
    VPROC_KERNEL_EVENT_SIGCHLD_AGGREGATE
} VProcKernelEventType;

typedef struct {
    VProcKernelEventType type;
    int shell_pid;
    int sig;
    bool allow_runtime_fallback;
    int sid;
    int pgid;
    int target_pid;
} VProcKernelControlEvent;
static VProcKernelControlEvent *gKernelControlQueue = NULL;
static size_t gKernelControlQueueCap = 0;
static size_t gKernelControlQueueCount = 0;
static size_t gKernelControlQueueHead = 0;
static pthread_t gShellSelfTid;
static bool gShellSelfTidValid = false;
static VProcSessionStdio gSessionStdioDefault = {
    .stdin_host_fd = -1,
    .stdout_host_fd = -1,
    .stderr_host_fd = -1,
    .kernel_pid = 0,
    .input = NULL,
    .stdin_pscal_fd = NULL,
    .stdout_pscal_fd = NULL,
    .stderr_pscal_fd = NULL,
    .pty_master = NULL,
    .pty_slave = NULL,
    .pty_out_thread = 0,
    .pty_active = false,
    .control_bytes_passthrough = false,
    .session_id = 0,
};
static pthread_once_t gSessionStdioDefaultOnce = PTHREAD_ONCE_INIT;
static _Thread_local VProcSessionStdio *gSessionStdioTls = NULL;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t owner;
    bool owner_valid;
    unsigned int depth;
    const char *name;
} VProcIsolationLock;

static VProcIsolationLock gVProcIsolationLocks[VPROC_ISOLATION_DOMAIN_COUNT] = {
    [VPROC_ISOLATION_DOMAIN_NEXTVI] = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .owner = (pthread_t)0,
        .owner_valid = false,
        .depth = 0,
        .name = "nextvi",
    },
    [VPROC_ISOLATION_DOMAIN_MICRO] = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .owner = (pthread_t)0,
        .owner_valid = false,
        .depth = 0,
        .name = "micro",
    },
};
static pthread_mutex_t gSessionInputInitMu = PTHREAD_MUTEX_INITIALIZER;
static const char *kLocationDevicePath = "/dev/location";
static const char *kLegacyGpsDevicePath = "/dev/gps";
static const char *kLegacyGpsDevicePath2 = "/dev/ttyGPS";

static bool vprocComputeGrowthCapacity(size_t current,
                                       size_t min_needed,
                                       size_t initial,
                                       size_t elem_size,
                                       size_t *out_cap) {
    if (!out_cap || elem_size == 0 || min_needed == 0) {
        return false;
    }
    size_t cap = current ? current : initial;
    if (cap < initial) {
        cap = initial;
    }
    while (cap < min_needed) {
        if (cap > SIZE_MAX / 2) {
            return false;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / elem_size) {
        return false;
    }
    *out_cap = cap;
    return true;
}

static void vprocResourceEnsureCapacity(VProc *vp, size_t needed) {
    if (!vp) {
        return;
    }
    if (vp->resource_capacity >= needed) {
        return;
    }
    size_t new_cap = 0;
    if (!vprocComputeGrowthCapacity(vp->resource_capacity,
                                    needed,
                                    8,
                                    sizeof(VProcResourceEntry),
                                    &new_cap)) {
        return;
    }
    VProcResourceEntry *resized = realloc(vp->resources, new_cap * sizeof(VProcResourceEntry));
    if (!resized) {
        return;
    }
    vp->resources = resized;
    vp->resource_capacity = new_cap;
}

static void vprocResourceTrackLocked(VProc *vp, int host_fd, VProcResourceKind kind) {
    if (!vp || host_fd < 0) {
        return;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        if (vp->resources[i].host_fd == host_fd) {
            vp->resources[i].kind = kind;
            return;
        }
    }
    vprocResourceEnsureCapacity(vp, vp->resource_count + 1);
    if (vp->resource_count < vp->resource_capacity) {
        vp->resources[vp->resource_count].host_fd = host_fd;
        vp->resources[vp->resource_count].kind = kind;
        vp->resource_count++;
    }
}

static void vprocResourceTrack(VProc *vp, int host_fd, VProcResourceKind kind) {
    if (!vp || host_fd < 0) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    vprocResourceTrackLocked(vp, host_fd, kind);
    pthread_mutex_unlock(&vp->mu);
}

static bool vprocResourceRemoveLocked(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        return false;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        if (vp->resources[i].host_fd != host_fd) {
            continue;
        }
        size_t last = vp->resource_count - 1;
        if (i != last) {
            vp->resources[i] = vp->resources[last];
        }
        vp->resource_count--;
        return true;
    }
    return false;
}

static void vprocResourceRemove(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    (void)vprocResourceRemoveLocked(vp, host_fd);
    pthread_mutex_unlock(&vp->mu);
}

static bool vprocResourceContainsLocked(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        return false;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        if (vp->resources[i].host_fd == host_fd) {
            return true;
        }
    }
    return false;
}

static bool vprocOwnsHostFdLocked(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        return false;
    }
    if (host_fd == vp->stdin_host_fd ||
        host_fd == vp->stdout_host_fd ||
        host_fd == vp->stderr_host_fd) {
        return true;
    }
    return vprocResourceContainsLocked(vp, host_fd);
}

static bool vprocAllowRealFdFallback(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        return false;
    }
    /* Not the host's stdio either: a vproc's own slot was closed, and `cat <&-` fails. */
    bool allow = false;
    pthread_mutex_lock(&vp->mu);
    allow = vprocOwnsHostFdLocked(vp, fd);
    pthread_mutex_unlock(&vp->mu);
    return allow;
}

static void vprocResourceCloseAllLocked(VProc *vp) {
    if (!vp) {
        return;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        int fd = vp->resources[i].host_fd;
        if (fd < 0) {
            continue;
        }
#if defined(PSCAL_TARGET_IOS)
        vprocHostCloseRaw(fd);
#else
        close(fd);
#endif
        vp->resources[i].host_fd = -1;
    }
    vp->resource_count = 0;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int read_fd;
    int write_fd;
    int stub_fd;
    int readers;
    uint64_t seq;
    bool cond_init;
    char last_payload[128];
    size_t last_len;
    bool has_payload;
    bool enabled;
} VProcLocationDevice;

static VProcLocationDevice gLocationDevice = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    -1,
    -1,
    -1,
    0,
    0,
    false,
    {0},
    0,
    false,
    true
};
static bool vprocLocationDebugEnabled(void);
static void vprocLocationDebugf(const char *fmt, ...);
static bool vprocToolDebugEnabled(void);
static bool vprocVprocDebugEnabled(void);
static bool vprocPipeDebugEnabled(void);
static bool vprocKillDebugEnabled(void);

typedef struct {
    uint64_t last_seq;
    size_t offset;
    size_t len;
    bool done;
    char payload[sizeof(((VProcLocationDevice *)0)->last_payload)];
} VProcLocationReader;

/* Forward declaration; defined later. */
static void vprocDeliverPendingSignalsForCurrent(void);
static void vprocDieHere(bool now);
static bool vprocCurrentHasInterruptingSignal(void);
static const struct pscal_fd_ops kVprocLocationFdOps;
typedef void (*VprocLocationReadersChangedFn)(int readers, void *context);
static VprocLocationReadersChangedFn gLocationReaderObserver = NULL;
static void *gLocationReaderObserverCtx = NULL;
static pthread_mutex_t gLocationReaderObserverMu = PTHREAD_MUTEX_INITIALIZER;

static void vprocLocationNotifyObservers(int readers) {
    VprocLocationReadersChangedFn cb = NULL;
    void *ctx = NULL;
    pthread_mutex_lock(&gLocationReaderObserverMu);
    cb = gLocationReaderObserver;
    ctx = gLocationReaderObserverCtx;
    pthread_mutex_unlock(&gLocationReaderObserverMu);
    if (cb) {
        cb(readers, ctx);
    }
}

/* -------- In-process pipe (mutex/cond-backed) -------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cond_read;
    pthread_cond_t cond_write;
    unsigned char *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
    bool read_closed;
    bool write_closed;
    int wait_readers;
    int wait_writers;
    int active_ops;
    bool freed;
    int readers;
    int writers;
} VprocInprocPipe;

typedef struct {
    VprocInprocPipe *pipe;
    bool is_reader;
} VprocInprocEnd;

static void vprocInprocPipeFree(VprocInprocPipe *pipe) {
    if (!pipe) {
        return;
    }
    pthread_mutex_destroy(&pipe->mu);
    pthread_cond_destroy(&pipe->cond_read);
    pthread_cond_destroy(&pipe->cond_write);
    free(pipe->buf);
    free(pipe);
}

static bool vprocInprocPipeShouldDestroy(VprocInprocPipe *pipe) {
    if (!pipe || pipe->freed) {
        return false;
    }
    bool destroy = pipe->read_closed &&
                   pipe->write_closed &&
                   pipe->readers <= 0 &&
                   pipe->writers <= 0 &&
                   pipe->active_ops == 0 &&
                   pipe->wait_readers == 0 &&
                   pipe->wait_writers == 0;
    if (destroy) {
        pipe->freed = true;
    }
    return destroy;
}

/* `kill` cancels a job's threads, and a condition wait is a cancellation point that takes the
 * mutex back before the thread's cleanup handlers run. Without a handler to let go of it, the
 * thread dies holding the mutex, or deadlocks on it in its own cleanup.
 * Darwin also acts on a cancel that is already pending as the wait starts, after it has let go of
 * the mutex and without taking it back, and this unlock then took another thread's hold on it: a
 * wait elsewhere ended the app as a corrupt mutex. So a pending cancel is acted on here first,
 * with the mutex held, and the task table's mutex checks its owner, so the rare cancel that comes
 * in between finds nothing to unlock. */
static void vprocCondWaitCancelled(void *mu) {
    pthread_mutex_unlock((pthread_mutex_t *)mu);
}

static int vprocCondWait(pthread_cond_t *cv, pthread_mutex_t *mu) {
    int rc;
    pthread_cleanup_push(vprocCondWaitCancelled, mu);
    pthread_testcancel();
    rc = pthread_cond_wait(cv, mu);
    pthread_cleanup_pop(0);
    return rc;
}

static int vprocCondTimedWait(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *deadline) {
    int rc;
    pthread_cleanup_push(vprocCondWaitCancelled, mu);
    pthread_testcancel();
    rc = pthread_cond_timedwait(cv, mu, deadline);
    pthread_cleanup_pop(0);
    return rc;
}

/* Bound the cond wait so blocking pipe I/O periodically rechecks for a pending
 * virtualized signal. We cannot interrupt a pthread_cond_wait with a host
 * signal the way iSH interrupts real_poll_wait with SIGUSR1, so a short
 * absolute-deadline poll (CLOCK_REALTIME, matching the default cond clock on
 * Apple/Linux) is how Ctrl-C/Ctrl-Z reach a stage parked on an empty/full
 * pipe. */
static void vprocInprocPipeWaitDeadline(struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += 100L * 1000000L; /* 100ms interrupt-poll interval */
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

typedef struct {
    VprocInprocPipe *pipe;
    int *waiters;
} VprocInprocPipeWaiter;

static void vprocInprocPipeWaitCancelled(void *arg) {
    VprocInprocPipeWaiter *waiter = (VprocInprocPipeWaiter *)arg;
    VprocInprocPipe *pipe = waiter->pipe;
    (*waiter->waiters)--;
    pipe->active_ops--;
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_mutex_unlock(&pipe->mu);
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
}

static void vprocInprocPipeWait(VprocInprocPipe *pipe, pthread_cond_t *cond, int *waiters) {
    VprocInprocPipeWaiter waiter = { pipe, waiters };
    struct timespec ts;
    vprocInprocPipeWaitDeadline(&ts);
    (*waiters)++;
    pthread_cleanup_push(vprocInprocPipeWaitCancelled, &waiter);
    pthread_cond_timedwait(cond, &pipe->mu, &ts);
    pthread_cleanup_pop(0);
    (*waiters)--;
}

static ssize_t vprocInprocPipeRead(struct pscal_fd *fd, void *buf, size_t bufsize) {
    if (!fd || !buf) {
        return _EBADF;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe || !end->is_reader) {
        return _EBADF;
    }
    VprocInprocPipe *pipe = end->pipe;
    pthread_mutex_lock(&pipe->mu);
    pipe->active_ops++;
    while (pipe->count == 0 && !pipe->write_closed) {
        if (fd->flags & O_NONBLOCK) {
            pipe->active_ops--;
            bool destroy = vprocInprocPipeShouldDestroy(pipe);
            pthread_mutex_unlock(&pipe->mu);
            if (destroy) {
                vprocInprocPipeFree(pipe);
            }
            return _EAGAIN;
        }
        vprocInprocPipeWait(pipe, &pipe->cond_read, &pipe->wait_readers);
        if (pipe->count == 0 && !pipe->write_closed &&
            vprocCurrentHasInterruptingSignal()) {
            pipe->active_ops--;
            bool destroy = vprocInprocPipeShouldDestroy(pipe);
            pthread_mutex_unlock(&pipe->mu);
            if (destroy) {
                vprocInprocPipeFree(pipe);
            }
            return _EINTR;
        }
    }
    if (pipe->count == 0 && pipe->write_closed) {
        pipe->active_ops--;
        bool destroy = vprocInprocPipeShouldDestroy(pipe);
        pthread_mutex_unlock(&pipe->mu);
        if (destroy) {
            vprocInprocPipeFree(pipe);
        }
        return 0;
    }
    size_t to_copy = bufsize;
    if (to_copy > pipe->count) {
        to_copy = pipe->count;
    }
    for (size_t i = 0; i < to_copy; ++i) {
        ((unsigned char *)buf)[i] = pipe->buf[pipe->head];
        pipe->head = (pipe->head + 1) % pipe->cap;
    }
    pipe->count -= to_copy;
    pipe->active_ops--;
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_cond_signal(&pipe->cond_write);
    pthread_mutex_unlock(&pipe->mu);
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
    pscal_fd_poll_wakeup(fd, POLLIN);
    return (ssize_t)to_copy;
}

static ssize_t vprocInprocPipeWrite(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    if (!fd || !buf) {
        return _EBADF;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe || end->is_reader) {
        return _EBADF;
    }
    VprocInprocPipe *pipe = end->pipe;
    pthread_mutex_lock(&pipe->mu);
    pipe->active_ops++;
    if (pipe->write_closed) {
        pipe->active_ops--;
        pthread_mutex_unlock(&pipe->mu);
        return _EPIPE;
    }
    while (pipe->count == pipe->cap && !pipe->read_closed) {
        if (fd->flags & O_NONBLOCK) {
            pipe->active_ops--;
            bool destroy = vprocInprocPipeShouldDestroy(pipe);
            pthread_mutex_unlock(&pipe->mu);
            if (destroy) {
                vprocInprocPipeFree(pipe);
            }
            return _EAGAIN;
        }
        vprocInprocPipeWait(pipe, &pipe->cond_write, &pipe->wait_writers);
        if (pipe->count == pipe->cap && !pipe->read_closed &&
            vprocCurrentHasInterruptingSignal()) {
            pipe->active_ops--;
            bool destroy = vprocInprocPipeShouldDestroy(pipe);
            pthread_mutex_unlock(&pipe->mu);
            if (destroy) {
                vprocInprocPipeFree(pipe);
            }
            return _EINTR;
        }
    }
    if (pipe->read_closed) {
        pipe->active_ops--;
        bool destroy = vprocInprocPipeShouldDestroy(pipe);
        pthread_mutex_unlock(&pipe->mu);
        if (destroy) {
            vprocInprocPipeFree(pipe);
        }
        return _EPIPE;
    }
    size_t space = pipe->cap - pipe->count;
    size_t to_copy = bufsize;
    if (to_copy > space) {
        to_copy = space;
    }
    for (size_t i = 0; i < to_copy; ++i) {
        pipe->buf[pipe->tail] = ((const unsigned char *)buf)[i];
        pipe->tail = (pipe->tail + 1) % pipe->cap;
    }
    pipe->count += to_copy;
    pipe->active_ops--;
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_cond_signal(&pipe->cond_read);
    pthread_mutex_unlock(&pipe->mu);
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
    pscal_fd_poll_wakeup(fd, POLLOUT);
    return (ssize_t)to_copy;
}

static int vprocInprocPipePoll(struct pscal_fd *fd) {
    if (!fd) {
        return 0;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe) {
        return 0;
    }
    VprocInprocPipe *pipe = end->pipe;
    int events = 0;
    pthread_mutex_lock(&pipe->mu);
    if (end->is_reader) {
        if (pipe->count > 0) {
            events |= POLLIN;
        }
        if (pipe->write_closed) {
            events |= POLLHUP;
        }
    } else {
        if (!pipe->read_closed && pipe->count < pipe->cap) {
            events |= POLLOUT;
        }
        if (pipe->read_closed) {
            events |= POLLERR;
        }
    }
    pthread_mutex_unlock(&pipe->mu);
    return events;
}

static int vprocInprocPipeClose(struct pscal_fd *fd) {
    if (!fd) {
        return _EBADF;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe) {
        return _EBADF;
    }
    VprocInprocPipe *pipe = end->pipe;
    pthread_mutex_lock(&pipe->mu);
    bool dbg = vprocPipeDebugEnabled();
    if (end->is_reader) {
        pipe->read_closed = true;
        if (pipe->readers > 0) {
            pipe->readers--;
        }
    } else {
        pipe->write_closed = true;
        if (pipe->writers > 0) {
            pipe->writers--;
        }
    }
    if (dbg) {
        fprintf(stderr,
                "[pipe-close] pipe=%p end=%s readers=%d writers=%d count=%zu read_closed=%d write_closed=%d active=%d wait_r=%d wait_w=%d\n",
                (void *)pipe,
                end->is_reader ? "r" : "w",
                pipe->readers,
                pipe->writers,
                pipe->count,
                (int)pipe->read_closed,
                (int)pipe->write_closed,
                pipe->active_ops,
                pipe->wait_readers,
                pipe->wait_writers);
    }
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_cond_broadcast(&pipe->cond_read);
    pthread_cond_broadcast(&pipe->cond_write);
    pthread_mutex_unlock(&pipe->mu);
    pscal_fd_poll_wakeup(fd, POLLHUP);
    free(end);
    fd->userdata = NULL;
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
    return 0;
}

static const struct pscal_fd_ops kVprocInprocPipeReadOps = {
    .read = vprocInprocPipeRead,
    .write = NULL,
    .poll = vprocInprocPipePoll,
    .ioctl_size = NULL,
    .ioctl = NULL,
    .close = vprocInprocPipeClose,
};

static const struct pscal_fd_ops kVprocInprocPipeWriteOps = {
    .read = NULL,
    .write = vprocInprocPipeWrite,
    .poll = vprocInprocPipePoll,
    .ioctl_size = NULL,
    .ioctl = NULL,
    .close = vprocInprocPipeClose,
};

int vprocCreateInprocPipe(struct pscal_fd **out_read, struct pscal_fd **out_write) {
    if (!out_read || !out_write) {
        errno = EINVAL;
        return -1;
    }
    *out_read = NULL;
    *out_write = NULL;

    VprocInprocPipe *pipe = (VprocInprocPipe *)calloc(1, sizeof(VprocInprocPipe));
    if (!pipe) {
        errno = ENOMEM;
        return -1;
    }
    pipe->cap = 8192;
    pipe->buf = (unsigned char *)calloc(pipe->cap, sizeof(unsigned char));
    pthread_mutex_init(&pipe->mu, NULL);
    pthread_cond_init(&pipe->cond_read, NULL);
    pthread_cond_init(&pipe->cond_write, NULL);
    pipe->readers = 1;
    pipe->writers = 1;

    VprocInprocEnd *read_end = (VprocInprocEnd *)calloc(1, sizeof(VprocInprocEnd));
    VprocInprocEnd *write_end = (VprocInprocEnd *)calloc(1, sizeof(VprocInprocEnd));
    if (!pipe->buf || !read_end || !write_end) {
        free(read_end);
        free(write_end);
        vprocInprocPipeFree(pipe);
        errno = ENOMEM;
        return -1;
    }
    read_end->pipe = pipe;
    read_end->is_reader = true;
    write_end->pipe = pipe;
    write_end->is_reader = false;

    struct pscal_fd *rfd = pscal_fd_create(&kVprocInprocPipeReadOps);
    struct pscal_fd *wfd = pscal_fd_create(&kVprocInprocPipeWriteOps);
    if (!rfd || !wfd) {
        if (rfd) pscal_fd_close(rfd);
        if (wfd) pscal_fd_close(wfd);
        free(read_end);
        free(write_end);
        vprocInprocPipeFree(pipe);
        errno = ENOMEM;
        return -1;
    }
    rfd->userdata = read_end;
    wfd->userdata = write_end;
    *out_read = rfd;
    *out_write = wfd;
    return 0;
}

typedef struct {
    uint64_t session_id;
    int shell_pid;
    struct pscal_fd *pty_slave;
    struct pscal_fd *pty_master;
    int last_winsize_cols;
    int last_winsize_rows;
    bool control_bytes_passthrough;
    VProcSessionOutputHandler output_handler;
    void *output_context;
    unsigned char *pending_output;
    size_t pending_output_len;
    size_t pending_output_cap;
    bool output_paused;
    bool output_backlog_saturated;
} VProcSessionPtyEntry;

enum {
    VPROC_SESSION_OUTPUT_BACKLOG_MAX = 2 * 1024 * 1024,
    VPROC_SESSION_OUTPUT_THROTTLE_US = 20000
};

static struct {
    VProcSessionPtyEntry *items;
    size_t count;
    size_t capacity;
    pthread_mutex_t mu;
} gVProcSessionPtys = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
};
static uint64_t gVProcSessionPtyHintId = 0;
static size_t gVProcSessionPtyHintIndex = 0;
static _Thread_local uint64_t gVProcSessionPtyTlsHintId = 0;
static _Thread_local size_t gVProcSessionPtyTlsHintIndex = 0;

static bool vprocSessionPtyEntryIsEmpty(const VProcSessionPtyEntry *entry) {
    return !entry || (!entry->pty_slave &&
                      !entry->pty_master &&
                      entry->last_winsize_cols <= 0 &&
                      entry->last_winsize_rows <= 0 &&
                      !entry->output_handler &&
                      entry->pending_output_len == 0 &&
                      !entry->output_paused);
}

static void vprocSessionPtyEntryClearPendingLocked(VProcSessionPtyEntry *entry) {
    if (!entry) {
        return;
    }
    free(entry->pending_output);
    entry->pending_output = NULL;
    entry->pending_output_len = 0;
    entry->pending_output_cap = 0;
    entry->output_backlog_saturated = false;
}

static bool vprocSessionEnsurePendingCapacityLocked(VProcSessionPtyEntry *entry, size_t needed) {
    if (!entry) {
        return false;
    }
    if (needed > VPROC_SESSION_OUTPUT_BACKLOG_MAX) {
        needed = VPROC_SESSION_OUTPUT_BACKLOG_MAX;
    }
    if (entry->pending_output_cap >= needed) {
        return true;
    }
    size_t cap = entry->pending_output_cap > 0 ? entry->pending_output_cap : 4096;
    while (cap < needed) {
        size_t next = cap << 1;
        if (next <= cap) {
            cap = needed;
            break;
        }
        cap = next;
        if (cap >= VPROC_SESSION_OUTPUT_BACKLOG_MAX) {
            cap = VPROC_SESSION_OUTPUT_BACKLOG_MAX;
            break;
        }
    }
    unsigned char *resized = (unsigned char *)realloc(entry->pending_output, cap);
    if (!resized) {
        return false;
    }
    entry->pending_output = resized;
    entry->pending_output_cap = cap;
    return true;
}

static bool vprocSessionQueueOutputLocked(VProcSessionPtyEntry *entry,
                                          const unsigned char *data,
                                          size_t len) {
    if (!entry || !data || len == 0) {
        return true;
    }
    /* Once detached backlog is saturated, keep only the freshest chunk. */
    if (entry->output_backlog_saturated) {
        if (len >= VPROC_SESSION_OUTPUT_BACKLOG_MAX) {
            if (!vprocSessionEnsurePendingCapacityLocked(entry, VPROC_SESSION_OUTPUT_BACKLOG_MAX)) {
                return false;
            }
            const unsigned char *tail = data + (len - VPROC_SESSION_OUTPUT_BACKLOG_MAX);
            memcpy(entry->pending_output, tail, VPROC_SESSION_OUTPUT_BACKLOG_MAX);
            entry->pending_output_len = VPROC_SESSION_OUTPUT_BACKLOG_MAX;
            return true;
        }
        if (!vprocSessionEnsurePendingCapacityLocked(entry, len)) {
            return false;
        }
        memcpy(entry->pending_output, data, len);
        entry->pending_output_len = len;
        return true;
    }
    if (len >= VPROC_SESSION_OUTPUT_BACKLOG_MAX) {
        if (!vprocSessionEnsurePendingCapacityLocked(entry, VPROC_SESSION_OUTPUT_BACKLOG_MAX)) {
            return false;
        }
        const unsigned char *tail = data + (len - VPROC_SESSION_OUTPUT_BACKLOG_MAX);
        memcpy(entry->pending_output, tail, VPROC_SESSION_OUTPUT_BACKLOG_MAX);
        entry->pending_output_len = VPROC_SESSION_OUTPUT_BACKLOG_MAX;
        entry->output_backlog_saturated = true;
        return true;
    }

    size_t total = entry->pending_output_len + len;
    if (total > VPROC_SESSION_OUTPUT_BACKLOG_MAX) {
        if (!vprocSessionEnsurePendingCapacityLocked(entry, len)) {
            return false;
        }
        memcpy(entry->pending_output, data, len);
        entry->pending_output_len = len;
        entry->output_backlog_saturated = true;
        return true;
    }

    size_t needed = entry->pending_output_len + len;
    if (!vprocSessionEnsurePendingCapacityLocked(entry, needed)) {
        return false;
    }
    memcpy(entry->pending_output + entry->pending_output_len, data, len);
    entry->pending_output_len += len;
    return true;
}

static VProcSessionPtyEntry *vprocSessionPtyFindLocked(uint64_t session_id, size_t *out_index) {
    if (out_index) {
        *out_index = 0;
    }
    if (session_id == 0 || !gVProcSessionPtys.items || gVProcSessionPtys.count == 0) {
        return NULL;
    }
    if (gVProcSessionPtyTlsHintId == session_id && gVProcSessionPtyTlsHintIndex < gVProcSessionPtys.count) {
        VProcSessionPtyEntry *hint = &gVProcSessionPtys.items[gVProcSessionPtyTlsHintIndex];
        if (hint->session_id == session_id) {
            if (out_index) {
                *out_index = gVProcSessionPtyTlsHintIndex;
            }
            return hint;
        }
    }
    if (gVProcSessionPtyHintId == session_id && gVProcSessionPtyHintIndex < gVProcSessionPtys.count) {
        VProcSessionPtyEntry *hint = &gVProcSessionPtys.items[gVProcSessionPtyHintIndex];
        if (hint->session_id == session_id) {
            if (out_index) {
                *out_index = gVProcSessionPtyHintIndex;
            }
            return hint;
        }
    }
    for (size_t i = 0; i < gVProcSessionPtys.count; ++i) {
        VProcSessionPtyEntry *entry = &gVProcSessionPtys.items[i];
        if (entry->session_id == session_id) {
            gVProcSessionPtyHintId = session_id;
            gVProcSessionPtyHintIndex = i;
            gVProcSessionPtyTlsHintId = session_id;
            gVProcSessionPtyTlsHintIndex = i;
            if (out_index) {
                *out_index = i;
            }
            return entry;
        }
    }
    return NULL;
}

static void vprocSessionPtyRemoveAtLocked(size_t idx) {
    if (idx >= gVProcSessionPtys.count) {
        return;
    }
    vprocSessionPtyEntryClearPendingLocked(&gVProcSessionPtys.items[idx]);
    size_t last = gVProcSessionPtys.count - 1;
    if (idx != last) {
        gVProcSessionPtys.items[idx] = gVProcSessionPtys.items[last];
    }
    gVProcSessionPtys.count--;
    gVProcSessionPtyHintId = 0;
    gVProcSessionPtyHintIndex = 0;
    gVProcSessionPtyTlsHintId = 0;
    gVProcSessionPtyTlsHintIndex = 0;
    if (gVProcSessionPtys.count == 0) {
        free(gVProcSessionPtys.items);
        gVProcSessionPtys.items = NULL;
        gVProcSessionPtys.capacity = 0;
    }
}

static VProcSessionPtyEntry *vprocSessionPtyEnsureLocked(uint64_t session_id) {
    VProcSessionPtyEntry *existing = vprocSessionPtyFindLocked(session_id, NULL);
    if (existing) {
        return existing;
    }
    if (gVProcSessionPtys.count >= gVProcSessionPtys.capacity) {
        size_t new_cap = 0;
        if (!vprocComputeGrowthCapacity(gVProcSessionPtys.capacity,
                                        gVProcSessionPtys.count + 1,
                                        4,
                                        sizeof(VProcSessionPtyEntry),
                                        &new_cap)) {
            return NULL;
        }
        VProcSessionPtyEntry *resized = realloc(gVProcSessionPtys.items, new_cap * sizeof(VProcSessionPtyEntry));
        if (!resized) {
            return NULL;
        }
        gVProcSessionPtys.items = resized;
        gVProcSessionPtys.capacity = new_cap;
    }
    size_t idx = gVProcSessionPtys.count++;
    VProcSessionPtyEntry *slot = &gVProcSessionPtys.items[idx];
    memset(slot, 0, sizeof(*slot));
    slot->session_id = session_id;
    gVProcSessionPtyHintId = session_id;
    gVProcSessionPtyHintIndex = idx;
    gVProcSessionPtyTlsHintId = session_id;
    gVProcSessionPtyTlsHintIndex = idx;
    return slot;
}

static void vprocSessionPtySetShellPid(uint64_t session_id, int shell_pid) {
    if (session_id == 0 || shell_pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (entry) {
        entry->shell_pid = shell_pid;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

static bool vprocSessionControlBytePassthroughLocked(uint64_t session_id) {
    if (session_id == 0) {
        return false;
    }
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    return entry ? entry->control_bytes_passthrough : false;
}

void vprocSessionSetControlBytePassthrough(uint64_t session_id, bool enabled) {
    if (session_id == 0) {
        return;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (entry) {
        entry->control_bytes_passthrough = enabled;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

bool vprocSessionGetControlBytePassthrough(uint64_t session_id) {
    if (session_id == 0) {
        return false;
    }
    bool enabled = false;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    enabled = vprocSessionControlBytePassthroughLocked(session_id);
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    return enabled;
}

static bool vprocPathIsLocationDevice(const char *path);
static bool vprocPathIsLegacyGpsDevice(const char *path);
static int vprocLocationDeviceOpen(VProc *vp, int flags);
static int vprocLocationDeviceOpenHost(int flags);
static inline bool vprocShimHasVirtualContext(void);
static bool vprocToolDebugEnabled(void);
static bool vprocVprocDebugEnabled(void);
static bool vprocPipeDebugEnabled(void);
static bool vprocKillDebugEnabled(void);

static int vprocNextPidSeed(void) {
#if defined(PSCAL_TARGET_IOS)
    return 1;
#else
    int host = (int)getpid();
    if (host < 2000) {
        host += 2000;
    }
    return host;
#endif
}

static void vprocRegistryAdd(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
    }
    if (gVProcRegistryCount >= gVProcRegistryCapacity) {
        size_t new_cap = 0;
        if (!vprocComputeGrowthCapacity(gVProcRegistryCapacity,
                                        gVProcRegistryCount + 1,
                                        16,
                                        sizeof(VProc *),
                                        &new_cap)) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
        VProc **resized = (VProc **)realloc(gVProcRegistry, new_cap * sizeof(VProc *));
        if (!resized) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
        gVProcRegistry = resized;
        gVProcRegistryCapacity = new_cap;
    }
    gVProcRegistry[gVProcRegistryCount++] = vp;
    gVProcRegistryHint = vp;
    __atomic_add_fetch(&gVProcRegistryVersion, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&gVProcRegistryMu);
}

static void vprocRegistryRemove(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            gVProcRegistry[i] = gVProcRegistry[gVProcRegistryCount - 1];
            gVProcRegistryCount--;
            if (gVProcRegistryHint == vp) {
                gVProcRegistryHint = (gVProcRegistryCount > 0) ? gVProcRegistry[0] : NULL;
            }
            if (gVProcRegistryCount == 0) {
                free(gVProcRegistry);
                gVProcRegistry = NULL;
                gVProcRegistryCapacity = 0;
                gVProcRegistryHint = NULL;
            }
            __atomic_add_fetch(&gVProcRegistryVersion, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
}

/* A thread that is not a vproc's owner can lose it at any moment: a pipeline stage's parent can
 * be killed while the stage starts. A pin keeps it whole until vprocUnpin, since vprocDestroy
 * waits for its pins once it has left the registry, and nothing can pin it after that. */
static VProc *vprocPin(VProc *vp) {
    if (!vp) {
        return NULL;
    }
    VProc *pinned = NULL;
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            vp->pins++;
            pinned = vp;
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return pinned;
}

void vprocUnpin(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    if (--vp->pins == 0) {
        pthread_cond_broadcast(&gVProcPinsCv);
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
}

static void vprocWaitForPins(VProc *vp) {
    int cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gVProcRegistryMu);
    while (vp->pins > 0) {
        pthread_cond_wait(&gVProcPinsCv, &gVProcRegistryMu);
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    pthread_setcancelstate(cancel_state, NULL);
}

static bool vprocRegistryContains(const VProc *vp) {
    if (!vp) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&gVProcRegistryMu);
    if (gVProcRegistryHint == vp) {
        found = true;
    } else {
        for (size_t i = 0; i < gVProcRegistryCount; ++i) {
            if (gVProcRegistry[i] == vp) {
                found = true;
                gVProcRegistryHint = gVProcRegistry[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return found;
}

/* For a pointer that may be freed: only its address is compared until the registry has it,
 * and the pid tells it from a newer vproc at the same address. */
static bool vprocRegistryHoldsOwn(const VProc *vp) {
    bool found = false;
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            found = pthread_equal(vp->creator, pthread_self());
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return found;
}

static bool vprocRegistryHoldsPid(const VProc *vp, int pid) {
    bool found = false;
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            found = (vp->pid == pid);
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return found;
}

static inline VProc **vprocStackSlot(size_t index) {
    return (index < VPROC_STACK_INLINE) ? &gVProcStackInline[index]
                                        : &gVProcStackSpill[index - VPROC_STACK_INLINE];
}

static void vprocStackFreeSpill(void) {
    free(gVProcStackSpill);
    gVProcStackSpill = NULL;
    gVProcStackSpillCapacity = 0;
}

static void vprocClearThreadState(void) {
    gVProcCurrent = NULL;
    gVProcStackDepth = 0;
    gVProcStackLost = 0;
    gVProcRegistrySeenVersion = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < VPROC_STACK_INLINE; ++i) {
        gVProcStackInline[i] = NULL;
    }
    vprocStackFreeSpill();
}

/* Fast validation for the common case where vp is the current thread's vproc. */
static bool vprocRegistryContainsValidated(VProc *vp) {
    if (!vp) {
        return false;
    }
    if (gVProcTlsReady && vp == gVProcCurrent) {
        unsigned long long version = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
        if (gVProcRegistrySeenVersion == version) {
            return true;
        }
        if (!vprocRegistryContains(vp)) {
            vprocClearThreadState();
            return false;
        }
        gVProcRegistrySeenVersion = version;
        return true;
    }
    return vprocRegistryContains(vp);
}

static VProc *vprocForThread(void);
static VProc *vprocResolveShellVprocForPathOps(void);
static VProc *vprocFindByPid(int pid);
static _Thread_local int gVprocSubshellPid;
static _Thread_local bool gVprocClosedPipe;

typedef struct {
    int pid;
    pthread_t tid;
    pthread_t *threads;
    size_t thread_count;
    size_t thread_capacity;
    int parent_pid;
    int pgid;
    int sid;
    bool session_leader;
    int fg_pgid;
    int status;
    int exit_signal;
    bool exited;
    bool stopped;
    bool continued;
    int stop_signo;
    bool zombie;
    bool stop_unsupported;
    bool cooperative_stop_wait;
    bool unwinds_itself;
    bool halting;
    bool shell_task; /* a shell runs under it: what it sends itself waits, as on a thread that unwinds */
    bool silent_to_parent;
    bool not_a_process;
    bool orphaned; /* its parent ended first, so nothing waits for it */
    uint32_t caught_signals;
    bool sigchld_delivery_queued;
    bool shell_prompt_read_active;
    bool gave_up_terminal; /* TIOCNOTTY: it has no controlling terminal, whatever its session has */
    bool adopted;          /* its parent died: whoever has it now keeps no group from being orphaned */
    int job_id;
    char *label;
    char comm[16];
    int *children;
    size_t child_count;
    size_t child_capacity;
    int sigchld_events;
    bool sigchld_blocked;
    int rusage_utime;
    int rusage_stime;
    bool group_exit;
    int group_exit_code;
    uint32_t blocked_signals;
    uint32_t pending_signals;
    uint32_t ignored_signals;
    int pending_counts[32];
    int fg_override_pgid;
    struct sigaction actions[32];
    bool real_timer_active;
    uint64_t real_timer_deadline_ns;
    uint64_t real_timer_interval_ns;
    uint64_t start_mono_ns;
    uint64_t dying_since_ns; /* killed, with a thread still to unwind; vprocMarkExit ends it */
    uid_t cred_ruid;         /* its vproc's, for kill's check from another task */
    uid_t cred_euid;
    uid_t cred_suid;
} VProcTaskEntry;

typedef enum {
    VPROC_SIGCHLD_EVENT_EXIT = 0,
    VPROC_SIGCHLD_EVENT_STOP,
    VPROC_SIGCHLD_EVENT_CONT
} VProcSigchldEvent;

typedef struct {
    VProcTaskEntry *items;
    size_t count;
    size_t capacity;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} VProcTaskTable;

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    VProc *vp;
    int vp_pid;
    VProcSessionStdio *session_stdio;
    int shell_self_pid;
    int kernel_pid;
    bool detach;
    VProcEnv *env;
    PSCALRuntimeContext *runtime_ctx;
    FrontendKind frontend_kind;
    pthread_mutex_t *worker_stop_mu;
    pthread_cond_t *worker_stop_cv;
    bool *worker_stopped;
} VProcThreadStartCtx;

static VProcTaskEntry *vprocTaskFindLocked(int pid);
static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid);
static void vprocClearEntryLocked(VProcTaskEntry *entry);
static void vprocQueuePendingSignalLocked(VProcTaskEntry *entry, int sig);
static void vprocCancelListAdd(pthread_t **list, size_t *count, size_t *capacity, pthread_t tid);
static void vprocTaskTableRepairLocked(void);
static int vprocPtyNumForPid(int pid);
static int vprocSetForegroundPgidInternal(int sid, int fg_pgid, bool sync_tty);
static void vprocSyncForegroundPgidToSessionTty(int sid, int fg_pgid);
static void vprocUnregisterThreadByPid(int pid, pthread_t tid);
static void vprocRegisterStartedThread(int pid, pthread_t tid);
static void vprocThreadLeavesStarter(void *arg);
static void vprocScratchTlsCleanupCurrentThread(void);
static __thread bool gVprocPipelineStage = false;
static __thread bool gVprocInterruptKeyRead = false;
static __thread bool gVprocTerminalInterrupted = false;
typedef void (*VprocStopWaitHookFn)(void);
static __thread VprocStopWaitHookFn gVprocStopWaitBeforeHook = NULL;
static __thread VprocStopWaitHookFn gVprocStopWaitAfterHook = NULL;

void vprocSetStopWaitHooks(void (*before_hook)(void), void (*after_hook)(void)) {
    gVprocStopWaitBeforeHook = before_hook;
    gVprocStopWaitAfterHook = after_hook;
}

__attribute__((weak)) void PSCALRuntimeOnProcessGroupEmpty(int pgid);

#if defined(PTHREAD_ERRORCHECK_MUTEX_INITIALIZER)
#define VPROC_TASKS_MUTEX_INITIALIZER PTHREAD_ERRORCHECK_MUTEX_INITIALIZER
#elif defined(PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP)
#define VPROC_TASKS_MUTEX_INITIALIZER PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#else
#define VPROC_TASKS_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#endif
/* An unlock by a thread that doesn't hold the mutex fails rather than freeing it: see
 * vprocCondWaitCancelled. */
static VProcTaskTable gVProcTasks = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = VPROC_TASKS_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static VProcTaskEntry *gVProcTasksItemsStable = NULL;
static size_t gVProcTasksCapacityStable = 0;
static size_t gVProcTaskFindHint = 0;
static size_t gVProcTaskFreeHint = 0;
#define VPROC_TASK_LOOKUP_CACHE_SIZE 2048u
_Static_assert((VPROC_TASK_LOOKUP_CACHE_SIZE & (VPROC_TASK_LOOKUP_CACHE_SIZE - 1u)) == 0,
               "VPROC_TASK_LOOKUP_CACHE_SIZE must be a power of two");
#define VPROC_TASK_LOOKUP_CACHE_WAYS 2u
typedef struct {
    int pid;
    uint32_t idx;
} VProcTaskLookupCacheEntry;
typedef struct {
    VProcTaskLookupCacheEntry way[VPROC_TASK_LOOKUP_CACHE_WAYS];
} VProcTaskLookupCacheSet;
static VProcTaskLookupCacheSet gVProcTaskLookupCache[VPROC_TASK_LOOKUP_CACHE_SIZE];

static inline size_t vprocTaskLookupSlotForPid(int pid) {
    uint32_t hash = (uint32_t)pid * 2654435761u;
    return (size_t)(hash & (VPROC_TASK_LOOKUP_CACHE_SIZE - 1u));
}

static inline void vprocTaskLookupRememberLocked(int pid, size_t idx) {
    if (pid <= 0 || idx > UINT32_MAX) {
        return;
    }
    VProcTaskLookupCacheSet *set = &gVProcTaskLookupCache[vprocTaskLookupSlotForPid(pid)];
    VProcTaskLookupCacheEntry *way0 = &set->way[0];
    VProcTaskLookupCacheEntry *way1 = &set->way[1];
    if (way0->pid == pid) {
        way0->idx = (uint32_t)idx;
        return;
    }
    if (way1->pid == pid) {
        way1->idx = (uint32_t)idx;
        VProcTaskLookupCacheEntry tmp = *way0;
        *way0 = *way1;
        *way1 = tmp;
        return;
    }
    /* MRU insert into way0 and age the previous entry into way1. */
    *way1 = *way0;
    way0->pid = pid;
    way0->idx = (uint32_t)idx;
}

static inline void vprocTaskLookupForgetLocked(int pid) {
    if (pid <= 0) {
        return;
    }
    VProcTaskLookupCacheSet *set = &gVProcTaskLookupCache[vprocTaskLookupSlotForPid(pid)];
    for (size_t way = 0; way < VPROC_TASK_LOOKUP_CACHE_WAYS; ++way) {
        VProcTaskLookupCacheEntry *cache = &set->way[way];
        if (cache->pid == pid) {
            cache->pid = 0;
            cache->idx = 0;
        }
    }
}

/* -- Helper Functions -- */

static void vprocMaybeNotifyPgidEmptyLocked(int pgid) {
    if (pgid <= 0) {
        return;
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) {
            continue;
        }
        if (entry->pgid == pgid && !entry->exited) {
            return;
        }
    }
    if (PSCALRuntimeOnProcessGroupEmpty) {
        PSCALRuntimeOnProcessGroupEmpty(pgid);
    }
}

static void vprocSetCommLocked(VProcTaskEntry *entry, const char *label) {
    if (!entry) return;
    if (label && *label) {
        const char *start = label;
        while (*start && isspace((unsigned char)*start)) {
            start++;
        }
        const char *end = start;
        while (*end && !isspace((unsigned char)*end)) {
            end++;
        }
        const char *base = start;
        for (const char *p = start; p < end; ++p) {
            if (*p == '/') {
                base = p + 1;
            }
        }
        size_t len = (size_t)(end - base);
        if (len >= sizeof(entry->comm)) {
            len = sizeof(entry->comm) - 1;
        }
        memcpy(entry->comm, base, len);
        entry->comm[len] = '\0';
    } else {
        memset(entry->comm, 0, sizeof(entry->comm));
    }
}

static bool vprocEntryIsKernel(const VProcTaskEntry *entry) {
    if (!entry) {
        return false;
    }
    if (entry->label && strcmp(entry->label, "kernel") == 0) {
        return true;
    }
    return entry->comm[0] != '\0' && strcmp(entry->comm, "kernel") == 0;
}

static bool vprocSoftSignalingDisabledByEnv(void) {
    const char *flag = getenv("PSCALI_DISABLE_SOFT_SIGNALING");
    if (!flag || !*flag) {
        return false;
    }
    return strcmp(flag, "0") != 0;
}

static bool vprocCommandLooksRemoteClient(const char *label) {
    if (!label || !*label) {
        return false;
    }
    const char *start = label;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    const char *end = start;
    while (*end && !isspace((unsigned char)*end)) {
        end++;
    }
    const char *base = start;
    for (const char *p = start; p < end; ++p) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    size_t len = (size_t)(end - base);
    /* ssh isn't here: ^C belongs to the remote end only when ssh has a terminal
     * there, and smallclue then sets the session's passthrough flag. Without one
     * ^C has to stop it, whichever way it was started. */
    if (len == 3 &&
        tolower((unsigned char)base[0]) == 's' &&
        tolower((unsigned char)base[1]) == 'c' &&
        tolower((unsigned char)base[2]) == 'p') {
        return true;
    }
    if (len == 4 &&
        tolower((unsigned char)base[0]) == 's' &&
        tolower((unsigned char)base[1]) == 'f' &&
        tolower((unsigned char)base[2]) == 't' &&
        tolower((unsigned char)base[3]) == 'p') {
        return true;
    }
    return false;
}

static bool vprocEntryPrefersControlBytePassthrough(const VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->comm[0] && vprocCommandLooksRemoteClient(entry->comm)) {
        return true;
    }
    if (entry->label && entry->label[0] && vprocCommandLooksRemoteClient(entry->label)) {
        return true;
    }
    return false;
}

static bool vprocForegroundGroupPrefersControlBytes(int sid, int fg_pgid, int shell_pid, int sig) {
    (void)sig;
    if (fg_pgid <= 0) {
        return false;
    }
    bool passthrough = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0 || entry->zombie || entry->exited) {
            continue;
        }
        if (sid > 0 && entry->sid != sid) {
            continue;
        }
        if (entry->pgid != fg_pgid) {
            continue;
        }
        if (shell_pid > 0 && entry->pid == shell_pid) {
            continue;
        }
        if (vprocEntryPrefersControlBytePassthrough(entry)) {
            passthrough = true;
            break;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return passthrough;
}

static bool vprocSessionPrefersControlBytePassthrough(VProcSessionStdio *session) {
    if (!session) {
        return false;
    }
    if (session->control_bytes_passthrough) {
        return true;
    }
    if (session->session_id == 0) {
        return false;
    }
    return vprocSessionGetControlBytePassthrough(session->session_id);
}

static bool vprocGetSessionJobControlState(VProcSessionStdio *session,
                                           int *shell_pid_out,
                                           int *shell_pgid_out,
                                           int *sid_out,
                                           int *fg_pgid_out) {
    int shell_pid = -1;
    int shell_pgid = -1;
    int sid = -1;
    int fg_pgid = -1;

    if (!session || vprocSessionStdioIsDefault(session)) {
        return vprocGetShellJobControlState(shell_pid_out,
                                            shell_pgid_out,
                                            sid_out,
                                            fg_pgid_out);
    }

    if (session->shell_pid > 0) {
        shell_pid = session->shell_pid;
    }
    if (session->session_id != 0) {
        pthread_mutex_lock(&gVProcSessionPtys.mu);
        VProcSessionPtyEntry *session_entry = vprocSessionPtyFindLocked(session->session_id, NULL);
        if (session_entry) {
            if (shell_pid <= 0 && session_entry->shell_pid > 0) {
                shell_pid = session_entry->shell_pid;
            }
            if (session_entry->pty_slave && session_entry->pty_slave->tty) {
                struct tty *tty = session_entry->pty_slave->tty;
                lock(&tty->lock);
                sid = (int)tty->session;
                fg_pgid = (int)tty->fg_group;
                unlock(&tty->lock);
            }
        }
        pthread_mutex_unlock(&gVProcSessionPtys.mu);
    }

    if (shell_pid > 0) {
        pthread_mutex_lock(&gVProcTasks.mu);
        VProcTaskEntry *shell_entry = vprocTaskFindLocked(shell_pid);
        if (shell_entry) {
            shell_pgid = shell_entry->pgid;
            if (sid <= 0) {
                sid = shell_entry->sid;
            }
        }
        if (sid > 0 && fg_pgid <= 0) {
            fg_pgid = vprocForegroundPgidLocked(sid);
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
    }

    if (fg_pgid <= 0) {
        fg_pgid = shell_pgid;
    }
    if (shell_pid_out) {
        *shell_pid_out = shell_pid;
    }
    if (shell_pgid_out) {
        *shell_pgid_out = shell_pgid;
    }
    if (sid_out) {
        *sid_out = sid;
    }
    if (fg_pgid_out) {
        *fg_pgid_out = fg_pgid;
    }
    return shell_pid > 0;
}

static bool vprocPrepareThreadNameLocked(const VProcTaskEntry *entry, char *name, size_t name_len) {
    if (!entry || entry->pid <= 0 || entry->tid == 0 || !name || name_len == 0) {
        return false;
    }
    if (!pthread_equal(entry->tid, pthread_self())) {
        return false;
    }
    if (vprocEntryIsKernel(entry)) {
        snprintf(name, name_len, "kernel");
        return true;
    }
    const char *base = entry->comm[0] ? entry->comm :
                       ((entry->label && entry->label[0]) ? entry->label : "vproc");
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "-%d", entry->pid);
    size_t suffix_len = strlen(suffix);
    size_t base_cap = (suffix_len + 1 < name_len) ? (name_len - suffix_len - 1) : 0;
    if (base_cap > 0) {
        snprintf(name, name_len, "%.*s%s", (int)base_cap, base, suffix);
    } else {
        snprintf(name, name_len, "%s", suffix);
    }
    return true;
}

static void vprocApplyThreadName(const char *name) {
    if (!name || !name[0]) {
        return;
    }
#if defined(__APPLE__)
    if (gVProcTlsReady) {
        vprocInterposeBypassEnter();
        pthread_setname_np(name);
        vprocInterposeBypassExit();
    } else {
        pthread_setname_np(name);
    }
#else
    if (gVProcTlsReady) {
        vprocInterposeBypassEnter();
        pthread_setname_np(pthread_self(), name);
        vprocInterposeBypassExit();
    } else {
        pthread_setname_np(pthread_self(), name);
    }
#endif
}

static void vprocNotifyPidSigchldLocked(int target_pid, VProcSigchldEvent evt);
static void vprocNotifyParentSigchldLocked(const VProcTaskEntry *child, VProcSigchldEvent evt);
static struct sigaction vprocGetSigactionLocked(VProcTaskEntry *entry, int sig);
static int vprocDefaultParentPid(void) {
    VProc *active = vprocCurrent();
    if (active) {
        int active_pid = vprocPid(active);
        if (active_pid > 0) {
            return active_pid;
        }
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        return shell_pid;
    }
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
    /* Test helper threads often create synthetic tasks without first
     * initialising shell/kernel synthetic ids. Fall back to host pid so
     * waitpid parent matching remains deterministic across threads. */
    int host_pid = (int)vprocHostGetpidRaw();
    if (host_pid > 0) {
        return host_pid;
    }
#endif
    int kernel = vprocGetKernelPid();
    if (kernel > 0) {
        return kernel;
    }
    return 0;
}

static int vprocAdoptiveParentPidLocked(const VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0) {
        return 0;
    }
    if (entry->sid > 0 && entry->sid != entry->pid) {
        VProcTaskEntry *sid_entry = vprocTaskFindLocked(entry->sid);
        if (sid_entry) {
            return entry->sid;
        }
    }
    int kernel = vprocGetKernelPid();
    if (kernel > 0 && kernel != entry->pid) {
        return kernel;
    }
    int shell = vprocGetShellSelfPid();
    if (shell > 0 && shell != entry->pid) {
        return shell;
    }
    int host = (int)vprocHostGetpidRaw();
    if (host > 0 && host != entry->pid) {
        return host;
    }
    return 0;
}

static inline uint32_t vprocSigMask(int sig) {
    if (sig <= 0 || sig >= 32) return 0;
    return (1u << sig);
}

int vprocNextJobIdSeed(void) {
    static int next_job_id = 1;
    return __sync_fetch_and_add(&next_job_id, 1);
}

static int vprocCentiFromMicros(int64_t micros) {
    if (micros <= 0) {
        return 0;
    }
    int64_t centi = micros / 10000;
    if (centi > INT_MAX) {
        return INT_MAX;
    }
    return (int)centi;
}

static bool vprocThreadUsageMicros(pthread_t tid, int64_t *user_us, int64_t *system_us) {
#if defined(__APPLE__)
    /* A thread that ended and was not joined still gives the name its port had, and that name
     * soon comes back for some other right. thread_info on an ordinary port ends the process,
     * so ask the kernel first whether the name is a thread's. The name isn't ours to release:
     * this call adds no reference, and a release takes another owner's right. */
    if (pthread_kill(tid, 0) != 0) {
        return false;
    }
    thread_t thread_port = pthread_mach_thread_np(tid);
    if (thread_port == MACH_PORT_NULL) {
        return false;
    }
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t kr = thread_info(thread_port, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    if (user_us) {
        *user_us = (int64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds;
    }
    if (system_us) {
        *system_us = (int64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
    }
    return true;
#else
    (void)tid;
    if (user_us) *user_us = 0;
    if (system_us) *system_us = 0;
    return false;
#endif
}

static bool vprocComputeCpuTimesLocked(const VProcTaskEntry *entry, int *utime_cs, int *stime_cs) {
    if (!entry) {
        return false;
    }
    int64_t user_total = 0;
    int64_t system_total = 0;
    bool saw = false;
    if (entry->thread_count == 0) {
        int64_t user_us = 0;
        int64_t system_us = 0;
        if (vprocThreadUsageMicros(entry->tid, &user_us, &system_us)) {
            user_total += user_us;
            system_total += system_us;
            saw = true;
        }
    } else {
        for (size_t i = 0; i < entry->thread_count; ++i) {
            pthread_t tid = entry->threads[i];
            int64_t user_us = 0;
            int64_t system_us = 0;
            if (vprocThreadUsageMicros(tid, &user_us, &system_us)) {
                user_total += user_us;
                system_total += system_us;
                saw = true;
            }
        }
    }
    if (!saw) {
        return false;
    }
    if (utime_cs) {
        *utime_cs = vprocCentiFromMicros(user_total);
    }
    if (stime_cs) {
        *stime_cs = vprocCentiFromMicros(system_total);
    }
    return true;
}

static uint64_t vprocNowMonoNs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

static int vprocTimespecCmp(const struct timespec *a, const struct timespec *b) {
    if (!a || !b) {
        return 0;
    }
    if (a->tv_sec < b->tv_sec) {
        return -1;
    }
    if (a->tv_sec > b->tv_sec) {
        return 1;
    }
    if (a->tv_nsec < b->tv_nsec) {
        return -1;
    }
    if (a->tv_nsec > b->tv_nsec) {
        return 1;
    }
    return 0;
}

static bool vprocRealtimeDeadlineFromMonoNs(uint64_t mono_deadline_ns, struct timespec *out_abs_rt) {
    if (!out_abs_rt) {
        return false;
    }
    struct timespec mono_now;
    struct timespec real_now;
    if (clock_gettime(CLOCK_MONOTONIC, &mono_now) != 0) {
        return false;
    }
    if (clock_gettime(CLOCK_REALTIME, &real_now) != 0) {
        return false;
    }
    uint64_t mono_now_ns = (uint64_t)mono_now.tv_sec * 1000000000ull + (uint64_t)mono_now.tv_nsec;
    uint64_t delta_ns = 0;
    if (mono_deadline_ns > mono_now_ns) {
        delta_ns = mono_deadline_ns - mono_now_ns;
    }
    uint64_t sec_add = delta_ns / 1000000000ull;
    uint64_t nsec_add = delta_ns % 1000000000ull;
    time_t sec = real_now.tv_sec;
    long nsec = real_now.tv_nsec;
    if (sec_add > (uint64_t)LONG_MAX) {
        sec = (time_t)LONG_MAX;
        nsec = 999999999L;
    } else {
        sec = (time_t)(sec + (time_t)sec_add);
        nsec = nsec + (long)nsec_add;
        if (nsec >= 1000000000L) {
            sec += 1;
            nsec -= 1000000000L;
        }
    }
    out_abs_rt->tv_sec = sec;
    out_abs_rt->tv_nsec = nsec;
    return true;
}

static bool vprocTimevalValid(const struct timeval *tv) {
    if (!tv) {
        return false;
    }
    if (tv->tv_sec < 0) {
        return false;
    }
    if (tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        return false;
    }
    return true;
}

static uint64_t vprocTimevalToNs(const struct timeval *tv) {
    if (!tv) {
        return 0;
    }
    return (uint64_t)tv->tv_sec * 1000000000ull + (uint64_t)tv->tv_usec * 1000ull;
}

static void vprocNsToTimeval(uint64_t ns, struct timeval *tv) {
    if (!tv) {
        return;
    }
    tv->tv_sec = (time_t)(ns / 1000000000ull);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ull) / 1000ull);
}

static unsigned int vprocNsToSecondsCeil(uint64_t ns) {
    if (ns == 0) {
        return 0;
    }
    uint64_t seconds = (ns + 1000000000ull - 1ull) / 1000000000ull;
    if (seconds > (uint64_t)UINT_MAX) {
        return UINT_MAX;
    }
    return (unsigned int)seconds;
}

static uint64_t vprocTaskRealTimerRemainingNsLocked(const VProcTaskEntry *entry, uint64_t now_ns) {
    if (!entry || !entry->real_timer_active || entry->real_timer_deadline_ns == 0) {
        return 0;
    }
    if (entry->real_timer_deadline_ns <= now_ns) {
        return 0;
    }
    return entry->real_timer_deadline_ns - now_ns;
}

static bool vprocTaskExpireRealTimerLocked(VProcTaskEntry *entry, uint64_t now_ns) {
    if (!entry || !entry->real_timer_active || entry->real_timer_deadline_ns == 0) {
        return false;
    }
    if (entry->real_timer_deadline_ns > now_ns) {
        return false;
    }
    vprocQueuePendingSignalLocked(entry, SIGALRM);
    if (entry->real_timer_interval_ns > 0) {
        uint64_t interval = entry->real_timer_interval_ns;
        uint64_t elapsed = now_ns - entry->real_timer_deadline_ns;
        uint64_t ticks = (elapsed / interval) + 1u;
        uint64_t advance = interval;
        if (ticks > 1u) {
            if (ticks > UINT64_MAX / interval) {
                advance = interval;
            } else {
                advance = ticks * interval;
            }
        }
        if (UINT64_MAX - entry->real_timer_deadline_ns < advance) {
            entry->real_timer_deadline_ns = now_ns + interval;
        } else {
            entry->real_timer_deadline_ns += advance;
        }
    } else {
        entry->real_timer_active = false;
        entry->real_timer_deadline_ns = 0;
        entry->real_timer_interval_ns = 0;
    }
    return true;
}

static void vprocTaskFillRealTimerLocked(const VProcTaskEntry *entry, uint64_t now_ns, struct itimerval *out_timer) {
    if (!out_timer) {
        return;
    }
    memset(out_timer, 0, sizeof(*out_timer));
    if (!entry || !entry->real_timer_active) {
        return;
    }
    uint64_t remain_ns = vprocTaskRealTimerRemainingNsLocked(entry, now_ns);
    vprocNsToTimeval(remain_ns, &out_timer->it_value);
    vprocNsToTimeval(entry->real_timer_interval_ns, &out_timer->it_interval);
}

static void vprocTaskSetRealTimerLocked(VProcTaskEntry *entry, const struct itimerval *new_timer, uint64_t now_ns) {
    if (!entry || !new_timer) {
        return;
    }
    uint64_t interval_ns = vprocTimevalToNs(&new_timer->it_interval);
    uint64_t value_ns = vprocTimevalToNs(&new_timer->it_value);
    if (value_ns == 0) {
        entry->real_timer_active = false;
        entry->real_timer_deadline_ns = 0;
        entry->real_timer_interval_ns = interval_ns;
        return;
    }
    entry->real_timer_active = true;
    entry->real_timer_interval_ns = interval_ns;
    if (UINT64_MAX - now_ns < value_ns) {
        entry->real_timer_deadline_ns = UINT64_MAX;
    } else {
        entry->real_timer_deadline_ns = now_ns + value_ns;
    }
}

static int vprocClampTimeoutByCurrentRealTimer(VProc *vp, int timeout_ms, bool *timer_limited) {
    if (timer_limited) {
        *timer_limited = false;
    }
    if (!vp || timeout_ms == 0) {
        return timeout_ms;
    }
    int pid = vprocPid(vp);
    if (pid <= 0) {
        return timeout_ms;
    }
    uint64_t remain_ns = 0;
    bool has_timer = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->real_timer_active && entry->real_timer_deadline_ns > 0) {
        uint64_t now_ns = vprocNowMonoNs();
        remain_ns = vprocTaskRealTimerRemainingNsLocked(entry, now_ns);
        has_timer = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (!has_timer) {
        return timeout_ms;
    }
    int timer_ms = 0;
    if (remain_ns > 0) {
        uint64_t rounded_ms = (remain_ns + 1000000ull - 1ull) / 1000000ull;
        if (rounded_ms == 0) {
            rounded_ms = 1;
        }
        if (rounded_ms > (uint64_t)INT_MAX) {
            timer_ms = INT_MAX;
        } else {
            timer_ms = (int)rounded_ms;
        }
    }
    if (timeout_ms < 0 || timer_ms < timeout_ms) {
        if (timer_limited) {
            *timer_limited = true;
        }
        return timer_ms;
    }
    return timeout_ms;
}

static int vprocWaitUntilTimerOrSignalLocked(VProcTaskEntry *entry,
                                             const struct timespec *abs_deadline_rt,
                                             bool *timeout_hit) {
    if (timeout_hit) {
        *timeout_hit = false;
    }
    struct timespec timer_deadline_rt;
    const struct timespec *wait_abs = abs_deadline_rt;
    if (entry && entry->real_timer_active && entry->real_timer_deadline_ns > 0) {
        if (vprocRealtimeDeadlineFromMonoNs(entry->real_timer_deadline_ns, &timer_deadline_rt)) {
            if (!wait_abs || vprocTimespecCmp(&timer_deadline_rt, wait_abs) < 0) {
                wait_abs = &timer_deadline_rt;
            }
        }
    }
    if (!wait_abs) {
        return vprocCondWait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
    int rc = vprocCondTimedWait(&gVProcTasks.cv, &gVProcTasks.mu, wait_abs);
    if (rc == ETIMEDOUT && timeout_hit) {
        *timeout_hit = true;
    }
    return rc;
}

static inline bool vprocSigIndexValid(int sig) {
    return sig > 0 && sig < 32;
}

static inline bool vprocSignalBlockable(int sig) {
    return !(sig == SIGKILL || sig == SIGSTOP);
}

static inline bool vprocSignalIgnorable(int sig) {
    return vprocSignalBlockable(sig);
}

static void vprocInitEntryDefaultsLocked(VProcTaskEntry *entry, int pid, const VProcTaskEntry *parent) {
    /* Whose it is, for kill's check, until a vproc says: the caller's, or the one real user's. */
    VProc *cred_source = vprocCurrent();
    uid_t cred_ruid = cred_source ? cred_source->ruid : getuid();
    uid_t cred_euid = cred_source ? cred_source->euid : getuid();
    uid_t cred_suid = cred_source ? cred_source->suid : getuid();
    const VProcTaskEntry *inherit_parent = NULL;
    VProc *active = vprocCurrent();
    int shell_pid = vprocGetShellSelfPid();
    if (parent) {
        if ((active && vprocPid(active) == parent->pid) ||
            (shell_pid > 0 && parent->pid == shell_pid)) {
            inherit_parent = parent;
        }
    }
    if (!inherit_parent && (!parent || vprocEntryIsKernel(parent))) {
        int candidate_pid = 0;
        if (active) {
            candidate_pid = vprocPid(active);
        } else if (shell_pid > 0) {
            candidate_pid = shell_pid;
        }
        if (candidate_pid > 0) {
            VProcTaskEntry *candidate = vprocTaskFindLocked(candidate_pid);
            if (candidate) {
                inherit_parent = candidate;
            }
        }
    }
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    entry->stop_unsupported = false;
    entry->sigchld_delivery_queued = false;
    entry->shell_prompt_read_active = false;
    entry->pid = pid;
    entry->pgid = pid;
    entry->sid = pid;
    entry->session_leader = false;
    entry->fg_pgid = pid;
    entry->sigchld_blocked = false;
    /* Job ids are assigned explicitly by the shell; never inherit them. */
    entry->job_id = 0;
    for (int i = 0; i < 32; ++i) {
        sigemptyset(&entry->actions[i].sa_mask);
        entry->actions[i].sa_handler = SIG_DFL;
        entry->actions[i].sa_flags = 0;
    }
    if (inherit_parent) {
        if (inherit_parent->sid > 0) entry->sid = inherit_parent->sid;
        if (inherit_parent->pgid > 0) entry->pgid = inherit_parent->pgid;
        if (inherit_parent->fg_pgid > 0) entry->fg_pgid = inherit_parent->fg_pgid;
        entry->blocked_signals = inherit_parent->blocked_signals &
                                 ~(vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP));
        entry->ignored_signals = inherit_parent->ignored_signals & ~(vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP));
        entry->sigchld_blocked = inherit_parent->sigchld_blocked;
        entry->gave_up_terminal = inherit_parent->gave_up_terminal;
        memcpy(entry->actions, inherit_parent->actions, sizeof(entry->actions));
        if (shell_pid > 0 && inherit_parent->pid == shell_pid) {
            int job_sigs[] = { SIGTSTP, SIGTTIN, SIGTTOU };
            size_t sig_count = sizeof(job_sigs) / sizeof(job_sigs[0]);
            for (size_t i = 0; i < sig_count; ++i) {
                int sig = job_sigs[i];
                if (!vprocSigIndexValid(sig)) {
                    continue;
                }
                entry->blocked_signals &= ~vprocSigMask(sig);
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_handler = SIG_DFL;
                entry->actions[sig].sa_flags = 0;
                entry->ignored_signals &= ~vprocSigMask(sig);
            }
        }
    }
    entry->start_mono_ns = vprocNowMonoNs();
    entry->cred_ruid = cred_ruid;
    entry->cred_euid = cred_euid;
    entry->cred_suid = cred_suid;
}

static bool vprocAddChildLocked(VProcTaskEntry *parent, int child_pid) {
    if (!parent || parent->pid <= 0 || child_pid <= 0 || parent->pid == child_pid) {
        return false;
    }
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (parent->children[i] == child_pid) {
            return true;
        }
    }
    if (parent->child_count + 1 > parent->child_capacity) {
        size_t new_cap = parent->child_capacity ? parent->child_capacity : 4;
        while (new_cap < parent->child_count + 1) {
            if (new_cap > SIZE_MAX / 2) {
                return false;
            }
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(int)) {
            return false;
        }
        int *resized = realloc(parent->children, new_cap * sizeof(int));
        if (!resized) {
            return false;
        }
        parent->children = resized;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child_pid;
    return true;
}

static bool vprocSignalBlockedLocked(VProcTaskEntry *entry, int sig) {
    if (!vprocSignalBlockable(sig)) {
        return false;
    }
    uint32_t mask = vprocSigMask(sig);
    return mask != 0 && (entry->blocked_signals & mask);
}

static bool vprocSignalIgnoredLocked(VProcTaskEntry *entry, int sig) {
    if (!vprocSignalIgnorable(sig)) {
        return false;
    }
    if (vprocSigIndexValid(sig) && entry->actions[sig].sa_handler == SIG_IGN) {
        return true;
    }
    uint32_t mask = vprocSigMask(sig);
    return mask != 0 && (entry->ignored_signals & mask);
}

static void vprocMaybeStampRusageLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    int utime = 0;
    int stime = 0;
    if (vprocComputeCpuTimesLocked(entry, &utime, &stime)) {
        if (utime > entry->rusage_utime) {
            entry->rusage_utime = utime;
        }
        if (stime > entry->rusage_stime) {
            entry->rusage_stime = stime;
        }
        return;
    }
    /* Nothing measured is no CPU time. Time alive stood in here, a tenth of it as system time,
     * so a row with no thread to measure looked like it was spinning for as long as it lived. */
}

/* What delivering a signal to a virtual process does, once blocking has been
 * ruled out. */
typedef enum {
    VPROC_SIG_IGNORE,   /* discard the signal */
    VPROC_SIG_STOP,     /* put the process in the stopped state */
    VPROC_SIG_CONT,     /* resume a stopped process */
    VPROC_SIG_KILL,     /* terminate the process */
    VPROC_SIG_HANDLER   /* run an installed handler */
} VProcSignalAction;

/* The action a signal has while its disposition is SIG_DFL (POSIX, and
 * Darwin's signal(3) for the BSD-specific ones). */
static VProcSignalAction vprocDefaultSignalAction(int sig) {
    switch (sig) {
#ifdef SIGCHLD
        case SIGCHLD:
#endif
#ifdef SIGWINCH
        case SIGWINCH:
#endif
#ifdef SIGURG
        case SIGURG:
#endif
#ifdef SIGIO
        case SIGIO:
#endif
            return VPROC_SIG_IGNORE;
        case SIGCONT:
            return VPROC_SIG_CONT;
        case SIGTSTP:
        case SIGSTOP:
        case SIGTTIN:
        case SIGTTOU:
            return VPROC_SIG_STOP;
        default:
            return VPROC_SIG_KILL;
    }
}

/* The action delivering `sig` to `entry` takes, given the disposition it has
 * installed. Callers hold gVProcTasks.mu. */
static VProcSignalAction vprocEffectiveSignalActionLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return vprocDefaultSignalAction(sig);
    }
    if (entry->actions[sig].sa_handler == SIG_IGN) {
        return VPROC_SIG_IGNORE;
    }
    if (entry->actions[sig].sa_handler == SIG_DFL) {
        return vprocDefaultSignalAction(sig);
    }
    return VPROC_SIG_HANDLER;
}

static bool vprocEntryIsCurrentThreadLocked(const VProcTaskEntry *entry) {
    if (!entry) {
        return false;
    }
    pthread_t self = pthread_self();
    if (entry->tid && pthread_equal(entry->tid, self)) {
        return true;
    }
    for (size_t i = 0; i < entry->thread_count; ++i) {
        if (pthread_equal(entry->threads[i], self)) {
            return true;
        }
    }
    return false;
}

/* Whose handler this thread is running. */
static _Thread_local int gVprocHandlerTask = 0;

int vprocHandlerTaskPid(void) {
    return gVprocHandlerTask;
}

static void vprocInvokeHandlerLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return;
    }
    struct sigaction sa = vprocGetSigactionLocked(entry, sig);
    if (sa.sa_handler == SIG_IGN || sa.sa_handler == SIG_DFL) {
        return;
    }
    int saved_blocked = entry->blocked_signals;
    if (!(sa.sa_flags & SA_NODEFER)) {
        entry->blocked_signals |= vprocSigMask(sig);
    }
    /* Apply handler mask. */
    for (int s = 1; s < 32; ++s) {
        if (sigismember(&sa.sa_mask, s)) {
            entry->blocked_signals |= vprocSigMask(s);
        }
    }
    int outer_task = gVprocHandlerTask;
    gVprocHandlerTask = entry->pid;
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (sa.sa_flags & SA_SIGINFO) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code = SI_USER;
        info.si_pid = entry->parent_pid;
        sa.sa_sigaction(sig, &info, NULL);
    } else {
        sa.sa_handler(sig);
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    gVprocHandlerTask = outer_task;
    entry->blocked_signals = saved_blocked;
}

static void vprocQueuePendingSignalLocked(VProcTaskEntry *entry, int sig) {
    uint32_t mask = vprocSigMask(sig);
    if (mask != 0) {
        entry->pending_signals |= mask;
        if (sig > 0 && sig < 32) {
            if (entry->pending_counts[sig] < INT_MAX) {
                entry->pending_counts[sig]++;
            }
        }
#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)
        /* A task waiting on a terminal looks at what came (L10). Takes no terminal lock. */
        pscalTtyWakeForSignal();
#endif
    }
}

static bool vprocConsumePendingSignalLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return false;
    }
    uint32_t mask = vprocSigMask(sig);
    if (mask == 0) {
        return false;
    }
    if (!(entry->pending_signals & mask) && entry->pending_counts[sig] <= 0) {
        return false;
    }
    if (entry->pending_counts[sig] > 0) {
        entry->pending_counts[sig]--;
    }
    if (entry->pending_counts[sig] <= 0) {
        entry->pending_signals &= ~mask;
        entry->pending_counts[sig] = 0;
    }
    return true;
}

static bool vprocCurrentIsEntryLocked(const VProcTaskEntry *entry);

static void vprocApplySignalLocked(VProcTaskEntry *entry, int sig) {
    VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);

    if (vprocSignalIgnoredLocked(entry, sig) || action == VPROC_SIG_IGNORE) {
        return;
    }
    if (action == VPROC_SIG_HANDLER) {
        struct sigaction sa = vprocGetSigactionLocked(entry, sig);
        if (sa.sa_flags & SA_RESETHAND) {
            entry->actions[sig].sa_handler = SIG_DFL;
            entry->actions[sig].sa_flags = 0;
            sigemptyset(&entry->actions[sig].sa_mask);
            entry->ignored_signals &= ~vprocSigMask(sig);
        }
        if (sig != SIGCONT) {
            entry->continued = false;
        }
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        vprocInvokeHandlerLocked(entry, sig);
        return;
    }
    if (action == VPROC_SIG_STOP) {
        if (entry->stop_unsupported) {
            vprocQueuePendingSignalLocked(entry, sig);
            return;
        }
        entry->stopped = true;
        entry->continued = false;
        entry->exited = false;
        entry->stop_signo = sig;
        entry->exit_signal = 0;
        entry->status = 128 + sig;
        entry->zombie = false;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_STOP);
    } else if (action == VPROC_SIG_CONT) {
        entry->stopped = false;
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        entry->continued = true;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_CONT);
    } else if (sig > 0) {
        entry->status = entry->status & 0xff;
        entry->exit_signal = sig;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        if ((entry->tid || entry->thread_count > 0) &&
            (!vprocEntryIsCurrentThreadLocked(entry) || vprocCurrentIsEntryLocked(entry))) {
            /* Its thread is still running, this one too if the task has just ended itself, and holds
             * what the task holds until it has unwound. Whoever waits for the task finds out from
             * vprocMarkExit, once that is over. */
            if (entry->dying_since_ns == 0) {
                entry->dying_since_ns = vprocNowMonoNs();
            }
            return;
        }
        entry->exited = true;
        entry->zombie = true;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);
    }
}

static VProcTaskEntry *vprocSessionLeaderBySidLocked(int sid) {
    if (sid <= 0) {
        return NULL;
    }
    VProcTaskEntry *leader = vprocTaskFindLocked(sid);
    if (leader && leader->sid == sid && leader->session_leader) {
        return leader;
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) {
            continue;
        }
        if (entry->sid == sid && entry->session_leader) {
            gVProcTaskFindHint = i;
            vprocTaskLookupRememberLocked(entry->pid, i);
            return entry;
        }
    }
    return NULL;
}

static int vprocForegroundPgidLocked(int sid) {
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    return leader ? leader->fg_pgid : -1;
}

static bool vprocShouldStopForBackgroundTty(VProc *vp, int sig) {
    if (!vp) {
        return false;
    }
    bool stopped = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    if (entry && entry->stop_unsupported) {
        /* Pipelines and synthetic tasks that disable stops should never be parked. */
        pthread_mutex_unlock(&gVProcTasks.mu);
        return false;
    }
    if (entry && entry->sid > 0) {
        int fg = vprocForegroundPgidLocked(entry->sid);
        if (fg > 0 && entry->pgid != fg) {
            vprocApplySignalLocked(entry, sig);
            pthread_cond_broadcast(&gVProcTasks.cv);
            stopped = true;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return stopped;
}

static void vprocDebugLogf(const char *format, ...);

static bool vprocStopWaitDebugEnabled(void) {
    const char *env = getenv("PSCALI_STOPWAIT_DEBUG");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        return false;
    }
    return true;
}

/* What the VM's check before each instruction found, for what is on a thread it can't cancel. */
static _Thread_local bool gVprocKilledHere = false;

bool vprocTaskKilledHere(void) {
    return gVprocKilledHere;
}

bool vprocWaitIfStopped(VProc *vp) {
    /* Each call answers for the task it was asked about, the shell's included. */
    gVprocKilledHere = false;
    if (!vp) {
        return false;
    }
    int pid = vprocPid(vp);
    if (pid <= 0) {
        return false;
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0 && pid == shell_pid) {
        return false;
    }
    bool waited = false;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    gVprocKilledHere = entry && entry->dying_since_ns != 0;
    if (entry &&
        entry->stop_unsupported &&
        entry->cooperative_stop_wait &&
        !entry->stopped &&
        !entry->exited) {
        int stop_sig = 0;
        const int stop_sigs[] = { SIGTSTP, SIGSTOP, SIGTTIN, SIGTTOU };
        for (size_t i = 0; i < sizeof(stop_sigs) / sizeof(stop_sigs[0]); ++i) {
            int candidate = stop_sigs[i];
            if (!vprocConsumePendingSignalLocked(entry, candidate)) {
                continue;
            }
            stop_sig = candidate;
            break;
        }
        if (stop_sig > 0) {
            entry->stopped = true;
            entry->continued = false;
            entry->exited = false;
            entry->stop_signo = stop_sig;
            entry->exit_signal = 0;
            entry->status = 128 + stop_sig;
            entry->zombie = false;
            vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_STOP);
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
    }
    if (entry && entry->stopped && !entry->exited &&
        entry->tid && pthread_equal(entry->tid, self)) {
        /*
         * Only the shell thread needs cooperative conversion to avoid locking
         * the prompt. Worker threads should remain truly stopped so bg/fg can
         * resume them later.
         */
        if (vprocIsShellSelfThread()) {
            int stop_sig = entry->stop_signo;
            if (stop_sig <= 0 || stop_sig >= 32) {
                stop_sig = SIGTSTP;
            }
            entry->pending_signals |= vprocSigMask(stop_sig);
            entry->stopped = false;
            entry->continued = true;
            entry->stop_signo = 0;
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
    }
    if (entry && entry->stop_unsupported && !entry->cooperative_stop_wait && entry->stopped) {
        /* Convert hard-stop state into a cooperative pending signal so
         * in-process loops can unwind to shell instead of parking forever. */
        int stop_sig = entry->stop_signo;
        if (stop_sig <= 0 || stop_sig >= 32) {
            stop_sig = SIGTSTP;
        }
        entry->pending_signals |= vprocSigMask(stop_sig);
        entry->stopped = false;
        entry->continued = true;
        entry->stop_signo = 0;
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    if (entry && entry->stop_unsupported && !entry->cooperative_stop_wait) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return false;
    }
    bool stop_wait_debug = vprocStopWaitDebugEnabled();
    uint64_t wait_start_ns = 0;
    uint64_t last_log_ns = 0;
    bool should_wait = (entry && entry->stopped && !entry->exited);
    if (should_wait && stop_wait_debug) {
        wait_start_ns = vprocNowMonoNs();
        last_log_ns = wait_start_ns;
        vprocDebugLogf("[vproc-stopwait] begin pid=%d sid=%d pgid=%d stop_sig=%d stop_unsup=%d coop=%d pending=0x%08x tid=%p",
                       pid,
                       entry->sid,
                       entry->pgid,
                       entry->stop_signo,
                       entry->stop_unsupported ? 1 : 0,
                       entry->cooperative_stop_wait ? 1 : 0,
                       entry->pending_signals,
                       (void*)entry->tid);
    }
    if (should_wait && gVprocStopWaitBeforeHook) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        gVprocStopWaitBeforeHook();
        pthread_mutex_lock(&gVProcTasks.mu);
        entry = vprocTaskFindLocked(pid);
    }
    while (entry && entry->stopped && !entry->exited) {
        waited = true;
        if (stop_wait_debug) {
            struct timespec deadline;
            if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
                deadline.tv_sec += 1;
                int wait_rc = vprocCondTimedWait(&gVProcTasks.cv, &gVProcTasks.mu, &deadline);
                if (wait_rc == ETIMEDOUT && entry && entry->stopped && !entry->exited) {
                    uint64_t now_ns = vprocNowMonoNs();
                    if (now_ns >= last_log_ns + 1000000000ull) {
                        uint64_t waited_ms = wait_start_ns > 0 ? (now_ns - wait_start_ns) / 1000000ull : 0;
                        vprocDebugLogf("[vproc-stopwait] heartbeat pid=%d sid=%d pgid=%d stop_sig=%d waited_ms=%" PRIu64 " pending=0x%08x",
                                       pid,
                                       entry->sid,
                                       entry->pgid,
                                       entry->stop_signo,
                                       waited_ms,
                                       entry->pending_signals);
                        last_log_ns = now_ns;
                    }
                }
            } else {
                vprocCondWait(&gVProcTasks.cv, &gVProcTasks.mu);
            }
        } else {
            vprocCondWait(&gVProcTasks.cv, &gVProcTasks.mu);
        }
        if (entry->pid != pid) {
            entry = vprocTaskFindLocked(pid);
        }
    }
    if (waited && stop_wait_debug) {
        uint64_t waited_ms = 0;
        if (wait_start_ns > 0) {
            uint64_t now_ns = vprocNowMonoNs();
            waited_ms = (now_ns - wait_start_ns) / 1000000ull;
        }
        vprocDebugLogf("[vproc-stopwait] end pid=%d waited_ms=%" PRIu64 " entry=%p stopped=%d exited=%d stop_sig=%d",
                       pid,
                       waited_ms,
                       (void*)entry,
                       (entry && entry->stopped) ? 1 : 0,
                       (entry && entry->exited) ? 1 : 0,
                       entry ? entry->stop_signo : 0);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (waited && gVprocStopWaitAfterHook) {
        gVprocStopWaitAfterHook();
    }
    return waited;
}

/* A VM worker runs under its frontend's vproc without being one of its threads. */
static bool vprocCurrentIsEntryLocked(const VProcTaskEntry *entry) {
    VProc *current = vprocCurrent();
    return current && vprocPid(current) == entry->pid;
}

static bool vprocDeliverPendingSignalsLocked(VProcTaskEntry *entry) {
    uint32_t pending = entry->pending_signals;
    bool exit_current = false;
    for (int sig = 1; sig < 32; ++sig) {
        uint32_t mask = vprocSigMask(sig);
        if (!(pending & mask)) continue;
        if (vprocSignalBlockedLocked(entry, sig)) continue;
        VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
        if (action == VPROC_SIG_IGNORE || vprocSignalIgnoredLocked(entry, sig)) {
            entry->pending_signals &= ~mask;
            entry->pending_counts[sig] = 0;
            continue;
        }
        if (action == VPROC_SIG_STOP && entry->stop_unsupported) {
            /* Cooperative-stop workers consume SIGTSTP via explicit polling
             * paths (vprocSigpending/vprocSigwait or runtime request checks).
             * Do not transform this into a hard stop here. */
            continue;
        }
        if (action == VPROC_SIG_KILL && (entry->unwinds_itself || entry->shell_task)) {
            /* vprocDeliverTaskSignals hands it to the task. */
            continue;
        }
        if (action == VPROC_SIG_HANDLER && (entry->tid || entry->thread_count > 0) &&
            !vprocEntryIsCurrentThreadLocked(entry) && !vprocCurrentIsEntryLocked(entry)) {
            /* This may be a child ending or the kernel thread. A handler belongs on its
             * task's thread, where the task's state and descriptors are. */
            continue;
        }
        vprocApplySignalLocked(entry, sig);
        /* If this is the current thread and the action was a terminating
         * default, arrange to self-terminate after we drop the lock. */
        if (action == VPROC_SIG_KILL && vprocEntryIsCurrentThreadLocked(entry) && vprocCurrentIsEntryLocked(entry) &&
            (entry->exited || entry->dying_since_ns != 0)) {
            exit_current = true;
        }
        entry->pending_signals &= ~mask;
        entry->pending_counts[sig] = 0;
    }
    return exit_current;
}

/*
 * True when the calling thread's vproc has a pending signal that should
 * interrupt a blocking wait (in-proc pipe read/write, poll/select). Mirrors
 * iSH's poll_wait recheck of `pending & ~blocked`, but also skips signals whose
 * effective disposition is ignore/no-op so we don't spuriously EINTR on
 * SIGCHLD/SIGWINCH and friends. Locks the task table; it is safe to call while
 * holding an unrelated pipe mutex because no path holds gVProcTasks.mu across a
 * pipe op (vprocInvokeHandlerLocked drops the table lock before running
 * handlers), so the order pipe->mu -> gVProcTasks.mu never inverts.
 */
static bool vprocCurrentHasInterruptingSignal(void) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return false;
    }
    int pid = vprocPid(vp);
    if (pid <= 0) {
        return false;
    }
    bool interrupt = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->dying_since_ns != 0) {
        interrupt = true;
    } else if (entry && entry->pending_signals) {
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t mask = vprocSigMask(sig);
            if (!(entry->pending_signals & mask)) {
                continue;
            }
            if (vprocSignalBlockedLocked(entry, sig)) {
                continue;
            }
            if (vprocSignalIgnoredLocked(entry, sig)) {
                continue;
            }
            VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
            if (action == VPROC_SIG_IGNORE) {
                continue;
            }
            if (action == VPROC_SIG_KILL && (entry->unwinds_itself || entry->shell_task)) {
                /* Stays queued until the task looks, so a retried read would only spin. */
                continue;
            }
            interrupt = true;
            break;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return interrupt;
}

/* The shell at its prompt: is a signal that ends it waiting? One stays queued with a task that
 * unwinds itself until the task looks, and a prompt read never looks on its own. */
static bool vprocPromptKillPending(int shell_pid) {
    if (shell_pid <= 0) {
        return false;
    }
    bool pending = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(shell_pid);
    if (entry && entry->unwinds_itself && entry->pending_signals) {
        for (int sig = 1; sig < 32 && !pending; ++sig) {
            if (!(entry->pending_signals & vprocSigMask(sig)) || vprocSignalBlockedLocked(entry, sig) ||
                vprocSignalIgnoredLocked(entry, sig)) {
                continue;
            }
            pending = vprocEffectiveSignalActionLocked(entry, sig) == VPROC_SIG_KILL;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pending;
}

/* For a wait in place of a host call: does a signal end it? A handler runs here, and one that
 * asked for SA_RESTART leaves the wait going, as the host call would have. A task being killed
 * stops waiting even where its thread cannot be cancelled yet, such as during setup. */
static __attribute__((unused)) bool vprocHostWaitInterrupted(void) {
    VProc *vp = vprocCurrent();
    int pid = vp ? vprocPid(vp) : 0;
    if (pid <= 0) {
        return false;
    }
    bool pending = vprocCurrentHasInterruptingSignal();
    bool interrupt = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        interrupt = entry->dying_since_ns != 0;
        for (int sig = 1; pending && !interrupt && sig < 32; ++sig) {
            if (!(entry->pending_signals & vprocSigMask(sig)) ||
                vprocSignalBlockedLocked(entry, sig) || vprocSignalIgnoredLocked(entry, sig)) {
                continue;
            }
            VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
            if (action != VPROC_SIG_IGNORE) {
                interrupt = action != VPROC_SIG_HANDLER || !(entry->actions[sig].sa_flags & SA_RESTART);
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (pending) {
        vprocDeliverPendingSignalsForCurrent();
    }
    return interrupt;
}

#if defined(PSCAL_TARGET_IOS)
static void vprocHostWaitCancelled(void *unused) {
    (void)unused;
    vprocInterposeBypassExit();
}

/* The host's read, write and open here are the nocancel ones, so a kill could not end a thread
 * inside them: it stayed there after its job was reported gone, with what the job held still
 * open. So wait first where a cancel can act, looking for signals now and then. In select, not
 * poll: on Darwin poll reports neither the end of a FIFO nor that a FIFO is full. */
/* For a wait on behalf of task PID, when the thread's current vproc may no longer be it: a
 * killed job's cleanup flushes its frontend's stream after the job's vproc is gone, and a wait
 * that asked the current vproc, by then the caller's, would wait for good on a stalled reader. */
static bool vprocHostWaitEndedFor(int pid) {
    VProc *vp = vprocCurrent();
    if (pid <= 0 || (vp && vprocPid(vp) == pid)) {
        return vprocHostWaitInterrupted();
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    bool ended = !entry || entry->exited || entry->zombie || entry->dying_since_ns != 0;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return ended;
}

static int vprocWaitHostFdFor(int host, bool for_write, int pid) {
    if (host < 0 || host >= FD_SETSIZE) {
        return 0;
    }
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(host, &fds);
        struct timeval slice = { 0, 100000 };
        int ready;
        pthread_cleanup_push(vprocHostWaitCancelled, NULL);
        ready = vprocHostSelectRaw(host + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &slice);
        pthread_cleanup_pop(0);
        if (ready > 0 || (ready < 0 && errno != EINTR)) {
            /* Ready, or nothing select can say about: the call itself decides. */
            return 0;
        }
        if (vprocHostWaitEndedFor(pid)) {
            pthread_testcancel();
            errno = EINTR;
            return -1;
        }
    }
}

static int vprocWaitHostFd(int host, bool for_write) {
    return vprocWaitHostFdFor(host, for_write, 0);
}

static bool vprocHostFdBlocks(int host) {
    int flags = fcntl(host, F_GETFL, 0);
    return flags >= 0 && !(flags & O_NONBLOCK);
}

/* A pipe or socket says it is writable with room for PIPE_BUF bytes, and a write of more could
 * then block where no cancel reaches it. A longer write was never one unit anyway. */
static ssize_t vprocHostWriteKillableFor(int host, const void *buf, size_t count, int pid) {
    struct stat st;
    if (count == 0 || !vprocHostFdBlocks(host) || vprocHostFstatRaw(host, &st) != 0 ||
        !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
        return vprocHostWrite(host, buf, count);
    }
    const char *bytes = (const char *)buf;
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done < PIPE_BUF ? count - done : PIPE_BUF;
        ssize_t wrote = vprocWaitHostFdFor(host, true, pid) < 0 ? -1 : vprocHostWrite(host, bytes + done, chunk);
        if (wrote < 0) {
            return done > 0 ? (ssize_t)done : -1;
        }
        done += (size_t)wrote;
    }
    return (ssize_t)done;
}

static ssize_t vprocHostWriteKillable(int host, const void *buf, size_t count) {
    return vprocHostWriteKillableFor(host, buf, count, 0);
}

static void vprocRaiseClosedPipe(int pid);

/* A stream of task PID's own, written on its behalf: its reader leaving is that task's SIGPIPE. */
ssize_t vprocHostWriteForTask(int pid, int fd, const void *buf, size_t count) {
    ssize_t res = vprocHostWriteKillableFor(fd, buf, count, pid);
    if (res < 0 && errno == EPIPE) {
        vprocRaiseClosedPipe(pid);
    }
    return res;
}

typedef struct {
    char path[PATH_MAX];
    int flags;
    mode_t mode;
    bool has_mode;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int fd;
    int err;
    bool done;
    bool abandoned;
} VProcFifoOpen;

static void vprocFifoOpenFree(VProcFifoOpen *open_state) {
    pthread_mutex_destroy(&open_state->mu);
    pthread_cond_destroy(&open_state->cv);
    free(open_state);
}

static void *vprocFifoOpenMain(void *arg) {
    VProcFifoOpen *open_state = (VProcFifoOpen *)arg;
    sigset_t all_signals;
    sigfillset(&all_signals);
    (void)pthread_sigmask(SIG_SETMASK, &all_signals, NULL);
    gVprocFifoOpenDirect = true;
    int fd = vprocHostOpenRawInternal(open_state->path, open_state->flags, open_state->mode, open_state->has_mode);
    int err = errno;
    pthread_mutex_lock(&open_state->mu);
    bool abandoned = open_state->abandoned;
    open_state->fd = fd;
    open_state->err = err;
    open_state->done = true;
    pthread_cond_broadcast(&open_state->cv);
    pthread_mutex_unlock(&open_state->mu);
    if (abandoned) {
        if (fd >= 0) {
            vprocHostClose(fd);
        }
        vprocFifoOpenFree(open_state);
    }
    return NULL;
}

/* Called with the lock held, by a killed or interrupted opener. The helper leaves its open only
 * when the other end opens, so open that end too, and neither stays behind as a reader or a
 * writer for whoever opens the FIFO next. */
static void vprocFifoOpenAbandon(void *arg) {
    VProcFifoOpen *open_state = (VProcFifoOpen *)arg;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", open_state->path);
    bool done = open_state->done;
    open_state->abandoned = true;
    pthread_mutex_unlock(&open_state->mu);
    if (done) {
        if (open_state->fd >= 0) {
            vprocHostClose(open_state->fd);
        }
        vprocFifoOpenFree(open_state);
        return;
    }
    int other_end = vprocHostOpenRawInternal(path, O_RDWR | O_NONBLOCK, 0, false);
    if (other_end >= 0) {
        vprocHostClose(other_end);
    }
}

static int vprocHostOpenFifoDirect(const char *path, int flags, mode_t mode, bool has_mode) {
    gVprocFifoOpenDirect = true;
    int fd = vprocHostOpenRawInternal(path, flags, mode, has_mode);
    int err = errno;
    gVprocFifoOpenDirect = false;
    errno = err;
    return fd;
}

/* Opening a FIFO blocks until the other end opens, in the host's nocancel open. A helper thread
 * waits there instead, and this thread waits where a kill or a signal can reach it. */
static int vprocHostOpenFifoKillable(const char *path, int flags, mode_t mode, bool has_mode) {
    VProcFifoOpen *open_state = strlen(path) < PATH_MAX ? (VProcFifoOpen *)calloc(1, sizeof(*open_state)) : NULL;
    if (!open_state) {
        return vprocHostOpenFifoDirect(path, flags, mode, has_mode);
    }
    snprintf(open_state->path, sizeof(open_state->path), "%s", path);
    open_state->flags = flags;
    open_state->mode = mode;
    open_state->has_mode = has_mode;
    open_state->fd = -1;
    pthread_mutex_init(&open_state->mu, NULL);
    pthread_cond_init(&open_state->cv, NULL);
    pthread_t helper;
    if (vprocHostPthreadCreate(&helper, NULL, vprocFifoOpenMain, open_state) != 0) {
        vprocFifoOpenFree(open_state);
        return vprocHostOpenFifoDirect(path, flags, mode, has_mode);
    }
    pthread_detach(helper);
    pthread_mutex_lock(&open_state->mu);
    while (!open_state->done) {
        struct timespec deadline;
        vprocInprocPipeWaitDeadline(&deadline);
        pthread_cleanup_push(vprocFifoOpenAbandon, open_state);
        pthread_cond_timedwait(&open_state->cv, &open_state->mu, &deadline);
        pthread_cleanup_pop(0);
        if (open_state->done) {
            break;
        }
        pthread_mutex_unlock(&open_state->mu);
        bool interrupted = vprocHostWaitInterrupted();
        pthread_mutex_lock(&open_state->mu);
        if (interrupted && !open_state->done) {
            vprocFifoOpenAbandon(open_state);
            errno = EINTR;
            return -1;
        }
    }
    int fd = open_state->fd;
    int err = open_state->err;
    pthread_mutex_unlock(&open_state->mu);
    vprocFifoOpenFree(open_state);
    errno = err;
    return fd;
}
#else
#define vprocHostWriteKillable(host, buf, count) vprocHostWrite((host), (buf), (count))

ssize_t vprocHostWriteForTask(int pid, int fd, const void *buf, size_t count) {
    (void)pid;
    return vprocHostWrite(fd, buf, count);
}
#endif

typedef struct {
    VProcSessionStdio *session;
    int shell_pid;
    int kernel_pid;
    uint64_t generation;
} VProcSessionInputCtx;

static VProcSessionInput *vprocSessionInputEnsure(VProcSessionStdio *session, int shell_pid, int kernel_pid);
static ssize_t vprocSessionReadInput(VProcSessionStdio *session, void *buf, size_t count, bool nonblocking);
static bool vprocRequestControlSignalForSessionInternal(uint64_t session_id,
                                                        int sig,
                                                        bool allow_runtime_fallback);
static bool vprocKernelQueueContinuePgid(int pgid);
static bool vprocKernelQueueSigchldAggregate(int target_pid, bool ensure_kernel);

static int vprocSessionHostFdForStd(int std_fd) {
    VProc *vp = vprocCurrent();
    if (vp) {
        int host_fd = vprocTranslateFd(vp, std_fd);
        if (host_fd >= 0) {
            return host_fd;
        }
    }
    return std_fd;
}

static bool vprocSessionStdioMatchFd(int session_fd, int std_fd) {
    if (session_fd < 0) {
        return false;
    }
    int host_fd = vprocSessionHostFdForStd(std_fd);
    if (host_fd < 0) {
        return false;
    }
    struct stat session_st;
    struct stat std_st;
    if (vprocHostFstatRaw(session_fd, &session_st) != 0) {
        return false;
    }
    if (vprocHostFstatRaw(host_fd, &std_st) != 0) {
        return false;
    }
    return session_st.st_dev == std_st.st_dev && session_st.st_ino == std_st.st_ino;
}

static bool vprocSessionFdMatchesStd(int fd, int std_fd) {
    if (fd < 0) {
        return false;
    }
    if (fd == std_fd) {
        return true;
    }
    struct stat fd_st;
    struct stat std_st;
    if (vprocHostFstatRaw(fd, &fd_st) != 0) {
        return false;
    }
    if (vprocHostFstatRaw(std_fd, &std_st) != 0) {
        return false;
    }
    return fd_st.st_dev == std_st.st_dev && fd_st.st_ino == std_st.st_ino;
}

static bool vprocSessionFdMatchesHost(int fd, int host_fd) {
    if (fd < 0 || host_fd < 0) {
        return false;
    }
    if (fd == host_fd) {
        return true;
    }
    struct stat fd_st;
    struct stat host_st;
    if (vprocHostFstatRaw(fd, &fd_st) != 0) {
        return false;
    }
    if (vprocHostFstatRaw(host_fd, &host_st) != 0) {
        return false;
    }
    return fd_st.st_dev == host_st.st_dev && fd_st.st_ino == host_st.st_ino;
}

static bool vprocToolDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_TOOL_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocVprocDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_VPROC_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocPipeDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_PIPE_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocKillDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_KILL_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocIoDebugEnabled(void) {
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }
    const char *env = getenv("PSCALI_IO_DEBUG");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        env = getenv("PSCALI_SSH_DEBUG");
    }
    if (!env || env[0] == '\0') {
        cached = 0;
        return false;
    }
    cached = (strcmp(env, "0") != 0) ? 1 : 0;
    return cached == 1;
}

static void vprocDebugLogf(const char *format, ...) {
    if (!format) {
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    size_t len = strlen(buf);
    if (len == 0) {
        return;
    }
    if (buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
#if defined(__APPLE__)
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    static void (*debug_log)(const char *message) = NULL;
    static int debug_log_checked = 0;
#if defined(PSCAL_TARGET_IOS)
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "%{public}s", buf);
#endif
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
    }
    if (!debug_log_checked) {
        debug_log_checked = 1;
        debug_log = (void (*)(const char *))dlsym(RTLD_DEFAULT, "pscalRuntimeDebugLog");
    }
    if (debug_log) {
        debug_log(buf);
    }
#endif
#if !defined(PSCAL_TARGET_IOS)
    fprintf(stderr, "%s\n", buf);
#endif
}

static bool vprocResizeDebugEnabled(void) {
#if !defined(PSCALI_ENABLE_SSH_RESIZE_DEBUG_LOGS)
#define PSCALI_ENABLE_SSH_RESIZE_DEBUG_LOGS 0
#endif
#if PSCALI_ENABLE_SSH_RESIZE_DEBUG_LOGS == 0
    return false;
#else
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }
    const char *env = getenv("PSCALI_SSH_RESIZE_DEBUG");
    if (!env || env[0] == '\0') {
        env = getenv("PSCALI_RESIZE_DEBUG");
    }
    if (!env || env[0] == '\0') {
        cached = 0;
        return false;
    }
    cached = (strcmp(env, "0") != 0) ? 1 : 0;
    return cached == 1;
#endif
}

static void vprocResizeLogf(const char *format, ...) {
    if (!vprocResizeDebugEnabled() || !format) {
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    vprocDebugLogf("%s", buf);
}

static void vprocIoTrace(const char *format, ...) {
    if (!vprocIoDebugEnabled() || !format) {
        return;
    }
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
#if defined(__APPLE__)
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
        return;
    }
#endif
    if (pscalRuntimeDebugLog) {
        pscalRuntimeDebugLog(buf);
    }
}

static void vprocSessionResolveOutputFd(VProcSessionStdio *session, int fd, bool *is_stdout, bool *is_stderr) {
    if (is_stdout) {
        *is_stdout = false;
    }
    if (is_stderr) {
        *is_stderr = false;
    }
    if (!session || !is_stdout || !is_stderr) {
        return;
    }
    if (fd == STDOUT_FILENO) {
        *is_stdout = true;
        return;
    }
    if (fd == STDERR_FILENO) {
        *is_stderr = true;
        return;
    }
    if (vprocSessionFdMatchesStd(fd, STDOUT_FILENO)) {
        *is_stdout = true;
        return;
    }
    if (vprocSessionFdMatchesStd(fd, STDERR_FILENO)) {
        *is_stderr = true;
    }
}

static void vprocSessionPtyRegister(uint64_t session_id,
                                    struct pscal_fd *pty_slave,
                                    struct pscal_fd *pty_master) {
    if (session_id == 0 || !pty_slave || !pty_master) {
        return;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (!entry) {
        pthread_mutex_unlock(&gVProcSessionPtys.mu);
        return;
    }
    if (entry->pty_slave) {
        pscal_fd_close(entry->pty_slave);
    }
    if (entry->pty_master) {
        pscal_fd_close(entry->pty_master);
    }
    entry->pty_slave = pscal_fd_retain(pty_slave);
    entry->pty_master = pscal_fd_retain(pty_master);
    entry->control_bytes_passthrough = false;
    if (entry->pty_slave &&
        entry->pty_slave->tty &&
        entry->last_winsize_cols > 0 &&
        entry->last_winsize_rows > 0) {
        struct winsize_ ws;
        memset(&ws, 0, sizeof(ws));
        ws.col = (word_t)entry->last_winsize_cols;
        ws.row = (word_t)entry->last_winsize_rows;
        tty_set_winsize(entry->pty_slave->tty, ws);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

static void vprocSessionPtyUnregister(uint64_t session_id) {
    if (session_id == 0) {
        return;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    size_t idx = 0;
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, &idx);
    if (entry) {
        if (entry->pty_slave) {
            pscal_fd_close(entry->pty_slave);
        }
        if (entry->pty_master) {
            pscal_fd_close(entry->pty_master);
        }
        entry->last_winsize_cols = 0;
        entry->last_winsize_rows = 0;
        entry->control_bytes_passthrough = false;
        vprocSessionPtyRemoveAtLocked(idx);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

int vprocSetSessionWinsize(uint64_t session_id, int cols, int rows) {
    if (session_id == 0 || cols <= 0 || rows <= 0) {
        errno = EINVAL;
        vprocIoTrace("[vproc-io] winsize session=%llu invalid cols=%d rows=%d",
                     (unsigned long long)session_id,
                     cols,
                     rows);
        return -1;
    }
    struct pscal_fd *pty_slave = NULL;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (entry) {
        entry->last_winsize_cols = cols;
        entry->last_winsize_rows = rows;
    }
    if (entry && entry->pty_slave) {
        pty_slave = pscal_fd_retain(entry->pty_slave);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);

    if (!pty_slave || !pty_slave->tty) {
        if (pty_slave) {
            pscal_fd_close(pty_slave);
        }
        errno = ESRCH;
        vprocIoTrace("[vproc-io] winsize session=%llu missing-pty cols=%d rows=%d (stored)",
                     (unsigned long long)session_id,
                     cols,
                     rows);
        return -1;
    }
    struct winsize_ ws;
    ws.col = (word_t)cols;
    ws.row = (word_t)rows;
    ws.xpixel = 0;
    ws.ypixel = 0;
    int tty_sid_before = (int)pty_slave->tty->session;
    int tty_fg_before = (int)pty_slave->tty->fg_group;
    int map_fg_before = (tty_sid_before > 0) ? pscalTtyGetForegroundPgid(tty_sid_before) : -1;
    vprocResizeLogf("[ssh-resize] winsize set session=%llu cols=%d rows=%d tty=%p sid=%d fg=%d map_fg=%d",
                    (unsigned long long)session_id,
                    cols,
                    rows,
                    (void *)pty_slave->tty,
                    tty_sid_before,
                    tty_fg_before,
                    map_fg_before);
    vprocIoTrace("[vproc-io] winsize session=%llu before tty=%p sid=%d fg=%d map_fg=%d cols=%d rows=%d",
                 (unsigned long long)session_id,
                 (void *)pty_slave->tty,
                 tty_sid_before,
                 tty_fg_before,
                 map_fg_before,
                 cols,
                 rows);
    tty_set_winsize(pty_slave->tty, ws);
    int tty_sid_after = (int)pty_slave->tty->session;
    int tty_fg_after = (int)pty_slave->tty->fg_group;
    int map_fg_after = (tty_sid_after > 0) ? pscalTtyGetForegroundPgid(tty_sid_after) : -1;
    pscal_fd_close(pty_slave);
    vprocResizeLogf("[ssh-resize] winsize done session=%llu cols=%d rows=%d sid=%d fg=%d map_fg=%d",
                    (unsigned long long)session_id,
                    cols,
                    rows,
                    tty_sid_after,
                    tty_fg_after,
                    map_fg_after);
    vprocIoTrace("[vproc-io] winsize session=%llu applied cols=%d rows=%d tty_sid=%d tty_fg=%d map_fg=%d",
                 (unsigned long long)session_id,
                 cols,
                 rows,
                 tty_sid_after,
                 tty_fg_after,
                 map_fg_after);
    return 0;
}

int vprocGetSessionWinsize(uint64_t session_id, int *cols_out, int *rows_out) {
    if (session_id == 0 || !cols_out || !rows_out) {
        errno = EINVAL;
        return -1;
    }
    *cols_out = 0;
    *rows_out = 0;

    struct pscal_fd *pty_slave = NULL;
    int fallback_cols = 0;
    int fallback_rows = 0;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry && entry->pty_slave) {
        pty_slave = pscal_fd_retain(entry->pty_slave);
    }
    if (entry) {
        fallback_cols = entry->last_winsize_cols;
        fallback_rows = entry->last_winsize_rows;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);

    if (!pty_slave || !pty_slave->tty) {
        if (pty_slave) {
            pscal_fd_close(pty_slave);
        }
        if (fallback_cols > 0 && fallback_rows > 0) {
            *cols_out = fallback_cols;
            *rows_out = fallback_rows;
            return 0;
        }
        errno = ESRCH;
        return -1;
    }

    struct winsize_ ws;
    memset(&ws, 0, sizeof(ws));
    lock(&pty_slave->tty->lock);
    ws = pty_slave->tty->winsize;
    unlock(&pty_slave->tty->lock);
    pscal_fd_close(pty_slave);

    *cols_out = (int)ws.col;
    *rows_out = (int)ws.row;
    if (*cols_out <= 0 || *rows_out <= 0) {
        if (fallback_cols > 0 && fallback_rows > 0) {
            *cols_out = fallback_cols;
            *rows_out = fallback_rows;
        }
    }
    return 0;
}

void vprocSessionSetOutputHandler(uint64_t session_id,
                                  VProcSessionOutputHandler handler,
                                  void *context) {
    if (session_id == 0) {
        return;
    }
    vprocIoTrace("[vproc-io] set output handler session=%llu handler=%p ctx=%p",
                 (unsigned long long)session_id,
                 (void *)handler,
                 context);
    unsigned char *pending_copy = NULL;
    size_t pending_len = 0;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (entry) {
        entry->output_handler = handler;
        entry->output_context = context;
        if (handler && !entry->output_paused && entry->pending_output_len > 0) {
            pending_copy = (unsigned char *)malloc(entry->pending_output_len);
            if (pending_copy) {
                memcpy(pending_copy, entry->pending_output, entry->pending_output_len);
                pending_len = entry->pending_output_len;
                entry->pending_output_len = 0;
                entry->output_backlog_saturated = false;
            }
        }
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    if (handler && pending_copy && pending_len > 0) {
        handler(session_id, pending_copy, pending_len, context);
    }
    free(pending_copy);
}

void vprocSessionClearOutputHandler(uint64_t session_id) {
    if (session_id == 0) {
        return;
    }
    vprocIoTrace("[vproc-io] clear output handler session=%llu",
                 (unsigned long long)session_id);
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    size_t idx = 0;
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, &idx);
    if (entry) {
        entry->output_handler = NULL;
        entry->output_context = NULL;
        entry->output_paused = false;
        vprocSessionPtyEntryClearPendingLocked(entry);
        if (vprocSessionPtyEntryIsEmpty(entry)) {
            vprocSessionPtyRemoveAtLocked(idx);
        }
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

void vprocSessionSetOutputPaused(uint64_t session_id, bool paused) {
    if (session_id == 0) {
        return;
    }
    unsigned char *pending_copy = NULL;
    size_t pending_len = 0;
    VProcSessionOutputHandler handler = NULL;
    void *context = NULL;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry) {
        entry->output_paused = paused;
        if (!paused && entry->output_handler && entry->pending_output_len > 0) {
            pending_copy = (unsigned char *)malloc(entry->pending_output_len);
            if (pending_copy) {
                memcpy(pending_copy, entry->pending_output, entry->pending_output_len);
                pending_len = entry->pending_output_len;
                handler = entry->output_handler;
                context = entry->output_context;
                entry->pending_output_len = 0;
                entry->output_backlog_saturated = false;
            }
        }
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    if (handler && pending_copy && pending_len > 0) {
        handler(session_id, pending_copy, pending_len, context);
    }
    free(pending_copy);
}

typedef enum {
    VPROC_OUTPUT_DISPATCH_DROPPED = 0,
    VPROC_OUTPUT_DISPATCH_HANDLED,
    VPROC_OUTPUT_DISPATCH_QUEUED,
    VPROC_OUTPUT_DISPATCH_THROTTLED
} VProcOutputDispatchResult;

static VProcOutputDispatchResult vprocSessionDispatchOutput(uint64_t session_id,
                                                            const unsigned char *data,
                                                            size_t len) {
    VProcSessionOutputHandler handler = NULL;
    void *context = NULL;
    bool queued = false;
    bool throttled = false;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (entry) {
        if (entry->output_handler && !entry->output_paused) {
            handler = entry->output_handler;
            context = entry->output_context;
        } else {
            queued = vprocSessionQueueOutputLocked(entry, data, len);
            if (queued && entry->output_paused && entry->output_backlog_saturated) {
                throttled = true;
            }
        }
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    if (handler) {
        vprocIoTrace("[vproc-io] output session=%llu len=%zu handler=%p",
                     (unsigned long long)session_id,
                     len,
                     (void *)handler);
        handler(session_id, data, len, context);
        return VPROC_OUTPUT_DISPATCH_HANDLED;
    }
    if (throttled) {
        return VPROC_OUTPUT_DISPATCH_THROTTLED;
    }
    if (queued) {
        return VPROC_OUTPUT_DISPATCH_QUEUED;
    }
    return VPROC_OUTPUT_DISPATCH_DROPPED;
}

ssize_t vprocSessionEmitOutput(uint64_t session_id, const void *buf, size_t len) {
    if (session_id == 0 || !buf || len == 0) {
        errno = EINVAL;
        return -1;
    }
    const unsigned char *bytes = (const unsigned char *)buf;
    for (;;) {
        VProcOutputDispatchResult result = vprocSessionDispatchOutput(session_id, bytes, len);
        if (result == VPROC_OUTPUT_DISPATCH_HANDLED ||
            result == VPROC_OUTPUT_DISPATCH_QUEUED) {
            return (ssize_t)len;
        }
        if (result == VPROC_OUTPUT_DISPATCH_THROTTLED) {
            usleep(VPROC_SESSION_OUTPUT_THROTTLE_US);
            continue;
        }
        errno = EPIPE;
        return -1;
    }
}

static bool vprocSessionOutputShouldThrottle(uint64_t session_id) {
    bool throttle = false;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry && entry->output_paused && entry->output_backlog_saturated) {
        throttle = true;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    return throttle;
}

static ssize_t vprocSessionWritePtyMaster(struct pscal_fd *master,
                                          const void *buf,
                                          size_t len,
                                          bool blocking) {
    if (!master || !master->tty || !master->tty->pty.other || !buf || len == 0) {
        errno = EINVAL;
        return -1;
    }
    struct tty *slave = master->tty->pty.other;
    size_t off = 0;
    while (off < len) {
        ssize_t w = tty_input(slave, (const char *)buf + off, len - off, blocking);
        if (w < 0) {
            if (w == _EINTR) {
                continue;
            }
            errno = pscalCompatErrno((int)w);
            return off > 0 ? (ssize_t)off : -1;
        }
        if (w == 0) {
            break;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

ssize_t vprocSessionWriteToMasterMode(uint64_t session_id,
                                      const void *buf,
                                      size_t len,
                                      bool blocking) {
    if (session_id == 0 || !buf || len == 0) {
        errno = EINVAL;
        return -1;
    }
    vprocIoTrace("[vproc-io] input write session=%llu len=%zu",
                 (unsigned long long)session_id,
                 len);
    struct pscal_fd *master = NULL;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry && entry->pty_master) {
        master = pscal_fd_retain(entry->pty_master);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    if (!master || !master->ops || !master->ops->write) {
        if (master) {
            pscal_fd_close(master);
        }
        errno = EBADF;
        return -1;
    }
    if (pscalPtyIsMaster(master) && master->tty && master->tty->pty.other) {
        ssize_t written = vprocSessionWritePtyMaster(master, buf, len, blocking);
        pscal_fd_close(master);
        return written;
    }
    unsigned prev_flags = 0;
    bool restore_flags = false;
    if (!blocking) {
        lock(&master->lock);
        prev_flags = master->flags;
        master->flags |= O_NONBLOCK;
        unlock(&master->lock);
        restore_flags = true;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = master->ops->write(master, (const unsigned char *)buf + off, len - off);
        if (w < 0) {
            if (w == _EINTR) {
                continue;
            }
            if (w == _EAGAIN && !blocking) {
                if (restore_flags) {
                    lock(&master->lock);
                    master->flags = prev_flags;
                    unlock(&master->lock);
                }
                pscal_fd_close(master);
                errno = pscalCompatErrno((int)w);
                return off > 0 ? (ssize_t)off : -1;
            }
            if (restore_flags) {
                lock(&master->lock);
                master->flags = prev_flags;
                unlock(&master->lock);
            }
            pscal_fd_close(master);
            errno = pscalCompatErrno((int)w);
            return off > 0 ? (ssize_t)off : -1;
        }
        if (w == 0) {
            break;
        }
        off += (size_t)w;
    }
    if (restore_flags) {
        lock(&master->lock);
        master->flags = prev_flags;
        unlock(&master->lock);
    }
    pscal_fd_close(master);
    return (ssize_t)off;
}

ssize_t vprocSessionWriteToMaster(uint64_t session_id, const void *buf, size_t len) {
    return vprocSessionWriteToMasterMode(session_id, buf, len, true);
}

static bool vprocDispatchControlSignalToForeground(int shell_pid,
                                                   int sig,
                                                   bool allow_runtime_fallback) {
    if (vprocSoftSignalingDisabledByEnv()) {
        return false;
    }
    if (shell_pid <= 0) {
        return false;
    }
    int target_fgid = -1;
    int shell_pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *shell_entry = vprocTaskFindLocked(shell_pid);
    if (shell_entry) {
        shell_pgid = shell_entry->pgid;
        if (shell_entry->sid > 0) {
            target_fgid = vprocForegroundPgidLocked(shell_entry->sid);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (target_fgid <= 0) {
        int override_fg = (pscalRuntimeCurrentForegroundPgid)
                ? pscalRuntimeCurrentForegroundPgid()
                : -1;
        if (override_fg > 0) {
            target_fgid = override_fg;
        } else {
            target_fgid = shell_pgid;
        }
    }
    if (target_fgid > 0) {
        int sid = -1;
        pthread_mutex_lock(&gVProcTasks.mu);
        VProcTaskEntry *shell_entry = vprocTaskFindLocked(shell_pid);
        if (shell_entry) {
            sid = shell_entry->sid;
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (vprocForegroundGroupPrefersControlBytes(sid, target_fgid, shell_pid, sig)) {
            return false;
        }
        if (vprocToolDebugEnabled()) {
            fprintf(stderr,
                    "[session-input-ctrl] fg-dispatch sig=%d shell=%d shell_pgid=%d target_fgid=%d\n",
                    sig,
                    shell_pid,
                    shell_pgid,
                    target_fgid);
        }
        /* Ended, the group can't take it, and the fallbacks below are what stops the command. */
        int rc = vprocKillLive(-target_fgid, sig);
        if (vprocToolDebugEnabled()) {
            fprintf(stderr,
                    "[session-input-ctrl] fg-dispatch result sig=%d target_fgid=%d rc=%d errno=%d\n",
                    sig,
                    target_fgid,
                    rc,
                    errno);
        }
        if (rc == 0) {
#if defined(PSCAL_TARGET_IOS)
            /* A VM program polls the runtime's flag, not its task's pending signals: the key's
             * SIGINT was only queued on it, and a ReadKey or a busy loop never ended. */
            if (sig == SIGINT) {
                pscalRuntimeNoteSigint();
            }
#endif
            return true;
        }
#if defined(PSCAL_TARGET_IOS)
        if (sig == SIGTSTP && shell_pgid > 0 && target_fgid != shell_pgid) {
            if (vprocKillLive(-shell_pgid, sig) == 0) {
                return true;
            }
        }
        if (allow_runtime_fallback && sig == SIGINT && pscalRuntimeRequestSigint) {
            pscalRuntimeRequestSigint();
            return true;
        }
        if (allow_runtime_fallback && sig == SIGTSTP && pscalRuntimeRequestSigtstp) {
            pscalRuntimeRequestSigtstp();
            return true;
        }
#endif
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (allow_runtime_fallback && sig == SIGINT && pscalRuntimeRequestSigint) {
        pscalRuntimeRequestSigint();
        return true;
    }
    if (allow_runtime_fallback && sig == SIGTSTP && pscalRuntimeRequestSigtstp) {
        pscalRuntimeRequestSigtstp();
        return true;
    }
#endif
    return false;
}

static bool vprocEnsureSessionInputWritableLocked(VProcSessionInput *input, size_t append_len) {
    if (!input || append_len == 0) {
        return false;
    }
    if (input->off > input->cap || input->len > (input->cap - input->off)) {
        return false;
    }
    size_t tail = input->off + input->len;
    if (append_len <= (input->cap - tail)) {
        return true;
    }
    if (input->off > 0 && append_len <= (input->cap - input->len)) {
        if (input->len > 0) {
            memmove(input->buf, input->buf + input->off, input->len);
        }
        input->off = 0;
        return true;
    }
    size_t needed = input->len + append_len;
    size_t new_cap = 0;
    if (!vprocComputeGrowthCapacity(input->cap, needed, 256, sizeof(unsigned char), &new_cap)) {
        return false;
    }
    unsigned char *resized = (unsigned char *)realloc(input->buf, new_cap);
    if (!resized) {
        return false;
    }
    if (input->len > 0 && input->off > 0) {
        memmove(resized, resized + input->off, input->len);
    }
    input->buf = resized;
    input->off = 0;
    input->cap = new_cap;
    return true;
}

static bool vprocSessionInputLockWithTimeout(VProcSessionInput *input,
                                             int timeout_ms,
                                             const char *context) {
    if (!input) {
        errno = EINVAL;
        return false;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    const int sleep_us = 1000;
    int waited_us = 0;
    int budget_us = timeout_ms * 1000;
    for (;;) {
        int rc = pthread_mutex_trylock(&input->mu);
        if (rc == 0) {
            return true;
        }
        if (rc != EBUSY) {
            errno = rc;
            fprintf(stderr,
                    "[session-input] lock-error ctx=%s rc=%d input=%p\n",
                    context ? context : "?",
                    rc,
                    (void *)input);
            return false;
        }
        if (waited_us >= budget_us) {
            errno = EBUSY;
            fprintf(stderr,
                    "[session-input] lock-timeout ctx=%s waited_ms=%d input=%p\n",
                    context ? context : "?",
                    timeout_ms,
                    (void *)input);
            return false;
        }
        usleep(sleep_us);
        waited_us += sleep_us;
    }
}

static VProcSessionInput *vprocSessionInputRecoverStuck(VProcSessionStdio *session,
                                                        VProcSessionInput *stuck,
                                                        const char *context) {
    if (!session || !stuck) {
        return NULL;
    }

    pthread_mutex_lock(&gSessionInputInitMu);
    if (session->input != stuck) {
        VProcSessionInput *current = session->input;
        pthread_mutex_unlock(&gSessionInputInitMu);
        return current;
    }

    VProcSessionInput *replacement = (VProcSessionInput *)calloc(1, sizeof(VProcSessionInput));
    if (!replacement) {
        pthread_mutex_unlock(&gSessionInputInitMu);
        return NULL;
    }
    pthread_mutex_init(&replacement->mu, NULL);
    pthread_cond_init(&replacement->cv, NULL);
    replacement->inited = true;
    replacement->reader_fd = -1;
    replacement->interrupt_pending = true;
    session->input = replacement;
    pthread_mutex_unlock(&gSessionInputInitMu);

    fprintf(stderr,
            "[session-input] recovered-stuck-lock ctx=%s session=%llu old=%p new=%p\n",
            context ? context : "?",
            (unsigned long long)session->session_id,
            (void *)stuck,
            (void *)replacement);
    return replacement;
}

static bool vprocTtyInLineMode(struct pscal_fd *pscal_fd) {
    if (!pscal_fd || !pscal_fd->tty) {
        return false;
    }
    lock(&pscal_fd->tty->lock);
    bool line_mode = (pscal_fd->tty->termios.lflags & ICANON_) != 0;
    unlock(&pscal_fd->tty->lock);
    return line_mode;
}

static void vprocSessionInputMarkEof(VProcSessionInput *input) {
    pthread_mutex_lock(&input->mu);
    if (input->eof_mark_count < VPROC_SESSION_EOF_MARKS) {
        input->eof_mark[input->eof_mark_count++] = input->len;
        pthread_cond_broadcast(&input->cv);
    }
    pthread_mutex_unlock(&input->mu);
    pscal_fd_poll_wakeup(NULL, POLLIN);
}

static void *vprocSessionInputThread(void *arg) {
    VProcSessionInputCtx *ctx = (VProcSessionInputCtx *)arg;
    if (!ctx || !ctx->session) {
        free(ctx);
        return NULL;
    }
    VProcSessionStdio *session = ctx->session;
    if (session) {
        vprocSessionStdioActivate(session);
    }
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }

    VProcSessionInput *input = session ? session->input : NULL;
    int fd = session ? session->stdin_host_fd : -1;
    struct pscal_fd *pscal_fd = session ? session->stdin_pscal_fd : NULL;
    const bool tool_dbg = vprocToolDebugEnabled();
    unsigned int retry_sleep_us = 1000;
    if (pscal_fd) {
        pscal_fd = pscal_fd_retain(pscal_fd);
    }
    if (pscal_fd && (!pscal_fd->ops || !pscal_fd->ops->read)) {
        pscal_fd_close(pscal_fd);
        pscal_fd = NULL;
    }
    unsigned char ch = 0;
    if (tool_dbg) {
        vprocDebugLogf(
                "[session-input] reader start host_fd=%d pscal_fd=%p shell=%d kernel=%d\n",
                fd,
                (void *)pscal_fd,
                ctx->shell_pid,
                ctx->kernel_pid);
    }
    while (fd >= 0 || pscal_fd) {
        if (input) {
            pthread_mutex_lock(&input->mu);
            bool stop_requested = input->stop_requested;
            pthread_mutex_unlock(&input->mu);
            if (stop_requested) {
                if (tool_dbg) {
                    vprocDebugLogf( "[session-input] reader stop fd=%d\n", fd);
                }
                break;
            }
        }
        ssize_t r = 0;
        if (fd >= 0) {
            /* Look again now and then for a stop: on Darwin a close of the terminal waits for
             * a read of it to end, and the shell closes its copy as it ends. */
            struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
            int ready = vprocHostPollRaw(&pfd, 1, 100);
            if (ready == 0 || (ready < 0 && errno == EINTR)) {
                continue;
            }
            r = vprocHostRead(fd, &ch, 1);
            if (r <= 0) {
                if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        usleep(retry_sleep_us);
                        if (retry_sleep_us < 16000) {
                            retry_sleep_us <<= 1;
                        }
                    }
                    continue;
                }
                if (tool_dbg) {
                    int saved_errno = errno;
                    vprocDebugLogf(
                            "[session-input] reader eof host_fd=%d r=%zd errno=%d\n",
                            fd,
                            r,
                            saved_errno);
                }
                if (input) {
                    pthread_mutex_lock(&input->mu);
                    bool is_current = (input->reader_generation == ctx->generation);
                    if (is_current) {
                        input->eof = true;
                        pthread_cond_broadcast(&input->cv);
                    }
                    pthread_mutex_unlock(&input->mu);
                    pscal_fd_poll_wakeup(NULL, POLLIN);
                }
                break;
            }
        } else {
            r = pscal_fd->ops->read(pscal_fd, &ch, 1);
            if (r <= 0) {
                if (r == 0 && input && pscalTtyTakeEofKey(pscal_fd)) {
                    vprocSessionInputMarkEof(input);
                    continue;
                }
                if (r == 0 && session && session->pty_active &&
                    (pscal_fd == session->stdin_pscal_fd ||
                     pscal_fd == session->pty_slave)) {
                    /* PTY slave reads may return 0 transiently when no byte is
                     * ready (e.g. VMIN=0). Treat that as "would block" instead
                     * of EOF so nested ssh/scp password reads do not exit early. */
                    usleep(retry_sleep_us);
                    if (retry_sleep_us < 16000) {
                        retry_sleep_us <<= 1;
                    }
                    continue;
                }
                if (r == _EIO && session && session->pty_active &&
                    (pscal_fd == session->stdin_pscal_fd ||
                     pscal_fd == session->pty_slave)) {
                    /* Some PTY-backed readers surface transient EIO between
                     * keystrokes; keep waiting so prompt loops don't terminate
                     * after the first typed character. */
                    usleep(retry_sleep_us);
                    if (retry_sleep_us < 16000) {
                        retry_sleep_us <<= 1;
                    }
                    continue;
                }
                if (r == _EINTR || r == _EAGAIN) {
                    if (r == _EAGAIN) {
                        usleep(retry_sleep_us);
                        if (retry_sleep_us < 16000) {
                            retry_sleep_us <<= 1;
                        }
                    }
                    continue;
                }
                if (r < 0) {
                    vprocSetCompatErrno((int)r);
                }
                if (tool_dbg) {
                    int saved_errno = errno;
                    vprocDebugLogf(
                            "[session-input] reader eof pscal_fd=%p r=%zd errno=%d\n",
                            (void *)pscal_fd,
                            r,
                            saved_errno);
                }
                if (input) {
                    pthread_mutex_lock(&input->mu);
                    bool is_current = (input->reader_generation == ctx->generation);
                    if (is_current) {
                        input->eof = true;
                        pthread_cond_broadcast(&input->cv);
                    }
                    pthread_mutex_unlock(&input->mu);
                    pscal_fd_poll_wakeup(NULL, POLLIN);
                }
                break;
            }
        }
        retry_sleep_us = 1000;
        if ((ch == 3 || ch == 26) &&
            !vprocSessionPrefersControlBytePassthrough(session) &&
            !vprocSoftSignalingDisabledByEnv()) {
            int shell_pid = -1;
            int shell_pgid = -1;
            int sid = -1;
            int fg_pgid = -1;
            (void)vprocGetSessionJobControlState(session,
                                                 &shell_pid,
                                                 &shell_pgid,
                                                 &sid,
                                                 &fg_pgid);
            bool shell_foreground = (shell_pgid > 0 && fg_pgid > 0 && fg_pgid == shell_pgid);
            bool shell_prompt_read_active = vprocShellPromptReadActive(shell_pid);
            if (tool_dbg) {
                fprintf(stderr,
                        "[session-input-ctrl] ch=%d shell_pid=%d shell_pgid=%d sid=%d fg=%d prompt=%d\n",
                        (int)ch,
                        shell_pid,
                        shell_pgid,
                        sid,
                        fg_pgid,
                        (int)shell_prompt_read_active);
            }
            if (shell_foreground && shell_prompt_read_active && input) {
                /* When the shell owns the foreground, treat Ctrl-C/Z as shell
                 * input control bytes (prompt/editor behavior) instead of
                 * forcing vproc signal dispatch. */
                if (!vprocSessionInputLockWithTimeout(input, 200, "reader.ctrl.shellfg")) {
                    VProcSessionInput *replacement =
                            vprocSessionInputRecoverStuck(session, input, "reader.ctrl.shellfg");
                    if (replacement) {
                        input = replacement;
                    }
                    continue;
                }
                if (vprocEnsureSessionInputWritableLocked(input, 1)) {
                    input->buf[input->off + input->len] = ch;
                    input->len++;
                }
                pthread_cond_broadcast(&input->cv);
                pthread_mutex_unlock(&input->mu);
                pscal_fd_poll_wakeup(NULL, POLLIN);
                continue;
            }
            int sig = (ch == 3) ? SIGINT : SIGTSTP;
            bool remote_passthrough = (fg_pgid > 0 &&
                                       !shell_foreground &&
                                       vprocForegroundGroupPrefersControlBytes(sid, fg_pgid, shell_pid, sig));
            if (remote_passthrough && input) {
                if (!vprocSessionInputLockWithTimeout(input, 200, "reader.ctrl.remote")) {
                    VProcSessionInput *replacement =
                            vprocSessionInputRecoverStuck(session, input, "reader.ctrl.remote");
                    if (replacement) {
                        input = replacement;
                    }
                    continue;
                }
                if (vprocEnsureSessionInputWritableLocked(input, 1)) {
                    input->buf[input->off + input->len] = ch;
                    input->len++;
                }
                pthread_cond_broadcast(&input->cv);
                pthread_mutex_unlock(&input->mu);
                pscal_fd_poll_wakeup(NULL, POLLIN);
                continue;
            }
            int shell_hint = ctx->shell_pid;
            if (shell_hint <= 0 && session && session->shell_pid > 0) {
                shell_hint = session->shell_pid;
                ctx->shell_pid = shell_hint;
            }
            bool dispatched = false;
            if (session && session->session_id != 0) {
                dispatched = vprocRequestControlSignalForSessionInternal(session->session_id, sig, true);
            }
            if (tool_dbg) {
                fprintf(stderr,
                        "[session-input-ctrl] dispatch sig=%d session=%llu shell=%d dispatched=%d\n",
                        sig,
                        (unsigned long long)(session ? session->session_id : 0),
                        shell_hint,
                        (int)dispatched);
            }
            if (!dispatched) {
                dispatched = vprocDispatchControlSignalToForeground(shell_hint, sig, true);
            }
            bool buffered_control_byte = false;
            if (!dispatched && input) {
                /* Keep control bytes observable even if virtual dispatch cannot
                 * resolve a foreground target. This preserves SSH passthrough
                 * and avoids dropping ^C/^Z for frontends while session/job
                 * metadata is still converging. */
                if (!vprocSessionInputLockWithTimeout(input, 200, "reader.ctrl.buffer")) {
                    VProcSessionInput *replacement =
                            vprocSessionInputRecoverStuck(session, input, "reader.ctrl.buffer");
                    if (replacement) {
                        input = replacement;
                    }
                    continue;
                }
                if (vprocEnsureSessionInputWritableLocked(input, 1)) {
                    input->buf[input->off + input->len] = ch;
                    input->len++;
                    buffered_control_byte = true;
                }
                pthread_cond_broadcast(&input->cv);
                pthread_mutex_unlock(&input->mu);
                pscal_fd_poll_wakeup(NULL, POLLIN);
            }
            if (input && !buffered_control_byte) {
                const bool line_mode = vprocTtyInLineMode(pscal_fd);
                if (!vprocSessionInputLockWithTimeout(input, 200, "reader.ctrl.interrupt")) {
                    VProcSessionInput *replacement =
                            vprocSessionInputRecoverStuck(session, input, "reader.ctrl.interrupt");
                    if (replacement) {
                        input = replacement;
                    }
                    continue;
                }
                /* As a terminal flushes its input on these keys in line mode:
                 * lines typed ahead must not run after what was interrupted. */
                if (line_mode) {
                    input->off = 0;
                    input->len = 0;
                    input->eof_mark_count = 0;
                }
                input->interrupt_pending = true;
                input->interrupt_key = (sig == SIGINT);
                pthread_cond_broadcast(&input->cv);
                pthread_mutex_unlock(&input->mu);
                pscal_fd_poll_wakeup(NULL, POLLIN);
            }
            continue;
        }
        if (!input) {
            continue;
        }
        if (!vprocSessionInputLockWithTimeout(input, 200, "reader.byte")) {
            VProcSessionInput *replacement =
                    vprocSessionInputRecoverStuck(session, input, "reader.byte");
            if (replacement) {
                input = replacement;
            }
            continue;
        }
        if (vprocEnsureSessionInputWritableLocked(input, 1)) {
            input->buf[input->off + input->len] = ch;
            input->len++;
            pthread_cond_signal(&input->cv);
        }
        pthread_mutex_unlock(&input->mu);
        pscal_fd_poll_wakeup(NULL, POLLIN);
    }
    if (input) {
        if (!vprocSessionInputLockWithTimeout(input, 200, "reader.cleanup")) {
            VProcSessionInput *replacement =
                    vprocSessionInputRecoverStuck(session, input, "reader.cleanup");
            if (replacement) {
                input = replacement;
            } else {
                pscal_fd_poll_wakeup(NULL, POLLIN);
                goto vproc_session_input_thread_done;
            }
            if (!vprocSessionInputLockWithTimeout(input, 200, "reader.cleanup.relock")) {
                pscal_fd_poll_wakeup(NULL, POLLIN);
                goto vproc_session_input_thread_done;
            }
        }
        bool is_current = (input->reader_generation == ctx->generation);
        if (is_current) {
            input->reader_active = false;
            input->reader_fd = -1;
            input->stop_requested = false;
            /* If a blocked reader raced this teardown while waiting for input
             * (and we are not at EOF), force a retry path instead of letting
             * it sleep forever with no active producer. */
            if (!input->eof && input->len == 0) {
                input->interrupt_pending = true;
            }
            pthread_cond_broadcast(&input->cv);
        }
        pthread_mutex_unlock(&input->mu);
        pscal_fd_poll_wakeup(NULL, POLLIN);
    }
vproc_session_input_thread_done:
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        pscal_fd = NULL;
    }
    if (session) {
        vprocSessionStdioActivate(NULL);
    }
    free(ctx);
    return NULL;
}

static void *vprocSessionPtyOutputThread(void *arg) {
    VProcSessionStdio *session = (VProcSessionStdio *)arg;
    if (!session) {
        return NULL;
    }
    struct pscal_fd *master = session->pty_master;
    if (!master || !master->ops || !master->ops->read) {
        vprocPtyTrace("[PTY] output thread missing master (master=%p)", (void *)master);
        return NULL;
    }
    pthread_t tid = pthread_self();
    vprocRegisterInterposeBypassThread(tid);
    pthread_setname_np("vproc-pty-out");
    vprocPtyTrace("[PTY] output thread start");
    /* Larger chunking reduces per-callback overhead for high-volume PTY output. */
    char buf[8192];
    unsigned int retry_sleep_us = 1000;
    bool backlog_throttled = false;
    while (session->pty_active) {
        if (backlog_throttled) {
            /* Stay in backoff until handler resumes or backlog is drained. */
            if (vprocSessionOutputShouldThrottle(session->session_id)) {
                usleep(VPROC_SESSION_OUTPUT_THROTTLE_US);
                continue;
            }
            backlog_throttled = false;
        }
        ssize_t r = master->ops->read(master, buf, sizeof(buf));
        if (r == 0) {
            vprocPtyTrace("[PTY] output thread EOF");
            break;
        }
        if (r < 0) {
            if (r == _EINTR || r == _EAGAIN) {
                if (r == _EAGAIN) {
                    usleep(retry_sleep_us);
                    if (retry_sleep_us < 16000) {
                        retry_sleep_us <<= 1;
                    }
                }
                continue;
            }
            vprocPtyTrace("[PTY] output thread read error code=%zd", r);
            session->pty_active = false;
            break;
        }
        retry_sleep_us = 1000;
        VProcOutputDispatchResult dispatch =
                vprocSessionDispatchOutput(session->session_id, (const unsigned char *)buf, (size_t)r);
        if (dispatch == VPROC_OUTPUT_DISPATCH_HANDLED ||
            dispatch == VPROC_OUTPUT_DISPATCH_QUEUED) {
            continue;
        }
        if (dispatch == VPROC_OUTPUT_DISPATCH_THROTTLED) {
            backlog_throttled = true;
            usleep(VPROC_SESSION_OUTPUT_THROTTLE_US);
            continue;
        }
        vprocIoTrace("[vproc-io] output drop session=%llu len=%zd (no handler)",
                     (unsigned long long)session->session_id,
                     r);
        vprocPtyTrace("[PTY] output thread drop len=%zd (no handler)", r);
    }
    vprocPtyTrace("[PTY] output thread exit active=%d", session->pty_active ? 1 : 0);
    vprocUnregisterInterposeBypassThread(tid);
    return NULL;
}

static VProcSessionInput *vprocSessionInputEnsure(VProcSessionStdio *session, int shell_pid, int kernel_pid) {
    if (!session) {
        return NULL;
    }
    const bool tool_dbg = vprocToolDebugEnabled();
    pthread_mutex_lock(&gSessionInputInitMu);
    if (!session->input) {
        session->input = (VProcSessionInput *)calloc(1, sizeof(VProcSessionInput));
        if (session->input) {
            pthread_mutex_init(&session->input->mu, NULL);
            pthread_cond_init(&session->input->cv, NULL);
            session->input->inited = true;
            session->input->reader_fd = -1;
        }
    }
    VProcSessionInput *input = session->input;
    pthread_mutex_unlock(&gSessionInputInitMu);
    if (!input) {
        return NULL;
    }

    bool has_pscal_fd = session->stdin_pscal_fd &&
            session->stdin_pscal_fd->ops &&
            session->stdin_pscal_fd->ops->read;
    bool spawn_reader = false;
    uint32_t spawn_generation = 0;
    int spawn_fd = -1;
    if (!vprocSessionInputLockWithTimeout(input, 200, "ensure")) {
        VProcSessionInput *replacement = vprocSessionInputRecoverStuck(session, input, "ensure");
        if (replacement) {
            input = replacement;
            if (!vprocSessionInputLockWithTimeout(input, 200, "ensure.relock")) {
                return input;
            }
        } else {
            return input;
        }
    }
    if (!input->reader_active && input->eof) {
        input->eof = false;
        input->off = 0;
        input->len = 0;
        input->eof_mark_count = 0;
        input->interrupt_pending = false;
        input->interrupt_key = false;
    }
    if (!input->reader_active &&
        (session->stdin_host_fd >= 0 || has_pscal_fd)) {
        input->reader_generation++;
        spawn_generation = input->reader_generation;
        input->stop_requested = false;
        input->reader_active = true;
        input->reader_fd = session->stdin_host_fd;
        spawn_fd = session->stdin_host_fd;
        spawn_reader = true;
    }
    pthread_mutex_unlock(&input->mu);

    if (spawn_reader) {
        VProcSessionInputCtx *ctx = (VProcSessionInputCtx *)calloc(1, sizeof(VProcSessionInputCtx));
        if (!ctx) {
            pthread_mutex_lock(&input->mu);
            if (input->reader_generation == spawn_generation) {
                input->reader_active = false;
                input->reader_fd = -1;
                input->stop_requested = false;
            }
            pthread_mutex_unlock(&input->mu);
            return input;
        }

        ctx->session = session;
        ctx->shell_pid = shell_pid;
        ctx->kernel_pid = kernel_pid;
        ctx->generation = spawn_generation;

        pthread_t tid;
        int create_rc = vprocHostPthreadCreate(&tid, NULL, vprocSessionInputThread, ctx);
        if (create_rc == 0) {
            pthread_detach(tid);
            if (tool_dbg) {
                fprintf(stderr,
                        "[session-input] reader spawned host_fd=%d pscal_fd=%p\n",
                        spawn_fd,
                        (void *)session->stdin_pscal_fd);
                vprocDebugLogf(
                        "[session-input] reader spawned host_fd=%d pscal_fd=%p\n",
                        spawn_fd,
                        (void *)session->stdin_pscal_fd);
            }
        } else {
            pthread_mutex_lock(&input->mu);
            if (input->reader_generation == spawn_generation) {
                input->reader_active = false;
                input->reader_fd = -1;
                input->stop_requested = false;
            }
            pthread_mutex_unlock(&input->mu);
            if (tool_dbg) {
                fprintf(stderr, "[session-input] reader spawn failed rc=%d\n", create_rc);
                vprocDebugLogf(
                        "[session-input] reader spawn failed rc=%d\n",
                        create_rc);
            }
            free(ctx);
        }
    }
    return input;
}

static int vprocResolveSessionShellPidForInput(VProcSessionStdio *session) {
    if (!session || vprocSessionStdioIsDefault(session)) {
        return vprocGetShellSelfPid();
    }
    if (session->shell_pid > 0) {
        return session->shell_pid;
    }
    if (session->session_id != 0) {
        int shell_pid = 0;
        pthread_mutex_lock(&gVProcSessionPtys.mu);
        VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session->session_id, NULL);
        if (entry && entry->shell_pid > 0) {
            shell_pid = entry->shell_pid;
        }
        pthread_mutex_unlock(&gVProcSessionPtys.mu);
        if (shell_pid > 0) {
            session->shell_pid = shell_pid;
            return shell_pid;
        }
    }

    VProc *current = vprocCurrent();
    if (current) {
        int current_pid = vprocPid(current);
        if (current_pid > 0) {
            session->shell_pid = current_pid;
            if (session->session_id != 0) {
                vprocSessionPtySetShellPid(session->session_id, current_pid);
            }
            return current_pid;
        }
    }
    return 0;
}

static ssize_t vprocSessionReadInput(VProcSessionStdio *session, void *buf, size_t count, bool nonblocking) {
    if (!session || !session->input || !buf || count == 0) {
        return 0;
    }
    VProcSessionInput *input = session->input;
    const bool tool_dbg = vprocToolDebugEnabled();
    const bool prompt_reader = vprocShellPromptReadActive(session->shell_pid);
    /* The shell itself, reading at its prompt, whichever session it has. */
    VProc *self_vp = vprocCurrent();
    const int self_pid = self_vp ? vprocPid(self_vp) : 0;
    const bool prompt_self = self_pid > 0 && vprocShellPromptReadActive(self_pid);
    const bool line_mode = vprocTtyInLineMode(session->pty_slave);
    if (tool_dbg) {
        if (!vprocSessionInputLockWithTimeout(input, 200, "read.debug")) {
            VProcSessionInput *replacement = vprocSessionInputRecoverStuck(session, input, "read.debug");
            if (replacement) {
                input = replacement;
            }
            errno = EINTR;
            return -1;
        }
        vprocDebugLogf(
                "[session-read] start off=%zu len=%zu eof=%d reader=%d fd=%d stdin=%d\n",
                input->off,
                input->len,
                (int)input->eof,
                (int)input->reader_active,
                input->reader_fd,
                session->stdin_host_fd);
        pthread_mutex_unlock(&input->mu);
    }
    if (!vprocSessionInputLockWithTimeout(input, 200, "read")) {
        VProcSessionInput *replacement = vprocSessionInputRecoverStuck(session, input, "read");
        if (replacement && replacement != input) {
            input = replacement;
            if (!vprocSessionInputLockWithTimeout(input, 200, "read.relock")) {
                errno = EINTR;
                return -1;
            }
        } else {
            errno = EINTR;
            return -1;
        }
    }
    for (;;) {
        if (nonblocking && input->len == 0 && !input->eof && !input->interrupt_pending &&
            input->eof_mark_count == 0) {
            pthread_mutex_unlock(&input->mu);
            errno = EAGAIN;
            return -1;
        }
        while (input->len == 0 && !input->eof && !input->interrupt_pending &&
               input->eof_mark_count == 0) {
            if (!prompt_self) {
                vprocCondWait(&input->cv, &input->mu);
                continue;
            }
            /* halt from a job, or kill -HUP from another tab, ends the shell now, not at the
             * next line typed. The prompt takes EINTR and acts on what came. */
            struct timespec deadline;
            vprocInprocPipeWaitDeadline(&deadline);
            if (vprocCondTimedWait(&input->cv, &input->mu, &deadline) != ETIMEDOUT) {
                continue;
            }
            pthread_mutex_unlock(&input->mu);
            bool killed = vprocPromptKillPending(self_pid);
            pthread_mutex_lock(&input->mu);
            if (killed) {
                pthread_mutex_unlock(&input->mu);
                errno = EINTR;
                return -1;
            }
        }
        if (input->interrupt_pending) {
            input->interrupt_pending = false;
            gVprocInterruptKeyRead = input->interrupt_key;
            input->interrupt_key = false;
            pthread_mutex_unlock(&input->mu);
            (void)vprocWaitIfStopped(vprocCurrent());
            errno = EINTR;
            return -1;
        }
        if (input->eof_mark_count == 0 || input->eof_mark[0] != 0) {
            break;
        }
        /* The end-of-file key, now that the input before it has gone. One
         * typed after its command ended means nothing to the prompt, which
         * would take it as the end of the session. */
        input->eof_mark_count--;
        memmove(input->eof_mark, input->eof_mark + 1,
                input->eof_mark_count * sizeof(input->eof_mark[0]));
        if (!prompt_reader) {
            pthread_mutex_unlock(&input->mu);
            return 0;
        }
    }
    if (input->len == 0 && input->eof) {
        if (tool_dbg) {
            vprocDebugLogf( "[session-read] eof\n");
        }
        pthread_mutex_unlock(&input->mu);
        return 0;
    }
    size_t to_copy = (count < input->len) ? count : input->len;
    if (input->eof_mark_count > 0 && to_copy > input->eof_mark[0]) {
        to_copy = input->eof_mark[0];
    }
    if (line_mode) {
        /* One line per read, as a terminal gives: what is typed ahead of the
         * next line belongs to whoever reads next, not to this read's buffer. */
        const unsigned char *newline = memchr(input->buf + input->off, '\n', to_copy);
        if (newline) {
            to_copy = (size_t)(newline - (input->buf + input->off)) + 1;
        }
    }
    memcpy(buf, input->buf + input->off, to_copy);
    input->off += to_copy;
    input->len -= to_copy;
    if (input->len == 0) {
        input->off = 0;
    }
    for (size_t i = 0; i < input->eof_mark_count; ++i) {
        input->eof_mark[i] -= to_copy;
    }
    pthread_mutex_unlock(&input->mu);
    return (ssize_t)to_copy;
}

ssize_t vprocSessionReadInputShim(void *buf, size_t count) {
    return vprocSessionReadInputShimMode(buf, count, false);
}

ssize_t vprocSessionReadInputShimMode(void *buf, size_t count, bool nonblocking) {
    if (!buf || count == 0) {
        errno = EINVAL;
        return -1;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        errno = EBADF;
        return -1;
    }
    const bool tool_dbg = vprocToolDebugEnabled();
    int shell_pid = vprocResolveSessionShellPidForInput(session);
    VProcSessionInput *input = vprocSessionInputEnsure(session,
                                                       shell_pid,
                                                       vprocGetKernelPid());
    if (!input) {
        errno = EBADF;
        return -1;
    }
    if (tool_dbg) {
        vprocDebugLogf("[session-read] buffered stdin=%d nonblock=%d\n",
                       session->stdin_host_fd,
                       (int)nonblocking);
    }
    return vprocSessionReadInput(session, buf, count, nonblocking);
}

void vprocSessionInputStop(VProcSessionStdio *session) {
    VProcSessionInput *input = session ? session->input : NULL;
    if (!input) {
        return;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    pthread_mutex_lock(&input->mu);
    input->stop_requested = true;
    pthread_cond_broadcast(&input->cv);
    while (input->reader_active) {
        if (vprocCondTimedWait(&input->cv, &input->mu, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&input->mu);
}

VProcSessionInput *vprocSessionInputEnsureShim(void) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        return NULL;
    }
    int shell_pid = vprocResolveSessionShellPidForInput(session);
    int kernel_pid = vprocGetKernelPid();
    const bool tool_dbg = vprocToolDebugEnabled();
    if (tool_dbg) {
        vprocDebugLogf(
                "[session-input] ensure shell=%d kernel=%d stdin_host=%d input=%p\n",
                shell_pid,
                kernel_pid,
                session->stdin_host_fd,
                (void *)session->input);
    }
    return vprocSessionInputEnsure(session, shell_pid, kernel_pid);
}

bool vprocSessionInjectInputShim(const void *data, size_t len) {
    if (!data || len == 0) {
        return false;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        return false;
    }
    int shell_pid = vprocResolveSessionShellPidForInput(session);
    int kernel_pid = vprocGetKernelPid();
    VProcSessionInput *input = vprocSessionInputEnsure(session, shell_pid, kernel_pid);
    if (!input) {
        return false;
    }
    const bool tool_dbg = vprocToolDebugEnabled();
    pthread_mutex_lock(&input->mu);
    if (len > SIZE_MAX - input->len) {
        pthread_mutex_unlock(&input->mu);
        return false;
    }
    if (!vprocEnsureSessionInputWritableLocked(input, len)) {
        pthread_mutex_unlock(&input->mu);
        return false;
    }
    memcpy(input->buf + input->off + input->len, data, len);
    input->len += len;
    pthread_cond_broadcast(&input->cv);
    if (tool_dbg) {
        vprocDebugLogf(
                "[session-input] injected len=%zu off=%zu total=%zu cap=%zu\n",
                len,
                input->off,
                input->len,
                input->cap);
    }
    pthread_mutex_unlock(&input->mu);
    pscal_fd_poll_wakeup(NULL, POLLIN);
    return true;
}

static void vprocRemoveChildLocked(VProcTaskEntry *parent, int child_pid) {
    if (!parent || !parent->children || parent->child_count == 0) return;
    for (size_t i = 0; i < parent->child_count;) {
        if (parent->children[i] == child_pid) {
            parent->children[i] = parent->children[parent->child_count - 1];
            parent->child_count--;
            continue;
        }
        ++i;
    }
}

static void vprocUpdateParentLocked(int child_pid, int new_parent_pid) {
    if (child_pid <= 0) return;
    if (new_parent_pid == child_pid) {
        /* Reject self-parenting to avoid cycles in parent/child bookkeeping. */
        new_parent_pid = 0;
    }
    VProcTaskEntry *child_entry = vprocTaskFindLocked(child_pid);
    if (!child_entry) return;
    int old_parent = child_entry->parent_pid;
    if (old_parent == new_parent_pid) return;
    if (old_parent > 0) {
        VProcTaskEntry *old_parent_entry = vprocTaskFindLocked(old_parent);
        if (old_parent_entry) {
            vprocRemoveChildLocked(old_parent_entry, child_pid);
        }
    }
    VProcTaskEntry *new_parent_entry = NULL;
    if (new_parent_pid > 0) {
        /* Host-side parents may not have synthetic task entries; preserve the
         * numeric parent for wait semantics and only track child links for
         * parents that exist in the task table. */
        new_parent_entry = vprocTaskFindLocked(new_parent_pid);
    }
    child_entry->parent_pid = new_parent_pid;
    if (new_parent_entry) {
        /* Some host-backed synthetic tasks are created before their shell
         * parent entry exists; once we have a concrete parent, adopt its
         * session id so job/session filtering stays consistent. */
        if (!child_entry->session_leader &&
            child_entry->sid == child_entry->pid &&
            new_parent_entry->sid > 0 &&
            new_parent_entry->sid != child_entry->pid) {
            child_entry->sid = new_parent_entry->sid;
        }
        if (!vprocAddChildLocked(new_parent_entry, child_pid)) {
            child_entry->parent_pid = 0;
        }
    }
}

static void vprocReparentChildrenLocked(int parent_pid, int new_parent_pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(parent_pid);
    if (!entry || entry->child_count == 0 || !entry->children) {
        if (entry) {
            entry->child_count = 0;
        }
        return;
    }
    while (entry->child_count > 0) {
        int child_pid = entry->children[entry->child_count - 1];
        entry->child_count--;
        if (child_pid > 0) {
            vprocUpdateParentLocked(child_pid, new_parent_pid);
        }
    }
}

/* A parent that ended leaves no one to wait for its children, and who takes them in only waits
 * for its own jobs. So an orphan's row goes when it ends, as init reaps it on Linux. */
static void vprocOrphanChildrenLocked(int parent_pid, int new_parent_pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(parent_pid);
    if (!entry || entry->child_count == 0 || !entry->children) {
        if (entry) {
            entry->child_count = 0;
        }
        return;
    }
    while (entry->child_count > 0) {
        int child_pid = entry->children[entry->child_count - 1];
        entry->child_count--;
        if (child_pid <= 0) {
            continue;
        }
        vprocUpdateParentLocked(child_pid, new_parent_pid);
        VProcTaskEntry *child = vprocTaskFindLocked(child_pid);
        if (child) {
            child->orphaned = true;
            child->adopted = true;
            if (child->exited) {
                vprocClearEntryLocked(child);
            }
        }
    }
}

/* The kernel's task is init: it waits for nothing, so it reaps a child of its own the moment that
 * child ends. A child of the kernel in another task's session stays for that session's leader to
 * wait for (vprocWaitParentMatchesLocked), until the leader ends too. A tab's shell and an ssh
 * session are the kernel's children, each its own session's leader, and stayed as zombies. */
static bool vprocKernelReapsLocked(const VProcTaskEntry *entry) {
    int kernel_pid = vprocGetKernelPid();
    if (!entry || kernel_pid <= 0 || entry->pid == kernel_pid || entry->parent_pid != kernel_pid) {
        return false;
    }
    if (entry->sid <= 0 || entry->sid == entry->pid || entry->sid == kernel_pid) {
        return true;
    }
    const VProcTaskEntry *leader = vprocTaskFindLocked(entry->sid);
    return !leader || leader->exited;
}

/* A session leader that ends leaves the kernel's ended children in its session to the kernel. */
static void vprocKernelReapSessionLocked(int sid) {
    int kernel_pid = vprocGetKernelPid();
    if (sid <= 0 || kernel_pid <= 0) {
        return;
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0 || entry->pid == sid || entry->sid != sid) continue;
        if (entry->parent_pid != kernel_pid || !entry->exited) continue;
        entry->zombie = false;
        vprocClearEntryLocked(entry);
    }
}

static void vprocNotifyPidSigchldLocked(int target_pid, VProcSigchldEvent evt) {
    /* The kernel reaps its children as they end and has no handler: a SIGCHLD only stayed
     * pending on it for good. */
    if (target_pid <= 0 || target_pid == vprocGetKernelPid()) {
        return;
    }
    VProcTaskEntry *target_entry = vprocTaskFindLocked(target_pid);
    if (!target_entry) {
        return;
    }
    if (evt == VPROC_SIGCHLD_EVENT_STOP) {
        struct sigaction sa = vprocGetSigactionLocked(target_entry, SIGCHLD);
        if (sa.sa_flags & SA_NOCLDSTOP) {
            return;
        }
    }
    target_entry->sigchld_events++;
    vprocQueuePendingSignalLocked(target_entry, SIGCHLD);
    if (!target_entry->sigchld_blocked) {
        target_entry->sigchld_delivery_queued = false;
        (void)vprocDeliverPendingSignalsLocked(target_entry);
    }
}

static void vprocNotifyParentSigchldLocked(const VProcTaskEntry *child, VProcSigchldEvent evt) {
    if (!child || child->silent_to_parent) {
        return;
    }
    int parent_pid = child->parent_pid;
    vprocNotifyPidSigchldLocked(parent_pid, evt);

    int kernel_pid = vprocGetKernelPid();
    if (kernel_pid > 0 && parent_pid == kernel_pid) {
        int sid = child->sid;
        if (sid > 0 && sid != parent_pid && sid != child->pid) {
            vprocNotifyPidSigchldLocked(sid, evt);
        }
    }
}

static int vprocWaiterPid(void) {
    VProc *cur = vprocCurrent();
    if (cur) {
        return vprocPid(cur);
    }
    int shell = vprocGetShellSelfPid();
    if (shell > 0) {
        return shell;
    }
    return (int)vprocHostGetpidRaw();
}

static bool vprocEnsureFdCapacityLocked(VProc *vp, size_t needed) {
    if (!vp || needed == 0) {
        errno = EINVAL;
        return false;
    }
    size_t max_fd_capacity = (size_t)INT_MAX + 1u;
    if (needed > max_fd_capacity) {
        errno = EMFILE;
        return false;
    }
    if (needed <= vp->capacity) {
        return true;
    }
    size_t new_cap = 0;
    if (!vprocComputeGrowthCapacity(vp->capacity,
                                    needed,
                                    VPROC_INITIAL_CAPACITY,
                                    sizeof(VProcFdEntry),
                                    &new_cap)) {
        errno = EMFILE;
        return false;
    }
    if (new_cap > max_fd_capacity) {
        new_cap = max_fd_capacity;
    }
    if (new_cap < needed) {
        errno = EMFILE;
        return false;
    }
    VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
    if (!resized) {
        errno = ENOMEM;
        return false;
    }
    for (size_t i = vp->capacity; i < new_cap; ++i) {
        resized[i].host_fd = -1;
        resized[i].pscal_fd = NULL;
        resized[i].kind = VPROC_FD_NONE;
        resized[i].inherit = false;
    }
    vp->entries = resized;
    vp->capacity = new_cap;
    return true;
}

// NOTE: Caller must hold vp->mu
static int vprocAllocSlot(VProc *vp) {
    if (!vp) {
        return -1;
    }
    if (vp->capacity > (size_t)INT_MAX) {
        errno = EMFILE;
        return -1;
    }
    int capacity = (int)vp->capacity;
    if (capacity <= 0) {
        return -1;
    }
    /* ulimit -n: only slots under the limit can be handed out. */
    int all_slots = capacity;
    rlim_t open_files = vp->limits[RLIMIT_NOFILE].rlim_cur;
    if (open_files != RLIM_INFINITY && open_files < (rlim_t)capacity) {
        capacity = (int)open_files;
        if (capacity <= 0) {
            errno = EMFILE;
            return -1;
        }
    }
    int start = vp->next_fd;
    if (start < 0 || start >= capacity) {
        start %= capacity;
        if (start < 0) {
            start += capacity;
        }
    }
    for (int idx = start; idx < capacity; ++idx) {
        if (vp->entries[idx].kind == VPROC_FD_NONE) {
            vp->next_fd = (idx + 1) % capacity;
            return idx;
        }
    }
    for (int idx = 0; idx < start; ++idx) {
        if (vp->entries[idx].kind == VPROC_FD_NONE) {
            vp->next_fd = (idx + 1) % capacity;
            return idx;
        }
    }
    size_t old_cap = vp->capacity;
    if (capacity < all_slots || (open_files != RLIM_INFINITY && old_cap >= open_files)) {
        errno = EMFILE;
        return -1;
    }
    if (!vprocEnsureFdCapacityLocked(vp, old_cap + 1)) {
        return -1;
    }
    int idx = (int)old_cap;
    vp->next_fd = (idx + 1) % (int)vp->capacity;
    return idx;
}

// NOTE: Caller must hold vp->mu, or wrap this
static int vprocInsertLocked(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        errno = EBADF;
        return -1;
    }
    int slot = vprocAllocSlot(vp);
    if (slot < 0) {
        return -1;
    }
    vp->entries[slot].host_fd = host_fd;
    vp->entries[slot].pscal_fd = NULL;
    vp->entries[slot].kind = VPROC_FD_HOST;
    vp->entries[slot].inherit = false;
    vprocResourceTrackLocked(vp, host_fd, VPROC_RESOURCE_GENERIC);
    return slot;
}

static int vprocInsert(VProc *vp, int host_fd) {
    if (!vp) return -1;
    pthread_mutex_lock(&vp->mu);
    int rc = vprocInsertLocked(vp, host_fd);
    pthread_mutex_unlock(&vp->mu);
    return rc;
}

int vprocAdoptHostFd(VProc *vp, int host_fd) {
    return vprocInsert(vp, host_fd);
}

int vprocInsertPscalFd(VProc *vp, struct pscal_fd *fd) {
    if (!vp || !fd) {
        errno = EBADF;
        return -1;
    }
    /* The fd table must own its own reference so callers can freely drop
     * theirs after insertion. */
    struct pscal_fd *retained = pscal_fd_retain(fd);
    if (!retained) {
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    int slot = vprocAllocSlot(vp);
    if (slot >= 0) {
        vp->entries[slot].host_fd = -1;
        vp->entries[slot].pscal_fd = retained;
        vp->entries[slot].kind = VPROC_FD_PSCAL;
        vp->entries[slot].inherit = false;
    }
    pthread_mutex_unlock(&vp->mu);
    if (slot < 0) {
        pscal_fd_close(retained);
    }
    return slot;
}

struct pscal_fd *vprocGetPscalFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        return NULL;
    }
    struct pscal_fd *res = NULL;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity && vp->entries[fd].kind == VPROC_FD_PSCAL) {
        res = pscal_fd_retain(vp->entries[fd].pscal_fd);
    }
    pthread_mutex_unlock(&vp->mu);
    return res;
}

/* Callers hold vp->mu, and fcntl is a cancellation point. A thread cancelled here would die
 * holding the lock that the next vprocSnapshot waits for, with the task table locked. */
static int vprocCloneFd(int source_fd) {
    int cancel_state = PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    int duped = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0 && errno == EINVAL) {
        duped = fcntl(source_fd, F_DUPFD, 0);
    }
    if (duped < 0) {
        duped = vprocHostDupRaw(source_fd);
    }
    if (duped >= 0) {
        fcntl(duped, F_SETFD, FD_CLOEXEC);
    }
    int saved_errno = errno;
    pthread_setcancelstate(cancel_state, NULL);
    errno = saved_errno;
    return duped;
}

static bool vprocPathMatches(const char *path, const char *target) {
    if (!path || !target) {
        return false;
    }
    if (strcmp(path, target) == 0) {
        return true;
    }
    /* Allow any sandbox path that still ends with the target (e.g., /var/.../dev/location). */
    const char *sub = strstr(path, target);
    if (sub && strcmp(sub, target) == 0) {
        return true;
    }
    /* Allow sandboxed prefixes (e.g., /private/var/.../dev/location). */
    size_t target_len = strlen(target);
    size_t path_len = strlen(path);
    if (path_len >= target_len) {
        if (strcmp(path + (path_len - target_len), target) == 0) {
            return true;
        }
    }
    if (strncmp(path, "/private", 8) == 0) {
        const char *trimmed = path + 8;
        if (strcmp(trimmed, target) == 0) {
            return true;
        }
    }
    return false;
}

static bool vprocLocationDebugEnabled(void) {
    const char *env = getenv("PSCALI_LOCATION_DEBUG");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) {
        return true;
    }
    return false;
}

static void vprocLocationDebugf(const char *fmt, ...) {
    if (!fmt || !vprocLocationDebugEnabled()) {
        return;
    }
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    vprocDebugLogf("%s", buf);
}

static void vprocEnsurePathParent(const char *path) {
    if (!path || path[0] != '/') {
        return;
    }
    char buf[PATH_MAX];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0777);
            buf[i] = '/';
        }
    }
}

static void vprocLocationEnsureStubPath(const char *path, int *opened_fd) {
    if (!path) {
        return;
    }
    if (vprocLocationDebugEnabled()) {
        vprocLocationDebugf("ensuring location stub at %s", path);
    }
    vprocEnsurePathParent(path);
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            if (unlink(path) != 0 && vprocLocationDebugEnabled()) {
                vprocLocationDebugf("failed to unlink non-fifo location stub %s: %s",
                                    path,
                                    strerror(errno));
            }
        }
    }
    if (mkfifo(path, 0666) != 0 && errno != EEXIST) {
        if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("failed to mkfifo location stub at %s: %s",
                                path,
                                strerror(errno));
        }
    }
    if (opened_fd && *opened_fd < 0) {
        int fd = vprocHostOpenRawInternal(path, O_RDWR | O_NONBLOCK, 0666, true);
        if (fd >= 0) {
            *opened_fd = fd;
            if (vprocLocationDebugEnabled()) {
                vprocLocationDebugf("opened location stub fifo fd=%d path=%s", fd, path);
            }
        } else if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("failed to open location stub fifo %s: %s",
                                path,
                                strerror(errno));
        }
    }
}

static void vprocLocationDeviceEnsureStub(void) {
#if defined(PSCAL_TARGET_IOS)
    char expanded[PATH_MAX];
    const char *path = kLocationDevicePath;
    if (pathTruncateEnabled() && pathTruncateExpand(kLocationDevicePath, expanded, sizeof(expanded))) {
        path = expanded;
    }
    vprocLocationEnsureStubPath(path, &gLocationDevice.stub_fd);
    /* Keep legacy aliases discoverable and open to avoid FIFO open blocking. */
    const char *gps_paths[] = { kLegacyGpsDevicePath, kLegacyGpsDevicePath2 };
    for (size_t i = 0; i < sizeof(gps_paths) / sizeof(gps_paths[0]); ++i) {
        const char *gps = gps_paths[i];
        const char *target = gps;
        char expanded_gps[PATH_MAX];
        if (pathTruncateEnabled() && pathTruncateExpand(gps, expanded_gps, sizeof(expanded_gps))) {
            target = expanded_gps;
        }
        vprocLocationEnsureStubPath(target, NULL);
    }
#else
    (void)kLocationDevicePath;
#endif
}

static bool vprocPathIsLocationDevice(const char *path) {
    if (vprocPathMatches(path, kLocationDevicePath)) {
        return true;
    }
    /* Backward compatibility: some callers still open /dev/gps. */
    if (vprocPathMatches(path, kLegacyGpsDevicePath)) {
        return true;
    }
    return vprocPathMatches(path, kLegacyGpsDevicePath2);
}

static bool vprocPathIsLegacyGpsDevice(const char *path) {
    return vprocPathMatches(path, kLegacyGpsDevicePath) || vprocPathMatches(path, kLegacyGpsDevicePath2);
}

static bool vprocPathIsDevTty(const char *path) {
    return vprocPathMatches(path, "/dev/tty") || vprocPathMatches(path, "/private/dev/tty");
}

static bool vprocPathIsDevConsole(const char *path) {
    return vprocPathMatches(path, "/dev/console") || vprocPathMatches(path, "/private/dev/console");
}

static bool vprocPathIsPtyMaster(const char *path) {
    return vprocPathMatches(path, "/dev/ptmx") ||
           vprocPathMatches(path, "/private/dev/ptmx") ||
           vprocPathMatches(path, "/dev/pts/ptmx") ||
           vprocPathMatches(path, "/private/dev/pts/ptmx");
}

static bool vprocPathIsDevPtsRoot(const char *path) {
    return vprocPathMatches(path, "/dev/pts") || vprocPathMatches(path, "/private/dev/pts");
}

static bool vprocPathIsDevPtsNode(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    const char *candidate = path;
    if (strncmp(candidate, "/private", 8) == 0) {
        candidate += 8;
    }
    if (strncmp(candidate, "/dev/pts/", 9) != 0) {
        return false;
    }
    return candidate[9] != '\0';
}

static bool vprocPathIsDevTtyFamily(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    if (vprocPathIsDevTty(path)) {
        return true;
    }
    const char *candidate = path;
    if (strncmp(candidate, "/private", 8) == 0) {
        candidate += 8;
    }
    return strncmp(candidate, "/dev/tty", 8) == 0;
}

static bool vprocPathRequiresRawHostOpen(const char *path) {
    return vprocPathIsDevConsole(path) ||
           vprocPathIsPtyMaster(path) ||
           vprocPathIsDevPtsRoot(path) ||
           vprocPathIsDevPtsNode(path) ||
           vprocPathIsDevTtyFamily(path);
}

static bool vprocPathIsUsrBinTree(const char *path) {
    static const char *kUsrBin = "/usr/bin";
    if (!path || path[0] != '/') {
        return false;
    }
    size_t len = strlen(kUsrBin);
    if (strncmp(path, kUsrBin, len) != 0) {
        return false;
    }
    return path[len] == '\0' || path[len] == '/';
}

static bool vprocPathIsSystemPath(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    if (vprocPathIsUsrBinTree(path)) {
        return false;
    }
    const char *prefixes[] = { "/System", "/usr", "/Library", "/Applications" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t len = strlen(prefix);
        if (strncmp(path, prefix, len) != 0) {
            continue;
        }
        if (path[len] == '\0' || path[len] == '/') {
            return true;
        }
    }
    return false;
}

static bool vprocNormalizeAbsolutePath(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size < 2 || input[0] != '/') {
        errno = EINVAL;
        return false;
    }

    size_t anchors[PATH_MAX / 2];
    size_t depth = 0;
    size_t length = 1;
    out[0] = '/';
    out[1] = '\0';

    const char *cursor = input;
    while (*cursor == '/') {
        cursor++;
    }
    while (*cursor != '\0') {
        const char *segment_start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        size_t segment_len = (size_t)(cursor - segment_start);
        while (*cursor == '/') {
            cursor++;
        }
        if (segment_len == 0) {
            continue;
        }
        if (segment_len == 1 && segment_start[0] == '.') {
            continue;
        }
        if (segment_len == 2 && segment_start[0] == '.' && segment_start[1] == '.') {
            if (depth > 0) {
                depth--;
                length = anchors[depth];
                out[length] = '\0';
            }
            continue;
        }
        if (depth >= sizeof(anchors) / sizeof(anchors[0])) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (length > 1) {
            if (length + 1 >= out_size) {
                errno = ENAMETOOLONG;
                return false;
            }
            out[length++] = '/';
        }
        size_t segment_start_pos = length;
        if (length + segment_len >= out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(out + length, segment_start, segment_len);
        length += segment_len;
        out[length] = '\0';

        size_t anchor = (segment_start_pos > 1) ? (segment_start_pos - 1) : 1;
        anchors[depth++] = anchor;
    }
    return true;
}

static void vprocCurrentVirtualCwd(VProc *vp, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!vp) {
        snprintf(out, out_size, "/");
        return;
    }
    pthread_mutex_lock(&vp->mu);
    const char *cwd = (vp->cwd[0] == '/') ? vp->cwd : "/";
    snprintf(out, out_size, "%s", cwd);
    pthread_mutex_unlock(&vp->mu);
}

static void vprocSetVirtualCwd(VProc *vp, const char *cwd) {
    if (!vp) {
        return;
    }
    const char *safe = (cwd && cwd[0] == '/') ? cwd : "/";
    pthread_mutex_lock(&vp->mu);
    snprintf(vp->cwd, sizeof(vp->cwd), "%s", safe);
    pthread_mutex_unlock(&vp->mu);
}

static bool vprocResolveVirtualPath(VProc *vp, const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size < 2) {
        errno = EINVAL;
        return false;
    }
    if (!path[0]) {
        /* An empty path names nothing. Taken as the cwd, `rm -rf ""` walks it
         * and removes "/<child>" for each entry, from the virtual root. */
        errno = ENOENT;
        return false;
    }
    if (path[0] == '/') {
        const char *source = path;
        char stripped[PATH_MAX];
        /*
         * Callers may pass already-expanded container paths. Convert them back
         * to virtual form first so we don't re-expand and fail lookups.
         */
        if (pathTruncateEnabled() && pathTruncateStrip(path, stripped, sizeof(stripped))) {
            source = stripped;
        }
        return vprocNormalizeAbsolutePath(source, out, out_size);
    }
    if (!vp) {
        errno = ENOENT;
        return false;
    }
    char cwd[PATH_MAX];
    vprocCurrentVirtualCwd(vp, cwd, sizeof(cwd));
    char joined[PATH_MAX];
    int written = snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    if (written < 0 || (size_t)written >= sizeof(joined)) {
        errno = ENAMETOOLONG;
        return false;
    }
    return vprocNormalizeAbsolutePath(joined, out, out_size);
}

/* The number a path names and the vproc whose table it is in. */
static int vprocPathNamedFdOf(const char *path, int *out_pid) {
    if (!path || !path[0]) {
        return -1;
    }
    /* Nearly every absolute path stops here, before anything is built. A relative one can be
     * a bare number in /dev/fd. */
    if (path[0] == '/' && !strstr(path, "/std") && !strstr(path, "/fd/")) {
        return -1;
    }
    VProc *vp = vprocForThread();
    char resolved[PATH_MAX];
    if (!vp || !vprocResolveVirtualPath(vp, path, resolved, sizeof(resolved))) {
        return -1;
    }
    if (out_pid) {
        *out_pid = vprocPid(vp);
    }
    const char *name = resolved;
    if (strncmp(name, "/private/dev/", 13) == 0 || strncmp(name, "/private/proc/", 14) == 0) {
        name += 8;
    }
    if (strcmp(name, "/dev/stdin") == 0) {
        return STDIN_FILENO;
    }
    if (strcmp(name, "/dev/stdout") == 0) {
        return STDOUT_FILENO;
    }
    if (strcmp(name, "/dev/stderr") == 0) {
        return STDERR_FILENO;
    }
    const char *digits = NULL;
    if (strncmp(name, "/dev/fd/", 8) == 0) {
        digits = name + 8;
    } else if (strncmp(name, "/proc/self/fd/", 14) == 0) {
        digits = name + 14;
    } else if (strncmp(name, "/proc/", 6) == 0) {
        char *end = NULL;
        long pid = strtol(name + 6, &end, 10);
        if (end != name + 6 && strncmp(end, "/fd/", 4) == 0 && pid > 0 && pid <= INT_MAX &&
            vprocFindByPid((int)pid)) {
            digits = end + 4;
            if (out_pid) {
                *out_pid = (int)pid;
            }
        }
    }
    if (!digits || !*digits) {
        return -1;
    }
    long fd = 0;
    for (; *digits; ++digits) {
        if (*digits < '0' || *digits > '9' || fd > INT_MAX / 10) {
            return -1;
        }
        fd = fd * 10 + (*digits - '0');
    }
    return fd <= INT_MAX ? (int)fd : -1;
}

int vprocPathNamedFd(const char *path) {
    return vprocPathNamedFdOf(path, NULL);
}

static bool vprocPathParseConsoleTty(const char *path, int *out_num);

bool vprocPathIsTerminal(const char *path) {
    int num = -1;
    return vprocPathIsDevTty(path) || vprocPathIsDevConsole(path) || vprocPathIsPtyMaster(path) ||
           vprocPathIsDevPtsNode(path) || vprocPathParseConsoleTty(path, &num);
}

static void vprocInitializeVirtualCwd(VProc *vp, VProc *active) {
    if (!vp) {
        return;
    }
    char initial[PATH_MAX];
    initial[0] = '\0';
    if (active) {
        vprocCurrentVirtualCwd(active, initial, sizeof(initial));
    }
    if (initial[0] == '\0') {
        if (vprocHostGetcwdRaw(initial, sizeof(initial)) == NULL) {
            snprintf(initial, sizeof(initial), "/");
        } else if (pathTruncateEnabled()) {
            char stripped[PATH_MAX];
            if (pathTruncateStrip(initial, stripped, sizeof(stripped))) {
                snprintf(initial, sizeof(initial), "%s", stripped);
            }
        }
    }
    char normalized[PATH_MAX];
    if (!vprocNormalizeAbsolutePath(initial, normalized, sizeof(normalized))) {
        snprintf(normalized, sizeof(normalized), "/");
    }
    vprocSetVirtualCwd(vp, normalized);
}

static const char *vprocPathExpandForShim(const char *path, char *buf, size_t buf_size) {
    if (!path) {
        return NULL;
    }
    const char *virtual_path = path;
    char virtual_buf[PATH_MAX];
    if (path[0] == '/') {
        if (vprocNormalizeAbsolutePath(path, virtual_buf, sizeof(virtual_buf))) {
            virtual_path = virtual_buf;
        }
    } else {
        VProc *vp = vprocForThread();
        if (!vp) {
            vp = vprocResolveShellVprocForPathOps();
        }
        if (vp && vprocResolveVirtualPath(vp, path, virtual_buf, sizeof(virtual_buf))) {
            virtual_path = virtual_buf;
        }
    }
    if (vprocPathIsSystemPath(virtual_path)) {
        if (virtual_path != path) {
            if (buf && buf_size > 0) {
                int written = snprintf(buf, buf_size, "%s", virtual_path);
                if (written > 0 && (size_t)written < buf_size) {
                    return buf;
                }
            }
            return path;
        }
        return virtual_path;
    }
    if (pathTruncateEnabled() && pathTruncateExpand(virtual_path, buf, buf_size)) {
        return buf;
    }
    if (virtual_path != path) {
        if (buf && buf_size > 0) {
            int written = snprintf(buf, buf_size, "%s", virtual_path);
            if (written > 0 && (size_t)written < buf_size) {
                return buf;
            }
        }
        return path;
    }
    return path;
}

static bool vprocPathParsePtySlave(const char *path, int *out_num) {
    if (!path || !out_num) {
        return false;
    }
    const char *prefixes[] = { "/dev/pts/", "/private/dev/pts/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(path, prefix, prefix_len) != 0) {
            continue;
        }
        const char *num_str = path + prefix_len;
        if (*num_str == '\0') {
            return false;
        }
        char *end = NULL;
        long parsed = strtol(num_str, &end, 10);
        if (!end || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
            return false;
        }
        *out_num = (int)parsed;
        return true;
    }
    return false;
}

static int vprocPtyApplyAttrsByNum(int pty_num,
                                   const mode_t *mode,
                                   const uid_t *uid,
                                   const gid_t *gid) {
    mode_t_ perms_local;
    uid_t_ uid_local;
    gid_t_ gid_local;
    const mode_t_ *perms_ptr = NULL;
    const uid_t_ *uid_ptr = NULL;
    const gid_t_ *gid_ptr = NULL;
    if (mode) {
        perms_local = (mode_t_)(*mode & 0777);
        perms_ptr = &perms_local;
    }
    if (uid) {
        uid_local = (uid_t_)(*uid);
        uid_ptr = &uid_local;
    }
    if (gid) {
        gid_local = (gid_t_)(*gid);
        gid_ptr = &gid_local;
    }
    return pscalPtySetSlaveInfo(pty_num, perms_ptr, uid_ptr, gid_ptr);
}

static int vprocPtyApplyAttrsForFd(struct pscal_fd *fd,
                                   const mode_t *mode,
                                   const uid_t *uid,
                                   const gid_t *gid) {
    if (!fd || !fd->tty) {
        return _EBADF;
    }
    struct tty *tty = fd->tty;
    if (tty->driver == &pty_master) {
        tty = tty->pty.other;
    }
    if (!tty || tty->driver != &pty_slave) {
        return _ENOTTY;
    }
    return vprocPtyApplyAttrsByNum(tty->num, mode, uid, gid);
}

static bool vprocPathParseConsoleTty(const char *path, int *out_num) {
    if (!path || !out_num) {
        return false;
    }
    const char *prefixes[] = { "/dev/tty", "/private/dev/tty" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(path, prefix, prefix_len) != 0) {
            continue;
        }
        const char *num_str = path + prefix_len;
        if (*num_str == '\0') {
            return false;
        }
        char *end = NULL;
        long parsed = strtol(num_str, &end, 10);
        if (!end || *end != '\0' || parsed <= 0 || parsed > INT_MAX) {
            return false;
        }
        *out_num = (int)parsed;
        return true;
    }
    return false;
}

static void vprocLocationDeviceCloseLocked(void) {
    if (gLocationDevice.stub_fd >= 0) {
        vprocHostCloseRaw(gLocationDevice.stub_fd);
        gLocationDevice.stub_fd = -1;
    }
    gLocationDevice.readers = 0;
    gLocationDevice.has_payload = false;
    gLocationDevice.last_len = 0;
}

static int vprocLocationDeviceEnsurePipeLocked(void) {
    return 0;
}

static void vprocLocationEnsureCond(void) {
    if (!gLocationDevice.cond_init) {
        pthread_cond_init(&gLocationDevice.cond, NULL);
        gLocationDevice.cond_init = true;
    }
}

static int vprocLocationDeviceOpenHost(int flags) {
    int access_mode = flags & O_ACCMODE;
    if (access_mode == O_WRONLY) {
        errno = EACCES;
        return -1;
    }
    bool debug = vprocLocationDebugEnabled();
    pthread_mutex_lock(&gLocationDevice.mu);
    if (!gLocationDevice.enabled) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        if (debug) {
            vprocLocationDebugf("open /dev/location denied (device disabled)");
        }
        errno = ENOENT;
        return -1;
    }
    /* No direct host support in the per-open blocking model. */
    pthread_mutex_unlock(&gLocationDevice.mu);
    errno = ENOTSUP;
    return -1;
}

static int vprocLocationDeviceOpen(VProc *vp, int flags) {
    (void)flags;
    if (!vp) {
        return vprocLocationDeviceOpenHost(flags);
    }
    struct pscal_fd *loc_fd = pscal_fd_create(&kVprocLocationFdOps);
    if (!loc_fd) {
        errno = ENOMEM;
        return -1;
    }
    VProcLocationReader *reader = (VProcLocationReader *)calloc(1, sizeof(VProcLocationReader));
    if (!reader) {
        pscal_fd_close(loc_fd);
        errno = ENOMEM;
        return -1;
    }
    reader->last_seq = 0;
    loc_fd->userdata = reader;
    pthread_mutex_lock(&gLocationDevice.mu);
    vprocLocationEnsureCond();
    /* New readers should only observe writes that occur after open(). */
    reader->last_seq = gLocationDevice.seq;
    gLocationDevice.readers++;
    int readers = gLocationDevice.readers;
    pthread_mutex_unlock(&gLocationDevice.mu);
    vprocLocationNotifyObservers(readers);
    int slot = vprocInsertPscalFd(vp, loc_fd);
    if (slot < 0) {
        free(reader);
        pscal_fd_close(loc_fd);
        pthread_mutex_lock(&gLocationDevice.mu);
        if (gLocationDevice.readers > 0) {
            gLocationDevice.readers--;
        }
        readers = gLocationDevice.readers;
        pthread_mutex_unlock(&gLocationDevice.mu);
        vprocLocationNotifyObservers(readers);
    }
    else {
        /* Transfer ownership to the fd table. */
        pscal_fd_close(loc_fd);
    }
    return slot;
}

static ssize_t vprocLocationPscalRead(struct pscal_fd *fd, void *buf, size_t bufsize) {
    if (!fd || !buf || bufsize == 0) {
        return _EINVAL;
    }
    VProcLocationReader *reader = (VProcLocationReader *)fd->userdata;
    if (!reader) {
        return _EBADF;
    }
    if (reader->done) {
        return 0;
    }
    pthread_mutex_lock(&gLocationDevice.mu);
    vprocLocationEnsureCond();
    while (gLocationDevice.enabled && !reader->done) {
        /* If the reader already has buffered data, serve it first. */
        if (reader->len > reader->offset) {
            break;
        }
        /* If there is a fresh payload, copy it into the reader-local buffer. */
        if (gLocationDevice.has_payload && reader->last_seq != gLocationDevice.seq) {
            size_t copy_len = gLocationDevice.last_len;
            if (copy_len >= sizeof(reader->payload)) {
                copy_len = sizeof(reader->payload) - 1;
            }
            memcpy(reader->payload, gLocationDevice.last_payload, copy_len);
            reader->payload[copy_len] = '\0';
            reader->len = copy_len;
            reader->offset = 0;
            reader->last_seq = gLocationDevice.seq;
            break;
        }
        if (fd->flags & O_NONBLOCK) {
            pthread_mutex_unlock(&gLocationDevice.mu);
            return _EAGAIN;
        }
        vprocCondWait(&gLocationDevice.cond, &gLocationDevice.mu);
    }
    if (!gLocationDevice.enabled) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        reader->done = true;
        return 0;
    }
    /* No data available and already marked done: return EOF. */
    if (reader->done) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        return 0;
    }
    size_t remaining = reader->len - reader->offset;
    /* Deliver the entire payload in one read; bail if the caller's buffer is too small. */
    if (remaining > bufsize) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        return _EINVAL;
    }
    memcpy(buf, reader->payload + reader->offset, remaining);
    reader->offset = 0;
    reader->len = 0;
    reader->done = true;
    pthread_mutex_unlock(&gLocationDevice.mu);
    return (ssize_t)remaining;
}

static int vprocLocationPscalPoll(struct pscal_fd *fd) {
    VProcLocationReader *reader = (VProcLocationReader *)fd->userdata;
    if (!reader) {
        return 0;
    }
    pthread_mutex_lock(&gLocationDevice.mu);
    int events = 0;
    if (reader->done || !gLocationDevice.enabled) {
        events = POLLHUP;
    } else if (reader->len > reader->offset) {
        events = POLLIN;
    } else if (gLocationDevice.has_payload && reader->last_seq != gLocationDevice.seq) {
        events = POLLIN;
    }
    pthread_mutex_unlock(&gLocationDevice.mu);
    return events;
}

static int vprocLocationPscalClose(struct pscal_fd *fd) {
    VProcLocationReader *reader = (VProcLocationReader *)fd->userdata;
    free(reader);
    pthread_mutex_lock(&gLocationDevice.mu);
    if (gLocationDevice.readers > 0) {
        gLocationDevice.readers--;
    }
    int readers = gLocationDevice.readers;
    pthread_mutex_unlock(&gLocationDevice.mu);
    vprocLocationNotifyObservers(readers);
    return 0;
}

static const struct pscal_fd_ops kVprocLocationFdOps = {
    .read = vprocLocationPscalRead,
    .write = NULL,
    .poll = vprocLocationPscalPoll,
    .ioctl_size = NULL,
    .ioctl = NULL,
    .close = vprocLocationPscalClose,
};

void vprocLocationDeviceSetEnabled(bool enabled) {
    pthread_mutex_lock(&gLocationDevice.mu);
    bool changed = (gLocationDevice.enabled != enabled);
    gLocationDevice.enabled = enabled;
    int readers = gLocationDevice.readers;
    if (!enabled) {
        if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("location device disabled (changed=%s)", changed ? "true" : "false");
        }
        vprocLocationDeviceCloseLocked();
        pthread_cond_broadcast(&gLocationDevice.cond);
        pscal_fd_poll_wakeup(NULL, POLLERR);
    } else {
        if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("location device enabled (changed=%s)", changed ? "true" : "false");
        }
        vprocLocationDeviceEnsureStub();
        if (vprocLocationDeviceEnsurePipeLocked() != 0 && vprocLocationDebugEnabled()) {
            vprocLocationDebugf("location pipe setup failed during enable: %s", strerror(errno));
        }
        pthread_cond_broadcast(&gLocationDevice.cond);
        pscal_fd_poll_wakeup(NULL, POLLIN);
    }
    pthread_mutex_unlock(&gLocationDevice.mu);
    if (changed) {
        vprocLocationNotifyObservers(readers);
    }
}

ssize_t vprocLocationDeviceWrite(const void *data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }
    bool debug = vprocLocationDebugEnabled();
    pthread_mutex_lock(&gLocationDevice.mu);
    if (!gLocationDevice.enabled) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        if (debug) {
            vprocLocationDebugf("write rejected; location device disabled");
        }
        errno = ENOENT;
        return -1;
    }
    vprocLocationEnsureCond();
    size_t copy_len = len < sizeof(gLocationDevice.last_payload) ? len : sizeof(gLocationDevice.last_payload) - 1;
    memcpy(gLocationDevice.last_payload, data, copy_len);
    gLocationDevice.last_payload[copy_len] = '\0';
    gLocationDevice.last_len = copy_len;
    gLocationDevice.has_payload = (copy_len > 0);
    gLocationDevice.seq++;
    pthread_cond_broadcast(&gLocationDevice.cond);

    if (gLocationDevice.stub_fd >= 0) {
        ssize_t wrote = vprocHostWriteRaw(gLocationDevice.stub_fd, data, len);
        if (debug && wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
            vprocLocationDebugf("stub write failed: %s", strerror(errno));
        }
    }
    pthread_mutex_unlock(&gLocationDevice.mu);
    pscal_fd_poll_wakeup(NULL, POLLIN);
    if (debug) {
        vprocLocationDebugf("write success bytes=%zu seq=%llu", len, (unsigned long long)gLocationDevice.seq);
    }
    return (ssize_t)len;
}

void vprocLocationDeviceRegisterReaderObserver(VprocLocationReadersChangedFn cb, void *context) {
    pthread_mutex_lock(&gLocationReaderObserverMu);
    gLocationReaderObserver = cb;
    gLocationReaderObserverCtx = context;
    pthread_mutex_unlock(&gLocationReaderObserverMu);
    if (cb) {
        int readers = 0;
        pthread_mutex_lock(&gLocationDevice.mu);
        readers = gLocationDevice.readers;
        pthread_mutex_unlock(&gLocationDevice.mu);
        cb(readers, context);
    }
}

static int vprocSelectHostFd(VProc *inherit_from, int option_fd, int stdno) {
    /* Explicit host fd provided: clone it. */
    if (option_fd >= 0) {
        return vprocCloneFd(option_fd);
    }
    /* Force /dev/null. */
    if (option_fd == -2) {
        int flags = (stdno == STDIN_FILENO) ? O_RDONLY : O_WRONLY;
        return vprocHostOpenRawInternal("/dev/null", flags, 0, false);
    }
    /* Otherwise inherit from the active vproc's mapping, falling back to host stdno. */
    int source = stdno;
    if (inherit_from) {
        int translated = vprocTranslateFd(inherit_from, stdno);
        if (translated >= 0) {
            source = translated;
        }
    }
    return vprocCloneFd(source);
}

int vprocReservePid(void) {
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    int shell_pid = vprocGetShellSelfPid();
    int kernel_pid = vprocGetKernelPid();
    int pid = -1;
    for (size_t attempts = 0; attempts < gVProcTasks.capacity + 1024; ++attempts) {
        int candidate = __sync_fetch_and_add(&gNextSyntheticPid, 1);
        if (candidate <= 0) {
            continue;
        }
        if (candidate == shell_pid || candidate == kernel_pid) {
            continue;
        }
        if (vprocTaskFindLocked(candidate)) {
            continue;
        }
        pid = candidate;
        break;
    }
    if (pid <= 0) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = EMFILE;
        return -1;
    }
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return -1;
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid == pid) {
        parent_pid = 0;
    }
    VProcTaskEntry *parent_entry = NULL;
    if (parent_pid > 0) {
        parent_entry = vprocTaskFindLocked(parent_pid);
    }
    vprocClearEntryLocked(entry);
    vprocInitEntryDefaultsLocked(entry, pid, parent_entry);
    entry->parent_pid = parent_pid;
    /* Reserve creates a brand-new process group; do not inherit the shell's
     * pgid/fg_pgid or later kill/pgid lookups will miss the pre-start task. */
    entry->pgid = pid;
    entry->fg_pgid = pid;
    if (parent_entry && parent_pid > 0) {
        if (!vprocAddChildLocked(parent_entry, pid)) {
            entry->parent_pid = 0;
        }
    }
    if (gVProcTasks.items && gVProcTasks.count > 0) {
        ptrdiff_t idx = entry - gVProcTasks.items;
        if (idx >= 0 && (size_t)idx < gVProcTasks.count) {
            vprocTaskLookupRememberLocked(pid, (size_t)idx);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pid;
}

static void vprocMaybeAdvancePidCounter(int pid_hint) {
    if (pid_hint <= 0) {
        return;
    }
    while (true) {
        int current = gNextSyntheticPid;
        if (pid_hint < current) {
            break;
        }
        if (__sync_bool_compare_and_swap(&gNextSyntheticPid, current, pid_hint + 1)) {
            break;
        }
    }
}

static void vprocTaskTableRepairLocked(void) {
    if (gVProcTasks.items != gVProcTasksItemsStable) {
        if (vprocVprocDebugEnabled()) {
            vprocDebugLogf( "[vproc] task table pointer mismatch; repairing\n");
        }
        gVProcTasks.items = gVProcTasksItemsStable;
        gVProcTasks.capacity = gVProcTasksCapacityStable;
    }
    if (!gVProcTasks.items || gVProcTasks.capacity == 0) {
        gVProcTasks.items = NULL;
        gVProcTasks.count = 0;
        gVProcTasks.capacity = 0;
        gVProcTaskFindHint = 0;
        gVProcTaskFreeHint = 0;
    } else if (gVProcTasks.count > gVProcTasks.capacity) {
        gVProcTasks.count = gVProcTasks.capacity;
    } else if (gVProcTasks.count == 0) {
        gVProcTaskFindHint = 0;
        gVProcTaskFreeHint = 0;
    }
}

static size_t vprocFindFreeTaskSlotLocked(void) {
    if (gVProcTasks.count == 0) {
        return SIZE_MAX;
    }
    size_t start = (gVProcTaskFreeHint < gVProcTasks.count) ? gVProcTaskFreeHint : 0;
    for (size_t idx = start; idx < gVProcTasks.count; ++idx) {
        if (gVProcTasks.items[idx].pid <= 0) {
            gVProcTaskFreeHint = (idx + 1 < gVProcTasks.count) ? (idx + 1) : 0;
            return idx;
        }
    }
    for (size_t idx = 0; idx < start; ++idx) {
        if (gVProcTasks.items[idx].pid <= 0) {
            gVProcTaskFreeHint = (idx + 1 < gVProcTasks.count) ? (idx + 1) : 0;
            return idx;
        }
    }
    return SIZE_MAX;
}

static VProcTaskEntry *vprocTaskFindLocked(int pid) {
    if (pid <= 0) {
        return NULL;
    }
    vprocTaskTableRepairLocked();
    if (gVProcTasks.count > 0 && gVProcTaskFindHint < gVProcTasks.count) {
        VProcTaskEntry *hint = &gVProcTasks.items[gVProcTaskFindHint];
        if (hint->pid == pid) {
            return hint;
        }
    }
    VProcTaskLookupCacheSet *set = &gVProcTaskLookupCache[vprocTaskLookupSlotForPid(pid)];
    for (size_t way = 0; way < VPROC_TASK_LOOKUP_CACHE_WAYS; ++way) {
        VProcTaskLookupCacheEntry *cache = &set->way[way];
        if (cache->pid != pid) {
            continue;
        }
        size_t idx = (size_t)cache->idx;
        if (idx < gVProcTasks.count && gVProcTasks.items[idx].pid == pid) {
            gVProcTaskFindHint = idx;
            if (way > 0) {
                VProcTaskLookupCacheEntry tmp = set->way[0];
                set->way[0] = set->way[way];
                set->way[way] = tmp;
            }
            return &gVProcTasks.items[idx];
        }
        cache->pid = 0;
        cache->idx = 0;
    }
    size_t start = (gVProcTaskFindHint < gVProcTasks.count) ? gVProcTaskFindHint : 0;
    for (size_t i = start; i < gVProcTasks.count; ++i) {
        if (gVProcTasks.items[i].pid == pid) {
            gVProcTaskFindHint = i;
            vprocTaskLookupRememberLocked(pid, i);
            return &gVProcTasks.items[i];
        }
    }
    for (size_t i = 0; i < start; ++i) {
        if (gVProcTasks.items[i].pid == pid) {
            gVProcTaskFindHint = i;
            vprocTaskLookupRememberLocked(pid, i);
            return &gVProcTasks.items[i];
        }
    }
    return NULL;
}

static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        return entry;
    }
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid == pid) {
        parent_pid = 0;
    }
    const VProcTaskEntry *parent_entry = vprocTaskFindLocked(parent_pid);
    /* Preallocate generously and avoid realloc to keep table pointer stable. */
    if (gVProcTasks.capacity == 0) {
        gVProcTasks.capacity = 4096;
        gVProcTasks.items = (VProcTaskEntry *)calloc(gVProcTasks.capacity, sizeof(VProcTaskEntry));
        if (!gVProcTasks.items) {
            gVProcTasks.capacity = 0;
            return NULL;
        }
        gVProcTasksItemsStable = gVProcTasks.items;
        gVProcTasksCapacityStable = gVProcTasks.capacity;
    }
    size_t free_idx = vprocFindFreeTaskSlotLocked();
    if (free_idx != SIZE_MAX) {
        entry = &gVProcTasks.items[free_idx];
    } else if (gVProcTasks.count < gVProcTasks.capacity) {
        entry = &gVProcTasks.items[gVProcTasks.count++];
    } else {
        errno = EMFILE;
        return NULL;
    }
    vprocInitEntryDefaultsLocked(entry, pid, parent_entry);
    entry->parent_pid = parent_pid;
    if (entry->parent_pid > 0) {
        VProcTaskEntry *parent = (VProcTaskEntry *)parent_entry;
        if (parent && parent->pid == entry->parent_pid) {
            if (!vprocAddChildLocked(parent, pid)) {
                entry->parent_pid = 0;
            }
        }
    }
    if (gVProcTasks.items && gVProcTasks.count > 0) {
        ptrdiff_t idx = entry - gVProcTasks.items;
        if (idx >= 0 && (size_t)idx < gVProcTasks.count) {
            vprocTaskLookupRememberLocked(pid, (size_t)idx);
        }
    }
    return entry;
}

VProcOptions vprocDefaultOptions(void) {
    VProcOptions opts;
    opts.stdin_fd = -1;
    opts.stdout_fd = -1;
    opts.stderr_fd = -1;
    opts.winsize_cols = 80;
    opts.winsize_rows = 24;
    opts.pid_hint = -1;
    opts.job_id = 0;
    return opts;
}

/* A slot in `to` becomes a copy of the slot in `from`, or closes if that one is closed. */
static void vprocCopyFdEntry(VProc *from, VProc *to, int fd) {
    struct pscal_fd *pscal = NULL;
    int host = -1;
    pthread_mutex_lock(&from->mu);
    if ((size_t)fd < from->capacity) {
        if (from->entries[fd].kind == VPROC_FD_PSCAL) {
            pscal = pscal_fd_retain(from->entries[fd].pscal_fd);
        } else if (from->entries[fd].kind == VPROC_FD_HOST) {
            host = vprocCloneFd(from->entries[fd].host_fd);
        }
    }
    pthread_mutex_unlock(&from->mu);
    (void)vprocClose(to, fd);
    if (!pscal && host < 0) {
        return;
    }
    pthread_mutex_lock(&to->mu);
    if ((size_t)fd < to->capacity || vprocEnsureFdCapacityLocked(to, (size_t)fd + 1)) {
        to->entries[fd].host_fd = host;
        to->entries[fd].pscal_fd = pscal;
        to->entries[fd].kind = pscal ? VPROC_FD_PSCAL : VPROC_FD_HOST;
        to->entries[fd].inherit = true;
    } else if (pscal) {
        pscal_fd_close(pscal);
    } else {
        vprocHostCloseRaw(host);
    }
    pthread_mutex_unlock(&to->mu);
}

/* What a redirection put above stdio, as `exec 3> f` or `cmd 3> f`, reaches what this vproc
 * starts, as across a fork. The slots it opens for its own use stay its own. */
static void vprocInheritFds(VProc *vp, VProc *from) {
    if (!from) {
        return;
    }
    int fds[32];
    size_t count = 0;
    pthread_mutex_lock(&from->mu);
    for (size_t fd = STDERR_FILENO + 1; fd < from->capacity && count < sizeof(fds) / sizeof(fds[0]); ++fd) {
        if (from->entries[fd].inherit && from->entries[fd].kind != VPROC_FD_NONE) {
            fds[count++] = (int)fd;
        }
    }
    pthread_mutex_unlock(&from->mu);
    for (size_t i = 0; i < count; ++i) {
        vprocCopyFdEntry(from, vp, fds[i]);
    }
}

void vprocMarkFdInherited(VProc *vp, int fd) {
    if (!vp || fd <= STDERR_FILENO) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity) {
        vp->entries[fd].inherit = true;
    }
    pthread_mutex_unlock(&vp->mu);
}

static VProc *vprocCreateFrom(const VProcOptions *opts, VProc *active) {
    VProcOptions local = opts ? *opts : vprocDefaultOptions();
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    bool vproc_dbg = vprocVprocDebugEnabled();
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    bool inherit_pscal_stdio = false;
    if (session_stdio &&
        session_stdio->stdin_pscal_fd &&
        session_stdio->stdout_pscal_fd &&
        session_stdio->stderr_pscal_fd &&
        local.stdin_fd == -1 &&
        local.stdout_fd == -1 &&
        local.stderr_fd == -1) {
        inherit_pscal_stdio = true;
        if (vprocVprocDebugEnabled()) {
            vprocDebugLogf( "[vproc] inherit pscal stdio from session\n");
        }
    } else if (vproc_dbg && session_stdio) {
        vprocDebugLogf(
                "[vproc] skip pscal stdio inherit stdin=%d stdout=%d stderr=%d opts=(%d,%d,%d)\n",
                session_stdio->stdin_pscal_fd ? 1 : 0,
                session_stdio->stdout_pscal_fd ? 1 : 0,
                session_stdio->stderr_pscal_fd ? 1 : 0,
                local.stdin_fd,
                local.stdout_fd,
                local.stderr_fd);
    }
#endif
    VProc *vp = calloc(1, sizeof(VProc));
    if (!vp) {
        return NULL;
    }
    vp->creator = pthread_self();
    
    // Initialize mutex before use
    if (pthread_mutex_init(&vp->mu, NULL) != 0) {
        free(vp);
        return NULL;
    }

    vp->capacity = VPROC_INITIAL_CAPACITY;
    vp->entries = calloc(vp->capacity, sizeof(VProcFdEntry));
    if (!vp->entries) {
        pthread_mutex_destroy(&vp->mu);
        free(vp);
        return NULL;
    }
    for (size_t i = 0; i < vp->capacity; ++i) {
        vp->entries[i].host_fd = -1;
        vp->entries[i].pscal_fd = NULL;
        vp->entries[i].kind = VPROC_FD_NONE;
    }
    vprocInitializeVirtualCwd(vp, active);
    vp->file_mask = active ? active->file_mask : vprocHostMaskGet();
    vprocCredInherit(vp, active);
    for (int resource = 0; resource < RLIM_NLIMITS; ++resource) {
        if (active) {
            vp->limits[resource] = active->limits[resource];
        } else if (getrlimit(resource, &vp->limits[resource]) != 0) {
            vp->limits[resource].rlim_cur = RLIM_INFINITY;
            vp->limits[resource].rlim_max = RLIM_INFINITY;
        }
    }
    /* A task's descriptors are slots, and an in-process pipe uses no host descriptor: the host's
     * few hundred would be too few. Linux starts a process at this. */
    if (!active && vp->limits[RLIMIT_NOFILE].rlim_cur < 1024 && vp->limits[RLIMIT_NOFILE].rlim_max >= 1024) {
        vp->limits[RLIMIT_NOFILE].rlim_cur = 1024;
    }
    vp->next_fd = 3;
    vp->winsize.cols = (local.winsize_cols > 0) ? local.winsize_cols : 80;
    vp->winsize.rows = (local.winsize_rows > 0) ? local.winsize_rows : 24;

    if (local.pid_hint > 0) {
        vprocMaybeAdvancePidCounter(local.pid_hint);
        vp->pid = local.pid_hint;
    } else {
        vp->pid = -1;
    }

    /* Ensure a task slot exists for synthetic pid bookkeeping. */
    pthread_mutex_lock(&gVProcTasks.mu);
    if (vp->pid <= 0) {
        int shell_pid = vprocGetShellSelfPid();
        int kernel_pid = vprocGetKernelPid();
        for (size_t attempts = 0; attempts < gVProcTasks.capacity + 1024; ++attempts) {
            int candidate = __sync_fetch_and_add(&gNextSyntheticPid, 1);
            if (candidate <= 0) {
                continue;
            }
            if (candidate == shell_pid || candidate == kernel_pid) {
                continue;
            }
            if (vprocTaskFindLocked(candidate)) {
                continue;
            }
            vp->pid = candidate;
            break;
        }
        if (vp->pid <= 0) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = EMFILE;
            pthread_mutex_destroy(&vp->mu);
            free(vp->entries);
            free(vp);
            return NULL;
        }
    }
    VProcTaskEntry *slot = vprocTaskEnsureSlotLocked(vp->pid);
    if (slot) {
        slot->cred_ruid = vp->ruid;
        slot->cred_euid = vp->euid;
        slot->cred_suid = vp->suid;
    }
    if (!slot) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (errno == 0) {
            errno = EMFILE;
        }
        pthread_mutex_destroy(&vp->mu);
        free(vp->entries);
        free(vp);
        return NULL;
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid == vp->pid) {
        parent_pid = 0;
    }
    VProcTaskEntry *parent_entry = NULL;
    if (parent_pid > 0) {
        parent_entry = vprocTaskFindLocked(parent_pid);
    }
    /* Reinitialize the slot in place for this pid. */
    vprocClearEntryLocked(slot);
    vprocInitEntryDefaultsLocked(slot, vp->pid, parent_entry);
    slot->parent_pid = parent_pid;
    if (parent_entry && parent_pid > 0) {
        if (!vprocAddChildLocked(parent_entry, vp->pid)) {
            slot->parent_pid = 0;
        }
    }
    if (local.job_id > 0) {
        slot->job_id = local.job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);

    int stdin_src = vprocSelectHostFd(active, local.stdin_fd, STDIN_FILENO);
    if (stdin_src < 0 && local.stdin_fd != -2) {
        stdin_src = vprocHostOpenRawInternal("/dev/null", O_RDONLY, 0, false);
        if (vproc_dbg && stdin_src < 0) {
            vprocDebugLogf( "[vproc] stdin clone failed fd=%d err=%s\n",
                    (local.stdin_fd >= 0) ? local.stdin_fd : STDIN_FILENO, strerror(errno));
        }
    }
    bool stdin_from_session = false;
#if defined(PSCAL_TARGET_IOS)
    if (stdin_src >= 0) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && session->stdin_host_fd >= 0) {
            struct stat session_st;
            struct stat stdin_st;
            if (vprocHostFstatRaw(session->stdin_host_fd, &session_st) == 0 &&
                vprocHostFstatRaw(stdin_src, &stdin_st) == 0 &&
                session_st.st_dev == stdin_st.st_dev &&
                session_st.st_ino == stdin_st.st_ino) {
                stdin_from_session = true;
            }
        }
    }
#endif
    int stdout_src = vprocSelectHostFd(active, local.stdout_fd, STDOUT_FILENO);
    if (stdout_src < 0) {
        stdout_src = vprocHostOpenRawInternal("/dev/null", O_WRONLY, 0, false);
        if (vproc_dbg && stdout_src < 0) {
            vprocDebugLogf( "[vproc] stdout clone failed fd=%d err=%s\n",
                    (local.stdout_fd >= 0) ? local.stdout_fd : STDOUT_FILENO, strerror(errno));
        }
    }
    int stderr_src = vprocSelectHostFd(active, local.stderr_fd, STDERR_FILENO);
    if (stderr_src < 0) {
        stderr_src = vprocHostOpenRawInternal("/dev/null", O_WRONLY, 0, false);
        if (vproc_dbg && stderr_src < 0) {
            vprocDebugLogf( "[vproc] stderr clone failed fd=%d err=%s\n",
                    (local.stderr_fd >= 0) ? local.stderr_fd : STDERR_FILENO, strerror(errno));
        }
    }

    if (stdin_src < 0 || stdout_src < 0 || stderr_src < 0) {
        if (stdin_src >= 0) vprocHostClose(stdin_src);
        if (stdout_src >= 0) vprocHostClose(stdout_src);
        if (stderr_src >= 0) vprocHostClose(stderr_src);
        if (vproc_dbg) {
            vprocDebugLogf( "[vproc] create failed stdin=%d stdout=%d stderr=%d\n",
                    stdin_src, stdout_src, stderr_src);
        }
        vprocDiscard(vp->pid);
        vprocDestroy(vp);
        return NULL;
    }
    vp->entries[0].host_fd = stdin_src;
    vp->entries[0].pscal_fd = NULL;
    vp->entries[0].kind = VPROC_FD_HOST;
    vp->entries[1].host_fd = stdout_src;
    vp->entries[1].pscal_fd = NULL;
    vp->entries[1].kind = VPROC_FD_HOST;
    vp->entries[2].host_fd = stderr_src;
    vp->entries[2].pscal_fd = NULL;
    vp->entries[2].kind = VPROC_FD_HOST;
    vp->stdin_fd = 0;
    vp->stdout_fd = 1;
    vp->stderr_fd = 2;
    vp->stdin_host_fd = stdin_src;
    vp->stdout_host_fd = stdout_src;
    vp->stderr_host_fd = stderr_src;
    vp->stdin_from_session = stdin_from_session;
#if defined(PSCAL_TARGET_IOS)
    if (inherit_pscal_stdio) {
        if (vprocAdoptPscalStdio(vp,
                                 session_stdio->stdin_pscal_fd,
                                 session_stdio->stdout_pscal_fd,
                                 session_stdio->stderr_pscal_fd) != 0) {
            if (vproc_dbg) {
                vprocDebugLogf( "[vproc] adopt pscal stdio failed: %s\n", strerror(errno));
            }
        }
    }
#endif
    vprocInheritFds(vp, active);
    vprocRegistryAdd(vp);
    return vp;
}

VProc *vprocCreate(const VProcOptions *opts) {
    /* A cancel here would leave the pin held, and the parent's owner waiting on it for ever. */
    int cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    VProc *active = vprocPin(vprocCurrent());
    VProc *vp = vprocCreateFrom(opts, active);
    vprocUnpin(active);
    pthread_setcancelstate(cancel_state, NULL);
    return vp;
}

int vprocAdoptPscalStdio(VProc *vp,
                         struct pscal_fd *stdin_fd,
                         struct pscal_fd *stdout_fd,
                         struct pscal_fd *stderr_fd) {
    if (!vp || !stdin_fd || !stdout_fd || !stderr_fd) {
        errno = EINVAL;
        return -1;
    }
    (void)vprocClose(vp, STDIN_FILENO);
    (void)vprocClose(vp, STDOUT_FILENO);
    (void)vprocClose(vp, STDERR_FILENO);

    pthread_mutex_lock(&vp->mu);
    if (vp->capacity <= STDERR_FILENO) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    vp->entries[STDIN_FILENO].host_fd = -1;
    vp->entries[STDIN_FILENO].pscal_fd = pscal_fd_retain(stdin_fd);
    vp->entries[STDIN_FILENO].kind = VPROC_FD_PSCAL;

    vp->entries[STDOUT_FILENO].host_fd = -1;
    vp->entries[STDOUT_FILENO].pscal_fd = pscal_fd_retain(stdout_fd);
    vp->entries[STDOUT_FILENO].kind = VPROC_FD_PSCAL;

    vp->entries[STDERR_FILENO].host_fd = -1;
    vp->entries[STDERR_FILENO].pscal_fd = pscal_fd_retain(stderr_fd);
    vp->entries[STDERR_FILENO].kind = VPROC_FD_PSCAL;

    vp->stdin_host_fd = -1;
    vp->stdout_host_fd = -1;
    vp->stderr_host_fd = -1;
    vp->stdin_from_session = false;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocAdoptPscalFd(VProc *vp, int target_fd, struct pscal_fd *pscal_fd) {
    if (!vp || !pscal_fd || target_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    (void)vprocClose(vp, target_fd);

    pthread_mutex_lock(&vp->mu);
    if ((size_t)target_fd >= vp->capacity) {
        pthread_mutex_unlock(&vp->mu);
        if (vprocPipeDebugEnabled()) {
            vprocDebugLogf("[vproc] adopt pscal fd=%d failed: capacity %zu\n", target_fd, vp->capacity);
        }
        errno = EBADF;
        return -1;
    }
    vp->entries[target_fd].host_fd = -1;
    vp->entries[target_fd].pscal_fd = pscal_fd_retain(pscal_fd);
    vp->entries[target_fd].kind = VPROC_FD_PSCAL;
    if (target_fd == STDIN_FILENO) {
        vp->stdin_host_fd = -1;
        vp->stdin_from_session = false;
    } else if (target_fd == STDOUT_FILENO) {
        vp->stdout_host_fd = -1;
    } else if (target_fd == STDERR_FILENO) {
        vp->stderr_host_fd = -1;
    }
    pthread_mutex_unlock(&vp->mu);
    if (vprocVprocDebugEnabled()) {
        fprintf(stderr, "[vproc] adopt pscal fd=%d ptr=%p rc=0\n",
                target_fd, (void *)pscal_fd);
    }
    return 0;
}

/* An entry closing its host fd takes any saved stdio number with it. Otherwise
 * vprocDestroy closes that number again, and by then it may be anything: another command's
 * descriptor, a later stage's stdout (`ls x 2>&1 | wc -l | cat` printed nothing, status 0),
 * or the process's poll wake pipe, whose wakeups then wrote NULs to whatever had the number
 * next. Any `dup2(x, 2)` during a command does this: Aether mutes stderr around its pre-pass,
 * Pascal captures it. */
static void vprocForgetSavedStdioLocked(VProc *vp, int host_fd) {
    if (host_fd < 0) {
        return;
    }
    if (host_fd == vp->stdin_host_fd) {
        vp->stdin_host_fd = -1;
        vp->stdin_from_session = false;
    }
    if (host_fd == vp->stdout_host_fd) {
        vp->stdout_host_fd = -1;
    }
    if (host_fd == vp->stderr_host_fd) {
        vp->stderr_host_fd = -1;
    }
}

int vprocReplaceStdio(VProc *vp, int target_fd, int host_src) {
    if (!vp || target_fd < STDIN_FILENO || target_fd > STDERR_FILENO || host_src < 0) {
        errno = EBADF;
        return -1;
    }
    int cloned = vprocCloneFd(host_src);
    if (cloned < 0) {
        return -1;
    }
    (void)vprocClose(vp, target_fd);

    pthread_mutex_lock(&vp->mu);
    vp->entries[target_fd].host_fd = cloned;
    vp->entries[target_fd].pscal_fd = NULL;
    vp->entries[target_fd].kind = VPROC_FD_HOST;
    if (target_fd == STDIN_FILENO) {
        vp->stdin_host_fd = cloned;
        vp->stdin_from_session = false;
    } else if (target_fd == STDOUT_FILENO) {
        vp->stdout_host_fd = cloned;
    } else {
        vp->stderr_host_fd = cloned;
    }
    pthread_mutex_unlock(&vp->mu);
    return target_fd;
}

static void vprocCloseDescriptorsLocked(VProc *vp) {
    /* Close only the vproc-owned fds; do not close the saved host stdio fds. */
    for (size_t i = 0; i < vp->capacity; ++i) {
        if (vp->entries[i].kind == VPROC_FD_PSCAL && vp->entries[i].pscal_fd) {
            pscal_fd_close(vp->entries[i].pscal_fd);
        } else if (vp->entries[i].kind == VPROC_FD_HOST &&
                   vp->entries[i].host_fd >= 0 &&
                   vp->entries[i].host_fd != vp->stdin_host_fd &&
                   vp->entries[i].host_fd != vp->stdout_host_fd &&
                   vp->entries[i].host_fd != vp->stderr_host_fd) {
            (void)vprocResourceRemoveLocked(vp, vp->entries[i].host_fd);
            vprocHostCloseRaw(vp->entries[i].host_fd);
        }
        vp->entries[i].host_fd = -1;
        vp->entries[i].pscal_fd = NULL;
        vp->entries[i].kind = VPROC_FD_NONE;
    }
    if (vp->stdin_host_fd >= 0) {
        (void)vprocResourceRemoveLocked(vp, vp->stdin_host_fd);
        vprocHostCloseRaw(vp->stdin_host_fd);
        vp->stdin_host_fd = -1;
    }
    if (vp->stdout_host_fd >= 0) {
        (void)vprocResourceRemoveLocked(vp, vp->stdout_host_fd);
        vprocHostCloseRaw(vp->stdout_host_fd);
        vp->stdout_host_fd = -1;
    }
    if (vp->stderr_host_fd >= 0) {
        (void)vprocResourceRemoveLocked(vp, vp->stderr_host_fd);
        vprocHostCloseRaw(vp->stderr_host_fd);
        vp->stderr_host_fd = -1;
    }
    vprocResourceCloseAllLocked(vp);
}

void vprocCloseDescriptors(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    vprocCloseDescriptorsLocked(vp);
    pthread_mutex_unlock(&vp->mu);
}

void vprocDestroy(VProc *vp) {
    if (!vp) {
        return;
    }
    vprocRegistryRemove(vp);
    vprocWaitForPins(vp);
    // Lock shouldn't strictly be needed here if refcount is 0, but good practice
    pthread_mutex_lock(&vp->mu);
    vprocCloseDescriptorsLocked(vp);
    free(vp->resources);
    vp->resources = NULL;
    vp->resource_capacity = 0;
    vp->resource_count = 0;
    free(vp->entries);
    vp->entries = NULL;
    if (gVProcCurrent == vp) {
        gVProcCurrent = NULL;
    }
    for (size_t i = 0; i < gVProcStackDepth; ++i) {
        VProc **slot = vprocStackSlot(i);
        if (*slot == vp) {
            *slot = NULL;
        }
    }
    pthread_mutex_unlock(&vp->mu);
    
    pthread_mutex_destroy(&vp->mu);
    free(vp);
}

static void vprocStackPush(VProc *vp) {
    size_t depth = gVProcStackDepth;
    if (gVProcStackLost > 0) {
        gVProcStackLost++;
        return;
    }
    if (depth >= VPROC_STACK_INLINE && depth - VPROC_STACK_INLINE >= gVProcStackSpillCapacity) {
        size_t capacity = gVProcStackSpillCapacity ? gVProcStackSpillCapacity * 2 : VPROC_STACK_INLINE;
        VProc **grown = (VProc **)realloc(gVProcStackSpill, capacity * sizeof(*grown));
        if (!grown) {
            gVProcStackLost++;
            return;
        }
        gVProcStackSpill = grown;
        gVProcStackSpillCapacity = capacity;
    }
    *vprocStackSlot(depth) = vp;
    gVProcStackDepth = depth + 1;
}

void vprocActivate(VProc *vp) {
    gVProcTlsReady = 1;
    if (gVProcCurrent && !vprocRegistryContainsValidated(gVProcCurrent)) {
        /* vprocRegistryContainsValidated already cleared thread-local state. */
    }
    if (gVProcCurrent) {
        vprocStackPush(gVProcCurrent);
    }
    gVProcCurrent = vp;
    gVProcRegistrySeenVersion = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
    gVProcInterposeReady = 1;
}

void vprocDeactivate(void) {
    if (gVProcStackLost > 0) {
        /* Memory ran out when this one began, so what it replaced is gone. The next one down
         * stands in for it: every level still ends once, and the thread keeps a vproc. */
        gVProcStackLost--;
        gVProcCurrent = *vprocStackSlot(gVProcStackDepth - 1);
        return;
    }
    if (gVProcStackDepth > 0) {
        VProc **top = vprocStackSlot(gVProcStackDepth - 1);
        gVProcCurrent = *top;
        *top = NULL;
        gVProcStackDepth--;
        if (gVProcStackDepth == VPROC_STACK_INLINE) {
            vprocStackFreeSpill();
        }
    } else {
        gVProcCurrent = NULL;
    }
}

VProc *vprocCurrent(void) {
    return vprocForThread();
}

VProc *vprocPinCurrent(void) {
    return vprocPin(vprocForThread());
}

void vprocDiscard(int pid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        vprocClearEntryLocked(entry);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocTerminateSession(int sid) {
    if (sid <= 0) {
        return;
    }
    pthread_t self = pthread_self();
    pthread_t *cancel = NULL;
    size_t cancel_count = 0;
    size_t cancel_cap = 0;
    size_t target_count = 0;
    size_t *target_indices = NULL;
    bool clear_direct = false;

    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) continue;
        if (entry->sid != sid) continue;
        target_count++;
    }
    if (target_count > 0) {
        if (target_count > SIZE_MAX / sizeof(size_t)) {
            clear_direct = true;
        } else {
            target_indices = (size_t *)calloc(target_count, sizeof(size_t));
            if (!target_indices) {
                clear_direct = true;
            }
        }
    }
    size_t target_index = 0;
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) continue;
        if (entry->sid != sid) continue;

        vprocMaybeStampRusageLocked(entry);
        entry->exit_signal = SIGKILL;
        entry->status = W_EXITCODE(128 + SIGKILL, 0);
        entry->exited = true;
        entry->zombie = false;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);

        if (entry->tid && !pthread_equal(entry->tid, self)) {
            vprocCancelListAdd(&cancel, &cancel_count, &cancel_cap, entry->tid);
        }
        for (size_t t = 0; t < entry->thread_count; ++t) {
            pthread_t tid = entry->threads[t];
            if (tid && !pthread_equal(tid, self)) {
                vprocCancelListAdd(&cancel, &cancel_count, &cancel_cap, tid);
            }
        }
        if (target_indices && target_index < target_count) {
            target_indices[target_index++] = i;
        }
    }
    if (clear_direct) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0 || entry->sid != sid) continue;
            vprocClearEntryLocked(entry);
        }
    } else {
        for (size_t i = 0; i < target_index; ++i) {
            size_t idx = target_indices[i];
            if (idx >= gVProcTasks.count) continue;
            VProcTaskEntry *entry = &gVProcTasks.items[idx];
            if (entry->pid <= 0 || entry->sid != sid) continue;
            vprocClearEntryLocked(entry);
        }
    }
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    for (size_t i = 0; i < cancel_count; ++i) {
        pthread_cancel(cancel[i]);
    }
    free(cancel);
    free(target_indices);
}

bool vprocTerminateSessionById(uint64_t session_id) {
    if (session_id == 0) {
        return false;
    }

    int sid = -1;
    int shell_pid = 0;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry) {
        shell_pid = entry->shell_pid;
        if (entry->pty_slave && entry->pty_slave->tty) {
            struct tty *tty = entry->pty_slave->tty;
            lock(&tty->lock);
            sid = (int)tty->session;
            unlock(&tty->lock);
        }
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);

    /* The session's shell says whose this is. A shell tab's tty->session can name
     * another tab's session, or none of them: its thread activates the terminal
     * before it has a task, and asks for the current session there. Going by that,
     * closing a second tab whose shell would not end took the first tab's shell. */
    if (shell_pid > 0) {
        sid = vprocGetSid(shell_pid);
    }
    if (sid <= 0) {
        return false;
    }

    vprocTerminateSession(sid);
    return true;
}

/* Runs on pthread_exit too: exit() in a tool thread is one. */
static void vprocThreadEnvCleanup(void *arg) {
    VProcEnv *env = (VProcEnv *)arg;
    if (env) {
        vprocEnvActivate(NULL);
        vprocEnvRelease(env);
    }
}

/* A kill or an exit() unwinds a thread past the code that ends the vprocs it made for itself:
 * command scopes, and the stages a shell runs on its own thread. They would stay in the task
 * table and keep their descriptors, so the reader of `sh -c 'sleep 30' | cat` would wait for
 * ever once that shell is killed in the sleep. A vproc made by another thread and activated
 * here has an owner that ends it, and the session's shell is ended by its session. */
void vprocEndAbandoned(VProc *keep) {
    int shell_pid = vprocGetShellSelfPid();
    while (gVProcCurrent != keep && (gVProcCurrent || gVProcStackDepth > 0)) {
        VProc *vp = gVProcCurrent;
        vprocDeactivate();
        if (vp && vprocRegistryHoldsOwn(vp) && vp->pid != shell_pid) {
            int pid = vp->pid;
            vprocCloseDescriptors(vp);
            vprocMarkExit(vp, 128 + SIGTERM);
            vprocDiscard(pid);
            vprocDestroy(vp);
        }
    }
}

static void vprocEndAllAbandoned(void *unused) {
    (void)unused;
    vprocEndAbandoned(NULL);
}

static void *vprocThreadTrampoline(void *arg) {
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)arg;

    PSCALRuntimeContext *prev_runtime_ctx = NULL;
    bool runtime_ctx_swapped = false;
#if defined(PSCAL_TARGET_IOS)
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx && ctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
        runtime_ctx_swapped = true;
    }
#else
    (void)prev_runtime_ctx;
    (void)runtime_ctx_swapped;
#endif

    if (ctx && ctx->detach) {
        pthread_detach(pthread_self());
    }

    if (ctx) {
        vprocSetShellSelfPid(ctx->shell_self_pid);
        vprocSetKernelPid(ctx->kernel_pid);
        frontendPushKind(ctx->frontend_kind);
        if (ctx->session_stdio) {
            vprocSessionStdioActivate(ctx->session_stdio);
        }
    }

    VProc *vp = ctx ? ctx->vp : NULL;
    int vp_pid = (ctx && ctx->vp_pid > 0) ? ctx->vp_pid : 0;
    bool vp_activated = false;
    /* A creator does not wait for this thread, so its vproc may be gone. Activating it would
     * mark the pointer as checked, and the next vprocCurrent would hand it out. The version
     * is read first: a vproc destroyed after the check is noticed at the next use. */
    unsigned long long registry_version = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
    if (vp && vprocRegistryHoldsPid(vp, vp_pid)) {
        vprocActivate(vp);
        gVProcRegistrySeenVersion = registry_version;
        vp_activated = true;
        vprocRegisterStartedThread(vp_pid, pthread_self());
    }

    /* A thread of a program reads the program's environment, as its creator does. */
    VProcEnv *thread_env = ctx ? ctx->env : NULL;
    if (thread_env) {
        vprocEnvActivate(thread_env);
    }
    void *res = NULL;
    /* Threads share the owning vproc; do not mark the process as exited on
     * thread teardown. A cancelled thread leaves its starter's list here too. */
    pthread_cleanup_push(vprocThreadLeavesStarter, (void *)(intptr_t)(vp_activated ? vp_pid : 0));
    pthread_cleanup_push(vprocThreadEnvCleanup, thread_env);
    pthread_cleanup_push(vprocEndAllAbandoned, NULL);
    if (ctx && ctx->start_routine) {
        res = ctx->start_routine(ctx->arg);
    }
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);

    if (vp_activated) {
        vprocDeactivate();
    }

    if (ctx && ctx->session_stdio) {
        vprocSessionStdioActivate(NULL);
    }

#if defined(PSCAL_TARGET_IOS)
    if (runtime_ctx_swapped && PSCALRuntimeSetCurrentRuntimeContext) {
        PSCALRuntimeSetCurrentRuntimeContext(prev_runtime_ctx);
    }
#endif

    vprocScratchTlsCleanupCurrentThread();
    free(ctx);
    return res;
}

int vprocPthreadCreateShim(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg) {
    if (vprocInterposeBypassActive() ||
        vprocThreadIsInterposeBypassed(pthread_self())) {
        int rc = vprocHostPthreadCreateRaw(thread, attr, start_routine, arg);
        if (rc != 0) {
            errno = rc;
        }
        return rc;
    }

    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)calloc(1, sizeof(VProcThreadStartCtx));
    if (!ctx) {
        errno = ENOMEM;
        return ENOMEM;
    }
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->vp = vprocCurrent();
    ctx->vp_pid = (ctx->vp && ctx->vp->pid > 0) ? ctx->vp->pid : 0;
    ctx->session_stdio = vprocSessionStdioCurrent();
    ctx->shell_self_pid = vprocGetShellSelfPid();
    ctx->kernel_pid = vprocGetKernelPid();
    ctx->frontend_kind = frontendGetKind();
    ctx->detach = false;
    ctx->env = vprocEnvRetain(vprocEnvCurrent());
#if defined(PSCAL_TARGET_IOS)
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        ctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
#endif
    if (attr) {
        int detach_state = 0;
        if (pthread_attr_getdetachstate(attr, &detach_state) == 0 &&
            detach_state == PTHREAD_CREATE_DETACHED) {
            ctx->detach = true;
        }
    }
    int rc = vprocHostPthreadCreateRaw(thread, attr, vprocThreadTrampoline, ctx);
    if (rc != 0) {
        vprocEnvRelease(ctx->env);
        free(ctx);
        errno = rc;
    }
    return rc;
}

int vprocIsolationEnter(VProcIsolationDomain domain) {
    if (domain < 0 || domain >= VPROC_ISOLATION_DOMAIN_COUNT) {
        errno = EINVAL;
        return -1;
    }
    VProcIsolationLock *lock = &gVProcIsolationLocks[domain];
    pthread_t self = pthread_self();

    pthread_mutex_lock(&lock->mu);
    while (lock->depth > 0 &&
           (!lock->owner_valid || !pthread_equal(lock->owner, self))) {
        vprocCondWait(&lock->cv, &lock->mu);
    }
    if (lock->depth == 0) {
        lock->owner = self;
        lock->owner_valid = true;
    }
    lock->depth++;
    pthread_mutex_unlock(&lock->mu);
    return 0;
}

int vprocIsolationTryEnter(VProcIsolationDomain domain) {
    if (domain < 0 || domain >= VPROC_ISOLATION_DOMAIN_COUNT) {
        errno = EINVAL;
        return -1;
    }
    VProcIsolationLock *lock = &gVProcIsolationLocks[domain];
    pthread_t self = pthread_self();

    pthread_mutex_lock(&lock->mu);
    if (lock->depth > 0 &&
        (!lock->owner_valid || !pthread_equal(lock->owner, self))) {
        pthread_mutex_unlock(&lock->mu);
        errno = EBUSY;
        return -1;
    }
    if (lock->depth == 0) {
        lock->owner = self;
        lock->owner_valid = true;
    }
    lock->depth++;
    pthread_mutex_unlock(&lock->mu);
    return 0;
}

void vprocIsolationLeave(VProcIsolationDomain domain) {
    if (domain < 0 || domain >= VPROC_ISOLATION_DOMAIN_COUNT) {
        return;
    }
    VProcIsolationLock *lock = &gVProcIsolationLocks[domain];
    pthread_t self = pthread_self();

    pthread_mutex_lock(&lock->mu);
    if (lock->depth == 0 ||
        !lock->owner_valid ||
        !pthread_equal(lock->owner, self)) {
        pthread_mutex_unlock(&lock->mu);
        return;
    }
    lock->depth--;
    if (lock->depth == 0) {
        if (vprocToolDebugEnabled()) {
            vprocDebugLogf("[vproc-isolation] release domain=%s\n",
                           lock->name ? lock->name : "unknown");
        }
        lock->owner = (pthread_t)0;
        lock->owner_valid = false;
        pthread_cond_broadcast(&lock->cv);
    }
    pthread_mutex_unlock(&lock->mu);
}

/* A stdio stream's descriptor is a host fd, not a slot in the vproc table, so a
 * shim given fileno(fp) as it stands answers EBADF, or worse, uses whichever
 * slot has that number. vprocFilenoShim marks the fd so the shims use it as a
 * host fd, and so does vprocTagHostFd for a socket and the like. */
#define VPROC_HOST_FD_TAG 0x40000000

static inline bool vprocFdIsTaggedHost(int fd) {
    return fd >= 0 && (fd & VPROC_HOST_FD_TAG) != 0;
}

int vprocTagHostFd(int fd) {
    /* With no vproc the shims pass every fd straight to the host. */
    if (fd < 0 || !vprocForThread()) {
        return fd;
    }
    return fd | VPROC_HOST_FD_TAG;
}

int vprocTranslateFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (vprocFdIsTaggedHost(fd)) {
        return fd & ~VPROC_HOST_FD_TAG;
    }
    if (!vprocRegistryContainsValidated(vp)) {
        errno = EBADF;
        return -1;
    }
    int host = -1;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity && vp->entries[fd].kind == VPROC_FD_HOST) {
        host = vp->entries[fd].host_fd;
    }
    pthread_mutex_unlock(&vp->mu);

    if (host < 0) {
        errno = EBADF;
        return -1;
    }
    return host;
}

int vprocHostDup2(int host_fd, int target_fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostDup2Raw(host_fd, target_fd);
#else
    return dup2(host_fd, target_fd);
#endif
}

int vprocHostDup(int fd) {
#if defined(PSCAL_TARGET_IOS)
    int duped = vprocHostDupRaw(fd);
    VProc *vp = vprocForThread();
    if (vp && duped >= 0) {
        vprocResourceTrack(vp, duped, VPROC_RESOURCE_GENERIC);
    }
    return duped;
#else
    return dup(fd);
#endif
}

static int vprocReopenHostFd(int host, int flags);
static int vprocCopyFdOfTask(VProc *vp, int pid, int fd);

/* A slot with no host side: a terminal or an in-process pipe. */
static bool vprocSlotIsPscal(VProc *vp, int fd) {
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        return true;
    }
    return false;
}

int vprocHostOpen(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
#if defined(PSCAL_TARGET_IOS)
    if (vprocPathIsLocationDevice(path)) {
        int fd = vprocLocationDeviceOpenHost(flags);
        VProc *vp = vprocForThread();
        if (vp && fd >= 0) {
            vprocResourceTrack(vp, fd, VPROC_RESOURCE_GENERIC);
        }
        return fd;
    }
    /* The virtualized open answers these with a slot, and this returns a host descriptor. */
    int named_pid = 0;
    int named_fd = vprocPathNamedFdOf(path, &named_pid);
    if (named_fd >= 0) {
        VProc *named_vp = vprocForThread();
        int copy = -1;
        if (named_pid != vprocPid(named_vp)) {
            copy = vprocCopyFdOfTask(named_vp, named_pid, named_fd);
            if (copy < 0) {
                return -1;
            }
            named_fd = copy;
        }
        int fd = -1;
        int named_host = vprocTranslateFd(named_vp, named_fd);
        if (named_host < 0) {
            errno = vprocSlotIsPscal(named_vp, named_fd) ? ENXIO : ENOENT;
        } else {
            fd = vprocReopenHostFd(named_host, flags);
            if (fd >= 0) {
                vprocResourceTrack(named_vp, fd, VPROC_RESOURCE_GENERIC);
            }
        }
        if (copy >= 0) {
            int err = errno;
            vprocCloseShim(copy);
            errno = err;
        }
        return fd;
    }
    if (vprocPathRequiresRawHostOpen(path)) {
        bool has_mode = (flags & O_CREAT) != 0;
        int fd = vprocHostOpenRawInternal(path, flags, (mode_t)mode, has_mode);
        VProc *vp = vprocForThread();
        if (vp && fd >= 0) {
            vprocResourceTrack(vp, fd, VPROC_RESOURCE_GENERIC);
        }
        return fd;
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    int fd = vprocHostOpenVirtualized(path, flags, mode);
    VProc *vp = vprocForThread();
    if (vp && fd >= 0) {
        vprocResourceTrack(vp, fd, VPROC_RESOURCE_GENERIC);
    }
    return fd;
#else
    return open(path, flags, mode);
#endif
}

/* A pipe that outlives every vproc. vprocHostPipe makes the current vproc its owner,
 * and destroying that vproc closes it. */
int vprocHostPipeUnowned(int pipefd[2]) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostPipeRaw(pipefd);
#else
    return pipe(pipefd);
#endif
}

int vprocHostPipe(int pipefd[2]) {
#if defined(PSCAL_TARGET_IOS)
    int rc = vprocHostPipeRaw(pipefd);
    VProc *vp = vprocForThread();
    if (rc == 0 && vp) {
        vprocResourceTrack(vp, pipefd[0], VPROC_RESOURCE_PIPE);
        vprocResourceTrack(vp, pipefd[1], VPROC_RESOURCE_PIPE);
    }
    return rc;
#else
    return pipe(pipefd);
#endif
}

int vprocHostSocket(int domain, int type, int protocol) {
#if defined(PSCAL_TARGET_IOS)
    int fd = vprocHostSocketRaw(domain, type, protocol);
    VProc *vp = vprocForThread();
    if (vp && fd >= 0) {
        vprocResourceTrack(vp, fd, VPROC_RESOURCE_SOCKET);
    }
    return fd;
#else
    return socket(domain, type, protocol);
#endif
}

int vprocHostAccept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
#if defined(PSCAL_TARGET_IOS)
    int res = vprocHostAcceptRaw(fd, addr, addrlen);
    VProc *vp = vprocForThread();
    if (vp && res >= 0) {
        vprocResourceTrack(vp, res, VPROC_RESOURCE_SOCKET);
    }
    return res;
#else
    return accept(fd, addr, addrlen);
#endif
}

int vprocHostSocketpair(int domain, int type, int protocol, int sv[2]) {
#if defined(PSCAL_TARGET_IOS)
    int rc = vprocHostSocketpairRaw(domain, type, protocol, sv);
    VProc *vp = vprocForThread();
    if (rc == 0 && vp) {
        vprocResourceTrack(vp, sv[0], VPROC_RESOURCE_PIPE);
        vprocResourceTrack(vp, sv[1], VPROC_RESOURCE_PIPE);
    }
    return rc;
#else
    return socketpair(domain, type, protocol, sv);
#endif
}

off_t vprocHostLseek(int fd, off_t offset, int whence) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostLseekRaw(fd, offset, whence);
#else
    return lseek(fd, offset, whence);
#endif
}

int vprocHostFsync(int fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostFsyncRaw(fd);
#else
    return fsync(fd);
#endif
}

int vprocHostSigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    return vprocHostSigactionRaw(sig, act, oldact);
}

int vprocHostClose(int fd) {
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocForThread();
    if (vp) {
        vprocResourceRemove(vp, fd);
    }
    return vprocHostCloseRaw(fd);
#else
    return close(fd);
#endif
}

ssize_t vprocHostRead(int fd, void *buf, size_t count) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostReadRaw(fd, buf, count);
#else
    return read(fd, buf, count);
#endif
}

ssize_t vprocHostWrite(int fd, const void *buf, size_t count) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostWriteRaw(fd, buf, count);
#else
    return write(fd, buf, count);
#endif
}

int vprocHostPoll(struct pollfd *fds, nfds_t nfds, int timeout) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostPollRaw(fds, nfds, timeout);
#else
    return poll(fds, nfds, timeout);
#endif
}

int vprocHostPthreadCreate(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostPthreadCreateRaw(thread, attr, start_routine, arg);
#else
    return pthread_create(thread, attr, start_routine, arg);
#endif
}

int vprocDup(VProc *vp, int fd) {
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        int slot = vprocInsertPscalFd(vp, pscal_fd);
        /* vprocInsertPscalFd now retains; drop our temporary ref. */
        pscal_fd_close(pscal_fd);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        return -1;
    }
    return vprocInsert(vp, cloned);
}

/* As vprocDup, at `min` or above. The shell's own saved copies stay clear of the low numbers
 * a script can name, as bash's do: `2>/dev/null >&3` must not find a saved stderr in slot 3. */
int vprocDupAbove(VProc *vp, int fd, int min) {
    struct pscal_fd *pscal = vprocGetPscalFd(vp, fd);
    int host = -1;
    if (!pscal) {
        host = vprocTranslateFd(vp, fd);
        if (host < 0) {
            return -1;
        }
        host = vprocCloneFd(host);
        if (host < 0) {
            return -1;
        }
    }
    pthread_mutex_lock(&vp->mu);
    int slot = min;
    while ((size_t)slot < vp->capacity && vp->entries[slot].kind != VPROC_FD_NONE) {
        ++slot;
    }
    if ((size_t)slot >= vp->capacity && !vprocEnsureFdCapacityLocked(vp, (size_t)slot + 1)) {
        pthread_mutex_unlock(&vp->mu);
        if (pscal) {
            pscal_fd_close(pscal);
        } else {
            vprocHostCloseRaw(host);
        }
        return -1;
    }
    vp->entries[slot].host_fd = host;
    vp->entries[slot].pscal_fd = pscal;
    vp->entries[slot].kind = pscal ? VPROC_FD_PSCAL : VPROC_FD_HOST;
    vp->entries[slot].inherit = false;
    if (!pscal) {
        vprocResourceTrackLocked(vp, host, VPROC_RESOURCE_GENERIC);
    }
    pthread_mutex_unlock(&vp->mu);
    return slot;
}

/* The lowest slot from min up with nothing in it. */
int vprocFreeFdAbove(VProc *vp, int min) {
    pthread_mutex_lock(&vp->mu);
    int slot = min;
    while ((size_t)slot < vp->capacity && vp->entries[slot].kind != VPROC_FD_NONE) {
        ++slot;
    }
    pthread_mutex_unlock(&vp->mu);
    return slot;
}

#if defined(PSCAL_TARGET_IOS)
/* stdin_host_fd follows stdin here, so the descriptor it named has no other owner once it
 * moves. Left open, `cat < f` kept 2 for each run, and a loop of them ran out after about 116
 * runs at a limit of 256. Callers clone first, so their source may be that descriptor. */
static void vprocMoveSavedStdinLocked(VProc *vp, int cloned) {
    if (vp->stdin_host_fd >= 0 && vp->stdin_host_fd != cloned) {
        (void)vprocResourceRemoveLocked(vp, vp->stdin_host_fd);
        vprocHostCloseRaw(vp->stdin_host_fd);
    }
    vp->stdin_host_fd = cloned;
    vp->stdin_from_session = false;
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && session->stdin_host_fd >= 0) {
        struct stat session_st;
        struct stat cloned_st;
        if (vprocHostFstatRaw(session->stdin_host_fd, &session_st) == 0 &&
            vprocHostFstatRaw(cloned, &cloned_st) == 0 &&
            session_st.st_dev == cloned_st.st_dev &&
            session_st.st_ino == cloned_st.st_ino) {
            vp->stdin_from_session = true;
        }
    }
}
#endif

int vprocDup2(VProc *vp, int fd, int target) {
    if (!vp || target < 0 || (rlim_t)target >= vp->limits[RLIMIT_NOFILE].rlim_cur) {
        errno = EBADF;
        return -1;
    }
    if (fd == target) {
        struct pscal_fd *same_pscal = vprocGetPscalFd(vp, fd);
        if (same_pscal) {
            pscal_fd_close(same_pscal);
            return target;
        }
        if (vprocTranslateFd(vp, fd) >= 0) {
            return target;
        }
        errno = EBADF;
        return -1;
    }

    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        pthread_mutex_lock(&vp->mu);
        if ((size_t)target >= vp->capacity) {
            if (!vprocEnsureFdCapacityLocked(vp, (size_t)target + 1)) {
                pthread_mutex_unlock(&vp->mu);
                pscal_fd_close(pscal_fd);
                return -1;
            }
        }
        if (vp->entries[target].kind == VPROC_FD_PSCAL && vp->entries[target].pscal_fd) {
            pscal_fd_close(vp->entries[target].pscal_fd);
        } else if (vp->entries[target].kind == VPROC_FD_HOST && vp->entries[target].host_fd >= 0) {
            vprocResourceRemoveLocked(vp, vp->entries[target].host_fd);
            vprocForgetSavedStdioLocked(vp, vp->entries[target].host_fd);
            vprocHostCloseRaw(vp->entries[target].host_fd);
        }
        vp->entries[target].host_fd = -1;
        vp->entries[target].pscal_fd = pscal_fd;
        vp->entries[target].kind = VPROC_FD_PSCAL;
        vp->entries[target].inherit = target > STDERR_FILENO;
        pthread_mutex_unlock(&vp->mu);
        return target;
    }

    // We cannot hold lock here during Clone, but we need it for Translate
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&vp->mu);
    if ((size_t)target >= vp->capacity) {
        if (!vprocEnsureFdCapacityLocked(vp, (size_t)target + 1)) {
            pthread_mutex_unlock(&vp->mu);
            return -1;
        }
    }
    if (vp->entries[target].kind == VPROC_FD_PSCAL && vp->entries[target].pscal_fd) {
        pscal_fd_close(vp->entries[target].pscal_fd);
        vp->entries[target].pscal_fd = NULL;
        vp->entries[target].kind = VPROC_FD_NONE;
    }
    if (vp->entries[target].kind == VPROC_FD_HOST && vp->entries[target].host_fd >= 0) {
        bool preserve_controlling_stdin =
            (target == STDIN_FILENO) && (vp->entries[target].host_fd == vp->stdin_host_fd);
        if (!preserve_controlling_stdin) {
            vprocResourceRemoveLocked(vp, vp->entries[target].host_fd);
            vprocForgetSavedStdioLocked(vp, vp->entries[target].host_fd);
            vprocHostCloseRaw(vp->entries[target].host_fd);
        }
        vp->entries[target].host_fd = -1;
    }
    // Clone calls fcntl on host, safe to do while holding lock as it doesn't re-enter vproc
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        pthread_mutex_unlock(&vp->mu);
        return -1;
    }
    vp->entries[target].host_fd = cloned;
    vp->entries[target].pscal_fd = NULL;
    vp->entries[target].kind = VPROC_FD_HOST;
    vp->entries[target].inherit = target > STDERR_FILENO;
#if defined(PSCAL_TARGET_IOS)
    if (target == STDIN_FILENO) {
        vprocMoveSavedStdinLocked(vp, cloned);
    }
#endif
    pthread_mutex_unlock(&vp->mu);
    return target;
}

/* Re-sync the vproc fd table to match a host fd already duplicated onto
 * target_fd at the OS level. This is used when shellRestoreExecRedirections
 * has performed a host dup2 and we need the vproc view to follow suit. */
int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src) {
    if (!vp || target_fd < 0 || host_src < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)target_fd >= vp->capacity) {
        if (!vprocEnsureFdCapacityLocked(vp, (size_t)target_fd + 1)) {
            pthread_mutex_unlock(&vp->mu);
            return -1;
        }
    }
    if (vp->entries[target_fd].kind == VPROC_FD_PSCAL && vp->entries[target_fd].pscal_fd) {
        pscal_fd_close(vp->entries[target_fd].pscal_fd);
        vp->entries[target_fd].pscal_fd = NULL;
        vp->entries[target_fd].kind = VPROC_FD_NONE;
    }
    if (vp->entries[target_fd].kind == VPROC_FD_HOST && vp->entries[target_fd].host_fd >= 0 &&
        !(target_fd == STDIN_FILENO && vp->entries[target_fd].host_fd == vp->stdin_host_fd)) {
        vprocResourceRemoveLocked(vp, vp->entries[target_fd].host_fd);
        vprocForgetSavedStdioLocked(vp, vp->entries[target_fd].host_fd);
        vprocHostCloseRaw(vp->entries[target_fd].host_fd);
    }
    int cloned = vprocCloneFd(host_src);
    if (cloned < 0) {
        pthread_mutex_unlock(&vp->mu);
        return -1;
    }
    vp->entries[target_fd].host_fd = cloned;
    vp->entries[target_fd].pscal_fd = NULL;
    vp->entries[target_fd].kind = VPROC_FD_HOST;
#if defined(PSCAL_TARGET_IOS)
    if (target_fd == STDIN_FILENO) {
        vprocMoveSavedStdinLocked(vp, cloned);
    }
#endif
    pthread_mutex_unlock(&vp->mu);
    return target_fd;
}

int vprocClose(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd >= vp->capacity) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    int host = vp->entries[fd].host_fd;
    struct pscal_fd *pscal_fd = vp->entries[fd].pscal_fd;
    VProcFdKind kind = vp->entries[fd].kind;
    if (kind == VPROC_FD_NONE) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    vp->entries[fd].host_fd = -1;
    vp->entries[fd].pscal_fd = NULL;
    vp->entries[fd].kind = VPROC_FD_NONE;
    vp->entries[fd].inherit = false;
    if (kind == VPROC_FD_HOST) {
        vprocForgetSavedStdioLocked(vp, host);
    }
    pthread_mutex_unlock(&vp->mu);
    if (kind == VPROC_FD_PSCAL && pscal_fd) {
        int rc = pscal_fd_close(pscal_fd);
        if (rc < 0) {
            return vprocSetCompatErrno(rc);
        }
        return 0;
    }
    if (host >= 0) {
        vprocResourceRemove(vp, host);
    }
    return vprocHostClose(host);
}

int vprocPipe(VProc *vp, int pipefd[2]) {
    if (!vp || !pipefd) {
        errno = EINVAL;
        return -1;
    }
    int raw[2];
    if (vprocHostPipeRaw(raw) != 0) {
        return -1;
    }
    // vprocInsert locks internally, so this is safe
    int left = vprocInsert(vp, raw[0]);
    int right = vprocInsert(vp, raw[1]);
    if (left < 0 || right < 0) {
        if (left >= 0) vprocClose(vp, left);
        else vprocHostClose(raw[0]);
        if (right >= 0) vprocClose(vp, right);
        else vprocHostClose(raw[1]);
        return -1;
    }
    pipefd[0] = left;
    pipefd[1] = right;
    return 0;
}

static int vprocOpenNamedFd(VProc *vp, int pid, int fd, int flags);

int vprocOpenAt(VProc *vp, const char *path, int flags, int mode) {
    if (!vp || !path) {
        errno = EINVAL;
        return -1;
    }
    if (vprocPathIsLegacyGpsDevice(path)) {
        errno = ENOENT;
        return -1;
    }
    if (vprocPathIsLocationDevice(path)) {
        return vprocLocationDeviceOpen(vp, flags);
    }
    int named_pid = 0;
    int named_fd = vprocPathNamedFdOf(path, &named_pid);
    if (named_fd >= 0) {
        return vprocOpenNamedFd(vp, named_pid, named_fd, flags);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    const char *effective = target ? target : path;
    bool dbg = vprocPipeDebugEnabled();
    int host_fd = vprocHostOpenVirtualized(effective, flags, mode);
#if defined(PSCAL_TARGET_IOS)
    if (host_fd < 0 && errno == ENOENT) {
        if (dbg) vprocDebugLogf( "[vproc-open] virtualized ENOENT for %s, fallback raw\n", effective);
        /* Fallback to raw open so pipelines can read plain files even when
         * virtualization rejects the path. This mirrors shell expectations
         * for cat/head pipelines on iOS. */
        host_fd = vprocHostOpenRawInternal(effective, flags, (mode_t)mode, (flags & O_CREAT) != 0);
    }
    if (dbg && host_fd >= 0) {
        vprocDebugLogf( "[vproc-open] opened %s -> fd=%d flags=0x%x\n", effective, host_fd, flags);
    }
#endif
    if (host_fd < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        vprocHostClose(host_fd);
    }
    return slot;
}

int vprocSetWinsize(VProc *vp, int cols, int rows) {
    if (!vp) {
        errno = EINVAL;
        return -1;
    }
    // Winsize changes are atomic enough to not strictly need mutex for this simple struct,
    // but better safe if we expanded struct later.
    pthread_mutex_lock(&vp->mu);
    if (cols > 0) vp->winsize.cols = cols;
    if (rows > 0) vp->winsize.rows = rows;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocGetWinsize(VProc *vp, VProcWinsize *out) {
    if (!vp || !out) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    *out = vp->winsize;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocPid(VProc *vp) {
    return vp ? vp->pid : -1;
}

static bool vprocTaskEntryHasThreadLocked(const VProcTaskEntry *entry, pthread_t tid) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->tid && pthread_equal(entry->tid, tid)) {
        return true;
    }
    for (size_t i = 0; i < entry->thread_count; ++i) {
        if (entry->threads[i] && pthread_equal(entry->threads[i], tid)) {
            return true;
        }
    }
    return false;
}

static bool vprocEnsureThreadCapacityLocked(VProcTaskEntry *entry, size_t needed) {
    if (!entry) {
        return false;
    }
    if (needed <= entry->thread_capacity) {
        return true;
    }
    size_t new_cap = entry->thread_capacity ? entry->thread_capacity : 4;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            return false;
        }
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(pthread_t)) {
        return false;
    }
    pthread_t *resized = realloc(entry->threads, new_cap * sizeof(pthread_t));
    if (!resized) {
        return false;
    }
    entry->threads = resized;
    entry->thread_capacity = new_cap;
    return true;
}

int vprocRegisterTidHint(int pid, pthread_t tid) {
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    bool vdbg = vprocVprocDebugEnabled();
    char thread_name[16];
    bool rename_thread = false;
    size_t thread_count = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        if (vdbg) {
            vprocDebugLogf( "[vproc] register tid hint failed pid=%d tid=%p\n", pid, (void *)tid);
        }
        return -1;
    }
    bool duplicate = vprocTaskEntryHasThreadLocked(entry, tid);
    if (!duplicate) {
        if (!vprocEnsureThreadCapacityLocked(entry, entry->thread_count + 1)) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = ENOMEM;
            return -1;
        }
        entry->threads[entry->thread_count++] = tid;
    }
    entry->tid = tid;
    rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
    thread_count = entry->thread_count;
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
    if (vdbg) {
        vprocDebugLogf( "[vproc] register tid hint pid=%d tid=%p thread_count=%zu\n",
                pid, (void *)tid, thread_count);
    }
    return pid;
}

int vprocRegisterThread(VProc *vp, pthread_t tid) {
    if (!vp || vp->pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    bool vdbg = vprocVprocDebugEnabled();
    char thread_name[16];
    bool rename_thread = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(vp->pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        if (vdbg) {
            vprocDebugLogf( "[vproc] register thread failed pid=%d tid=%p\n", vp->pid, (void *)tid);
        }
        return -1;
    }
    bool duplicate = vprocTaskEntryHasThreadLocked(entry, tid);
    if (!duplicate) {
        if (!vprocEnsureThreadCapacityLocked(entry, entry->thread_count + 1)) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = ENOMEM;
            return -1;
        }
        entry->threads[entry->thread_count++] = tid;
    }
    entry->tid = tid;
    rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
    if (vdbg) {
        vprocDebugLogf( "[vproc] register thread pid=%d tid=%p thread_count=%zu\n",
                vp->pid, (void *)tid, entry->thread_count);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
    return vp->pid;
}

/* Registers a thread by pid alone, for callers whose vproc may be gone. A task that has
 * ended stays ended: no thread is added to it, and no slot is made for a pid already reaped. */
static void vprocRegisterStartedThread(int pid, pthread_t tid) {
    if (pid <= 0) {
        return;
    }
    char thread_name[16];
    bool rename_thread = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && !entry->exited) {
        bool duplicate = vprocTaskEntryHasThreadLocked(entry, tid);
        if (duplicate || vprocEnsureThreadCapacityLocked(entry, entry->thread_count + 1)) {
            if (!duplicate) {
                entry->threads[entry->thread_count++] = tid;
            }
            /* A thread it starts doesn't take over. On the shell's task that hid the shell's own thread,
             * which a kill then cancelled by way of a stage running on it. */
            if (!entry->tid) {
                entry->tid = tid;
            }
            rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
}

static void vprocThreadLeavesStarter(void *arg) {
    vprocUnregisterThreadByPid((int)(intptr_t)arg, pthread_self());
}

static void vprocUnregisterThreadByPid(int pid, pthread_t tid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        bool cleared_primary = false;
        if (entry->tid && pthread_equal(entry->tid, tid)) {
            entry->tid = 0;
            cleared_primary = true;
        }
        for (size_t i = 0; i < entry->thread_count; ++i) {
            if (entry->threads[i] && pthread_equal(entry->threads[i], tid)) {
                entry->threads[i] = entry->threads[entry->thread_count - 1];
                entry->thread_count--;
                break;
            }
        }
        if (cleared_primary && entry->thread_count > 0) {
            /* Keep tid anchored to a live registered thread so snapshots/jobs
             * do not misclassify resumable stopped tasks as threadless. */
            entry->tid = entry->threads[entry->thread_count - 1];
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocUnregisterThread(VProc *vp, pthread_t tid) {
    if (!vp || vp->pid <= 0) {
        return;
    }
    vprocUnregisterThreadByPid(vp->pid, tid);
}

int vprocSpawnThread(VProc *vp, void *(*start_routine)(void *), void *arg, pthread_t *thread_out) {
    if (!vp || !start_routine) {
        errno = EINVAL;
        return EINVAL;
    }
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)calloc(1, sizeof(VProcThreadStartCtx));
    if (!ctx) {
        errno = ENOMEM;
        return ENOMEM;
    }
    int vp_pid = vp->pid;
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->vp = vp;
    ctx->vp_pid = vp_pid;
    ctx->session_stdio = vprocSessionStdioCurrent();
    ctx->shell_self_pid = vprocGetShellSelfPid();
    ctx->kernel_pid = vprocGetKernelPid();
    ctx->frontend_kind = frontendGetKind();
    ctx->detach = false;
#if defined(PSCAL_TARGET_IOS)
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        ctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
#endif
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_t *attrp = NULL;
    bool attr_ready = false;
    if (pthread_attr_init(&attr) == 0) {
        attr_ready = true;
        attrp = &attr;
        size_t stack_size = 8u * 1024u * 1024u;
        if (stack_size < (size_t)PTHREAD_STACK_MIN) {
            stack_size = (size_t)PTHREAD_STACK_MIN;
        }
        (void)pthread_attr_setstacksize(&attr, stack_size);
    }
    int rc = vprocHostPthreadCreateRaw(&thread, attrp, vprocThreadTrampoline, ctx);
    if (attr_ready) {
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        free(ctx);
        errno = rc;
        return rc;
    }
    /* A kill from here on cancels the thread, even one that has not run yet. It may also have
     * finished and destroyed vp already, so this goes by the pid. */
    vprocRegisterStartedThread(vp_pid, thread);
    if (thread_out) {
        *thread_out = thread;
    }
    return 0;
}

void vprocMarkExit(VProc *vp, int status) {
    if (!vp || vp->pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vp->pid);
    if (entry) {
        int pgid = entry->pgid;
        if (entry->exit_signal == 0) {
            entry->status = status;
        }
        vprocMaybeStampRusageLocked(entry);
        entry->exited = true;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        entry->zombie = true;
        entry->dying_since_ns = 0;
        entry->real_timer_active = false;
        entry->real_timer_deadline_ns = 0;
        entry->real_timer_interval_ns = 0;
        int adopt_parent = vprocAdoptiveParentPidLocked(entry);
        vprocOrphanChildrenLocked(vp->pid, adopt_parent);

        bool discard_zombie = entry->orphaned || vprocKernelReapsLocked(entry);
        if (entry->sid == entry->pid) {
            vprocKernelReapSessionLocked(entry->pid);
        }
        VProcTaskEntry *parent_entry = vprocTaskFindLocked(entry->parent_pid);
        if (parent_entry) {
            struct sigaction sa = vprocGetSigactionLocked(parent_entry, SIGCHLD);
            if (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT)) {
                discard_zombie = true;
            }
        }
        int kernel_pid = vprocGetKernelPid();
        if (!discard_zombie && kernel_pid > 0 && entry->parent_pid == kernel_pid &&
            entry->sid > 0 && entry->sid != entry->pid) {
            VProcTaskEntry *session_entry = vprocTaskFindLocked(entry->sid);
            if (session_entry) {
                struct sigaction sa = vprocGetSigactionLocked(session_entry, SIGCHLD);
                if (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT)) {
                    discard_zombie = true;
                }
            }
        }
        if (discard_zombie) {
            entry->zombie = false;
            vprocClearEntryLocked(entry);
        } else {
            vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);
        }
        vprocMaybeNotifyPgidEmptyLocked(pgid);
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocMarkGroupExit(int pid, int status) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        int pgid = entry->pgid;
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (peer->pid <= 0) continue;
            if (peer->pgid != pgid) continue;
            vprocMaybeStampRusageLocked(peer);
            peer->group_exit = true;
            peer->group_exit_code = status;
            peer->exited = true;
            peer->zombie = true;
            peer->real_timer_active = false;
            peer->real_timer_deadline_ns = 0;
            peer->real_timer_interval_ns = 0;
            vprocNotifyParentSigchldLocked(peer, VPROC_SIGCHLD_EVENT_EXIT);
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetParent(int pid, int parent_pid) {
    if (pid <= 0) {
        return;
    }
    bool dbg = vprocVprocDebugEnabled();
    pthread_mutex_lock(&gVProcTasks.mu);
    /* Keep explicit parent assignments intact. The parent task can be
     * registered slightly later than the child; forcing kernel_pid here
     * collapses lineage and makes children appear attached to the kernel. */
    if (parent_pid <= 0) {
        int kernel_pid = vprocGetKernelPid();
        if (kernel_pid > 0 && pid != kernel_pid) {
            parent_pid = kernel_pid;
        }
    }
    if (dbg) {
        VProcTaskEntry *entry = vprocTaskFindLocked(pid);
        if (entry) {
            vprocDebugLogf( "[vproc-parent] pid=%d old=%d new=%d\n",
                    pid, entry->parent_pid, parent_pid);
        } else {
            vprocDebugLogf( "[vproc-parent] pid=%d not found; new=%d\n", pid, parent_pid);
        }
    }
    vprocUpdateParentLocked(pid, parent_pid);
    VProcTaskEntry *adopted = vprocTaskFindLocked(pid);
    if (adopted) {
        adopted->orphaned = false;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocPassChildrenToParent(int pid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->parent_pid > 0) {
        vprocReparentChildrenLocked(pid, entry->parent_pid);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocSetPgid(int pid, int pgid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (pgid <= 0) {
        pgid = pid;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        if (entry->pgid == pgid) {
            rc = 0;
            goto out;
        }
        /* Session leaders cannot change process groups. */
        if (entry->session_leader && entry->pid == entry->sid && entry->pgid != pgid) {
            errno = EPERM;
            rc = -1;
            goto out;
        }
        /* Ensure pgid belongs to same session if it already exists. */
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (peer->pid <= 0 || peer->pgid != pgid) continue;
            if (peer->sid != entry->sid) {
                errno = EPERM;
                rc = -1;
                goto out;
            }
        }
        entry->pgid = pgid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
out:
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocSetSid(int pid, int sid) {
    if (pid <= 0 || sid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sid = sid;
        entry->pgid = sid;
        entry->session_leader = (pid == sid);
        entry->fg_pgid = sid;
        entry->blocked_signals = 0;
        entry->pending_signals = 0;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocSetStopUnsupported(int pid, bool stop_unsupported) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->stop_unsupported = stop_unsupported;
        if (stop_unsupported && !entry->cooperative_stop_wait && entry->stopped) {
            int stop_sig = entry->stop_signo;
            if (stop_sig <= 0 || stop_sig >= 32) {
                stop_sig = SIGTSTP;
            }
            entry->pending_signals |= vprocSigMask(stop_sig);
            entry->stopped = false;
            entry->continued = true;
            entry->stop_signo = 0;
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetCooperativeStopWait(int pid, bool enabled) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->cooperative_stop_wait = enabled;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

bool vprocGetStopUnsupported(int pid) {
    if (pid <= 0) {
        return false;
    }
    bool stop_unsupported = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        stop_unsupported = entry->stop_unsupported;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return stop_unsupported;
}

void vprocSetShellPromptReadActive(int pid, bool active) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->shell_prompt_read_active = active;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

bool vprocShellPromptReadActive(int pid) {
    if (pid <= 0) {
        return false;
    }
    bool active = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        active = entry->shell_prompt_read_active;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return active;
}

void vprocSetPipelineStage(bool active) {
    gVprocPipelineStage = active;
}

bool vprocTakeTerminalInterrupted(void) {
    bool interrupted = gVprocTerminalInterrupted;
    gVprocTerminalInterrupted = false;
    return interrupted;
}

int vprocGetPgid(int pid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
    int pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        pgid = entry->pgid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pgid;
}

int vprocGetSid(int pid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
    int sid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        sid = entry->sid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return sid;
}

bool vprocGetShellJobControlState(int *shell_pid_out,
                                  int *shell_pgid_out,
                                  int *sid_out,
                                  int *fg_pgid_out) {
    int shell_pid = vprocGetShellSelfPid();
    int shell_pgid = -1;
    int sid = -1;
    int fg_pgid = -1;
    if (shell_pid > 0) {
        pthread_mutex_lock(&gVProcTasks.mu);
        VProcTaskEntry *entry = vprocTaskFindLocked(shell_pid);
        if (entry) {
            shell_pgid = entry->pgid;
            sid = entry->sid;
        }
        if (sid > 0) {
            fg_pgid = vprocForegroundPgidLocked(sid);
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (fg_pgid <= 0) {
            fg_pgid = shell_pgid;
        }
    }
    if (shell_pid_out) {
        *shell_pid_out = shell_pid;
    }
    if (shell_pgid_out) {
        *shell_pgid_out = shell_pgid;
    }
    if (sid_out) {
        *sid_out = sid;
    }
    if (fg_pgid_out) {
        *fg_pgid_out = fg_pgid;
    }
    return shell_pid > 0;
}

static void vprocSyncForegroundPgidToSessionTty(int sid, int fg_pgid) {
#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)
    /* The kernel keeps its own books, by session, whoever is asking: this can be a thread with
     * no session of its own, and a program's tcsetpgrp is the one subject to checks. */
    if (sid <= 0 || fg_pgid <= 0) {
        return;
    }
    pscalTtySetSessionForeground((pid_t_)sid, (pid_t_)fg_pgid);
#else
    (void)sid;
    (void)fg_pgid;
#endif
}

static int vprocSetForegroundPgidInternal(int sid, int fg_pgid, bool sync_tty) {
    if (sid <= 0 || fg_pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    bool fg_changed = false;
    bool should_continue_fg = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    if (!leader) {
        leader = vprocTaskEnsureSlotLocked(sid);
        if (leader) {
            leader->sid = sid;
            leader->pid = sid;
            leader->session_leader = true;
        }
    }
    if (leader) {
        fg_changed = (leader->fg_pgid != fg_pgid);
        leader->fg_pgid = fg_pgid;
        if (fg_changed) {
            for (size_t i = 0; i < gVProcTasks.count; ++i) {
                VProcTaskEntry *entry = &gVProcTasks.items[i];
                if (entry->pid <= 0) {
                    continue;
                }
                if (entry->sid != sid || entry->pgid != fg_pgid) {
                    continue;
                }
                if (entry->stopped && !entry->stop_unsupported) {
                    should_continue_fg = true;
                    break;
                }
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rc == 0) {
        /* Before this returns: a job the shell has just put in the foreground may read at once,
         * and must not be taken for a background one. */
        if (sync_tty) {
            vprocSyncForegroundPgidToSessionTty(sid, fg_pgid);
        }
        if (sync_tty && should_continue_fg) {
            if (!vprocKernelQueueContinuePgid(fg_pgid)) {
                (void)vprocKillShim(-fg_pgid, SIGCONT);
            }
        }
    }
    return rc;
}

int vprocSetForegroundPgid(int sid, int fg_pgid) {
    return vprocSetForegroundPgidInternal(sid, fg_pgid, true);
}

int vprocGetForegroundPgid(int sid) {
    if (sid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int fg = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    if (leader) {
        fg = leader->fg_pgid;
    }
    if (fg < 0) errno = ESRCH;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return fg;
}

static void vprocClearEntryLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    int pid = entry->pid;
    int parent_pid = entry->parent_pid;
    int sid = entry->sid;
    if (parent_pid > 0 && pid > 0) {
        VProcTaskEntry *parent = vprocTaskFindLocked(parent_pid);
        if (parent) {
            vprocRemoveChildLocked(parent, pid);
        }
    }
    int adopt_parent = vprocAdoptiveParentPidLocked(entry);
    vprocOrphanChildrenLocked(pid, adopt_parent);

    if (sid > 0) {
        bool drop_session = true;
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (peer->pid <= 0) {
                continue;
            }
            if (peer->pid == pid) {
                continue;
            }
            if (peer->sid == sid) {
                drop_session = false;
                break;
            }
        }
        if (drop_session) {
            pscalTtyDropSession((pid_t_)sid);
        }
    }

    if (entry->label) {
        free(entry->label);
        entry->label = NULL;
    }
    vprocTaskLookupForgetLocked(pid);
    memset(entry->comm, 0, sizeof(entry->comm));
    entry->pid = 0;
    entry->tid = 0;
    entry->parent_pid = 0;
    entry->pgid = 0;
    entry->sid = 0;
    entry->status = 0;
    entry->exit_signal = 0;
    entry->exited = false;
    entry->stopped = false;
    entry->continued = false;
    entry->stop_signo = 0;
    entry->zombie = false;
    entry->stop_unsupported = false;
    entry->shell_prompt_read_active = false;
    entry->job_id = 0;
    if (entry->children) {
        free(entry->children);
    }
    if (entry->threads) {
        free(entry->threads);
    }
    entry->threads = NULL;
    entry->thread_capacity = 0;
    entry->thread_count = 0;
    entry->session_leader = false;
    entry->fg_pgid = 0;
    entry->sigchld_events = 0;
    entry->sigchld_blocked = false;
    entry->sigchld_delivery_queued = false;
    entry->children = NULL;
    entry->rusage_utime = 0;
    entry->rusage_stime = 0;
    entry->group_exit = false;
    entry->group_exit_code = 0;
    entry->blocked_signals = 0;
    entry->pending_signals = 0;
    entry->unwinds_itself = false;
    entry->halting = false;
    entry->shell_task = false;
    entry->silent_to_parent = false;
    entry->not_a_process = false;
    entry->orphaned = false;
    entry->caught_signals = 0;
    entry->ignored_signals = 0;
    for (int i = 0; i < 32; ++i) {
        entry->pending_counts[i] = 0;
    }
    entry->fg_override_pgid = 0;
    for (int i = 0; i < 32; ++i) {
        sigemptyset(&entry->actions[i].sa_mask);
        entry->actions[i].sa_handler = SIG_DFL;
        entry->actions[i].sa_flags = 0;
    }
    entry->real_timer_active = false;
    entry->real_timer_deadline_ns = 0;
    entry->real_timer_interval_ns = 0;
    entry->child_capacity = 0;
    entry->child_count = 0;
    entry->start_mono_ns = 0;
    if (gVProcTasks.items && gVProcTasks.count > 0) {
        ptrdiff_t idx = entry - gVProcTasks.items;
        if (idx >= 0 && (size_t)idx < gVProcTasks.count) {
            gVProcTaskFreeHint = (size_t)idx;
        }
    }

}

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) {
    size_t count = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) {
            continue;
        }
        if (out && count < capacity) {
            int fg_for_session = (entry->sid > 0) ? vprocForegroundPgidLocked(entry->sid) : -1;
            int tty_pty_num = -1;
#if defined(PSCAL_TARGET_IOS)
            if (entry->pid > 0) {
                tty_pty_num = vprocPtyNumForPid(entry->pid);
            }
#endif
            int utime = entry->rusage_utime;
            int stime = entry->rusage_stime;
            int cpu_utime = 0;
            int cpu_stime = 0;
            if (vprocComputeCpuTimesLocked(entry, &cpu_utime, &cpu_stime)) {
                if (cpu_utime > utime) {
                    utime = cpu_utime;
                }
                if (cpu_stime > stime) {
                    stime = cpu_stime;
                }
            }
            out[count].pid = entry->pid;
            out[count].tid = entry->tid;
            out[count].parent_pid = entry->parent_pid;
            out[count].pgid = entry->pgid;
            out[count].sid = entry->sid;
            out[count].tty_pty_num = tty_pty_num;
            out[count].exited = entry->exited;
            out[count].stopped = entry->stopped;
            out[count].continued = entry->continued;
            out[count].zombie = entry->zombie;
            out[count].exit_signal = entry->exit_signal;
            out[count].status = entry->status;
            out[count].stop_signo = entry->stop_signo;
            out[count].sigchld_pending = entry->sigchld_events > 0;
            out[count].rusage_utime = utime;
            out[count].rusage_stime = stime;
            out[count].fg_pgid = (fg_for_session > 0) ? fg_for_session : entry->fg_pgid;
            out[count].job_id = entry->job_id;
            strncpy(out[count].comm, entry->comm, sizeof(out[count].comm) - 1);
            out[count].comm[sizeof(out[count].comm) - 1] = '\0';
            if (entry->label) {
                strncpy(out[count].command, entry->label, sizeof(out[count].command) - 1);
                out[count].command[sizeof(out[count].command) - 1] = '\0';
            } else if (entry->comm[0]) {
                strncpy(out[count].command, entry->comm, sizeof(out[count].command) - 1);
                out[count].command[sizeof(out[count].command) - 1] = '\0';
            } else {
                out[count].command[0] = '\0';
            }
        }
        ++count;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    /* A caller sizes its buffer with one call and fills it with the next, and a task can start in
     * between. It loops over what this returns, so that must be what it holds. */
    return (out && count > capacity) ? capacity : count;
}

static inline bool vprocWaitParentMatchesLocked(const VProcTaskEntry *entry,
                                                int waiter_pid,
                                                int kernel_pid) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->parent_pid == waiter_pid) {
        return true;
    }
    return kernel_pid > 0 &&
           entry->parent_pid == kernel_pid &&
           entry->sid == waiter_pid;
}

static inline bool vprocWaitPidMatchesLocked(const VProcTaskEntry *entry,
                                             pid_t pid,
                                             int waiter_pgid) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (pid > 0) {
        return entry->pid == pid;
    }
    if (pid == -1) {
        return true;
    }
    if (pid == 0) {
        return (waiter_pgid > 0) ? (entry->pgid == waiter_pgid) : true;
    }
    return entry->pgid == -pid;
}

static inline bool vprocWaitStateChangeMatchesLocked(const VProcTaskEntry *entry,
                                                     bool allow_stop,
                                                     bool allow_cont) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->exited) {
        return true;
    }
    if (allow_stop && entry->stopped && entry->stop_signo > 0) {
        return true;
    }
    if (allow_cont && entry->continued) {
        return true;
    }
    return false;
}

static inline int vprocTaskPgidByPidLocked(int pid) {
    if (pid <= 0) {
        return -1;
    }
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    return entry ? entry->pgid : -1;
}

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
static bool vprocHasWaitCandidateLocked(pid_t pid,
                                        int waiter_pid,
                                        int waiter_pgid,
                                        int kernel_pid) {
    if (pid > 0) {
        VProcTaskEntry *entry = vprocTaskFindLocked((int)pid);
        if (!entry) {
            return false;
        }
        return vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid);
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid)) {
            continue;
        }
        if (vprocWaitPidMatchesLocked(entry, pid, waiter_pgid)) {
            return true;
        }
    }
    return false;
}
#endif

/* kill(-1) as on Linux: every task but the caller and init, which here is the kernel's own task. */
static bool vprocBroadcastReachesLocked(const VProcTaskEntry *entry, int self_pid) {
    return entry->pid > 1 && entry->pid != self_pid && entry->pid != vprocGetKernelPid();
}

static bool vprocHasKillTargetLocked(pid_t pid, int self_pid) {
    if (pid == 0) {
        return false;
    }
    bool broadcast_all = (pid == -1);
    bool target_group = (pid <= 0);
    int target = target_group ? -pid : pid;
    if (!broadcast_all && !target_group) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target);
        return entry != NULL;
    }
    if (target_group && !broadcast_all) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target);
        if (entry && entry->pgid == target) {
            return true;
        }
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) continue;
        if (broadcast_all) {
            if (vprocBroadcastReachesLocked(entry, self_pid)) {
                return true;
            }
            continue;
        }
        if (target_group) {
            if (entry->pgid == target) {
                return true;
            }
        } else if (entry->pid == target) {
            return true;
        }
    }
    return false;
}

/* A killed thread ends within moments, and vprocMarkExit says so. One stuck where a cancel cannot
 * reach it must not keep whoever waits for it for ever, so after a while it counts as gone.
 * Returns whether any killed task is still running. */
#define VPROC_KILLED_GRACE_NS (2ull * 1000000000ull)
static bool vprocEndOverdueKilledLocked(void) {
    bool running = false;
    uint64_t now = vprocNowMonoNs();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0 || entry->dying_since_ns == 0) {
            continue;
        }
        if (now - entry->dying_since_ns < VPROC_KILLED_GRACE_NS) {
            running = true;
            continue;
        }
        vprocMaybeStampRusageLocked(entry);
        entry->dying_since_ns = 0;
        entry->exited = true;
        entry->zombie = true;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);
    }
    return running;
}

pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options) {
    bool allow_stop = (options & WUNTRACED) != 0;
    bool allow_cont = (options & WCONTINUED) != 0;
    bool nohang = (options & WNOHANG) != 0;
    bool nowait = (options & WNOWAIT) != 0;
    bool dbg = vprocKillDebugEnabled();
    int waiter_pid = vprocWaiterPid();
    int waiter_pgid = -1;
    int kernel_pid = vprocGetKernelPid();

    if (!vprocShimHasVirtualContext()) {
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
        pthread_mutex_lock(&gVProcTasks.mu);
        if (pid == 0) {
            waiter_pgid = vprocTaskPgidByPidLocked(waiter_pid);
        }
        bool has_candidate = vprocHasWaitCandidateLocked(pid, waiter_pid, waiter_pgid, kernel_pid);
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (!has_candidate) {
            return vprocHostWaitpidRaw(pid, status_out, options);
        }
#else
        return vprocHostWaitpidRaw(pid, status_out, options);
#endif
    }

    pthread_mutex_lock(&gVProcTasks.mu);
    if (pid == 0) {
        waiter_pgid = vprocTaskPgidByPidLocked(waiter_pid);
    }
    while (true) {
        VProcTaskEntry *ready = NULL;
        bool has_candidate = false;
        bool killed_running = vprocEndOverdueKilledLocked();

        if (pid > 0) {
            VProcTaskEntry *entry = vprocTaskFindLocked((int)pid);
            if (entry && vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid)) {
                has_candidate = true;
                if (vprocWaitStateChangeMatchesLocked(entry, allow_stop, allow_cont)) {
                    ready = entry;
                }
            }
        } else {
            for (size_t i = 0; i < gVProcTasks.count; ++i) {
                VProcTaskEntry *entry = &gVProcTasks.items[i];
                if (!vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid)) continue;
                if (!vprocWaitPidMatchesLocked(entry, pid, waiter_pgid)) continue;
                has_candidate = true;

                if (vprocWaitStateChangeMatchesLocked(entry, allow_stop, allow_cont)) {
                    ready = entry;
                    break;
                }
            }
        }

        if (ready) {
            int status = 0;
            int waited_pid = ready->pid;
            VProcTaskEntry *waiter_entry = NULL;
            if (waiter_pid > 0) {
                waiter_entry = vprocTaskFindLocked(waiter_pid);
            }

            if (ready->exited) {
                if (ready->group_exit) {
                    status = W_EXITCODE(ready->group_exit_code & 0xff, 0);
                } else if (ready->exit_signal > 0) {
                    status = (ready->exit_signal & 0x7f);
                } else {
                    status = W_EXITCODE(ready->status & 0xff, 0);
                }
            } else if (ready->stopped && ready->stop_signo > 0) {
                status = W_STOPCODE(ready->stop_signo & 0xff);
            } else if (ready->continued) {
                status = W_STOPCODE(SIGCONT);
            }

            if (status_out) {
                *status_out = status;
            }

            if (ready->exited && !nowait) {
                vprocClearEntryLocked(ready);
            } else if (ready->exited) {
                ready->zombie = true;
            } else if (ready->stopped) {
                ready->stop_signo = 0; // Clear stop signal after reporting
            } else if (ready->continued) {
                ready->continued = false;
            }
            if (waiter_entry && waiter_entry->sigchld_events > 0 && !waiter_entry->sigchld_blocked &&
                vprocEffectiveSignalActionLocked(waiter_entry, SIGCHLD) != VPROC_SIG_HANDLER) {
                /* A handler still has to run: waiting for a child doesn't take its signal. */
                waiter_entry->sigchld_events--;
                if (waiter_entry->sigchld_events == 0) {
                    waiter_entry->pending_signals &= ~vprocSigMask(SIGCHLD);
                    if (vprocSigIndexValid(SIGCHLD)) {
                        waiter_entry->pending_counts[SIGCHLD] = 0;
                    }
                    waiter_entry->sigchld_delivery_queued = false;
                }
            }

            if (dbg) {
                vprocDebugLogf( "[vproc-wait] pid=%d status=%d exited=%d stop=%d\n",
                        waited_pid, status, ready->exited, ready->stopped);
            }
            pthread_mutex_unlock(&gVProcTasks.mu);
            return waited_pid;
        }

        if (!has_candidate) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = ECHILD;
            return -1;
        }

        if (nohang) {
            if (status_out) *status_out = 0;
            pthread_mutex_unlock(&gVProcTasks.mu);
            return 0;
        }

        if (killed_running) {
            struct timespec deadline;
            vprocInprocPipeWaitDeadline(&deadline);
            vprocCondTimedWait(&gVProcTasks.cv, &gVProcTasks.mu, &deadline);
        } else {
            vprocCondWait(&gVProcTasks.cv, &gVProcTasks.mu);
        }
    }
}

static void vprocCancelListAdd(pthread_t **list, size_t *count, size_t *capacity, pthread_t tid) {
    if (!list || !count || !capacity) {
        return;
    }
    if (tid == 0) {
        return;
    }
    for (size_t i = 0; i < *count; ++i) {
        if (pthread_equal((*list)[i], tid)) {
            return;
        }
    }
    if (*count >= *capacity) {
        size_t new_cap = 0;
        if (!vprocComputeGrowthCapacity(*capacity,
                                        *count + 1,
                                        8,
                                        sizeof(pthread_t),
                                        &new_cap)) {
            return;
        }
        pthread_t *resized = (pthread_t *)realloc(*list, new_cap * sizeof(pthread_t));
        if (!resized) {
            return;
        }
        *list = resized;
        *capacity = new_cap;
    }
    (*list)[(*count)++] = tid;
}

/* An inline stage runs on the thread of the shell that started it. Cancelling that thread would
 * end the shell, which has to unwind itself. Only its main thread: a stage's worker is listed on
 * the shell's task too, having started under it, and it can be cancelled. */
static bool vprocThreadUnwindsItselfLocked(pthread_t tid) {
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        const VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid > 0 && entry->unwinds_itself && !entry->exited && entry->tid &&
            pthread_equal(entry->tid, tid)) {
            return true;
        }
    }
    return false;
}

/* A command a shell runs on its own thread is not that thread: killing the command must leave the
 * shell above it to go on, as its process would under Linux. The command notices it was killed. A
 * shell the killed task runs, such as `sh` under `timeout`, is below it and ends with it. */
static bool vprocThreadRunsOtherShellLocked(pthread_t tid, const VProcTaskEntry *killed) {
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        const VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry == killed || entry->pid <= 0 || !entry->shell_task || entry->exited || !entry->tid ||
            !pthread_equal(entry->tid, tid)) {
            continue;
        }
        int parent = killed->parent_pid;
        for (int depth = 0; parent > 0 && depth < 64; ++depth) {
            if (parent == entry->pid) {
                return true;
            }
            const VProcTaskEntry *up = vprocTaskFindLocked(parent);
            if (!up || up == killed) {
                break;
            }
            parent = up->parent_pid;
        }
    }
    return false;
}

/* Pending, a stop and a continue cancel each other. A task that opted out of real stops
 * keeps its SIGTSTP: that is how it learns to unwind. */
static void vprocDropOpposingPendingLocked(VProcTaskEntry *entry, int sig) {
    if (sig == SIGCONT) {
        if (entry->stop_unsupported) {
            return;
        }
        const int stops[] = { SIGTSTP, SIGSTOP, SIGTTIN, SIGTTOU };
        for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); ++i) {
            entry->pending_signals &= ~vprocSigMask(stops[i]);
            entry->pending_counts[stops[i]] = 0;
        }
    } else if (sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU) {
        entry->pending_signals &= ~vprocSigMask(SIGCONT);
        entry->pending_counts[SIGCONT] = 0;
    }
}

static bool vprocKillDeliverEntryLocked(VProcTaskEntry *entry,
                                        pid_t requested_pid,
                                        int sig,
                                        bool dbg,
                                        pthread_t self,
                                        pthread_t **cancel_list,
                                        size_t *cancel_count,
                                        size_t *cancel_capacity,
                                        bool *request_runtime_sigint,
                                        bool *request_runtime_sigtstp,
                                        bool *die_here) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->zombie || entry->exited) {
        return false;
    }
    if (entry->dying_since_ns != 0) {
        return true;
    }
    if (entry->pid == vprocGetKernelPid()) {
        /* As init on Linux, nothing kills it. A cancel would end the thread that carries Ctrl-C. */
        return true;
    }
    if (dbg) {
        vprocDebugLogf( "[vproc-kill] pid=%d sig=%d entry_pid=%d tid=%p\n",
                (int)requested_pid, sig, entry->pid, (void *)entry->tid);
    }

    /* In-process workers often execute on the same pthread as the shell, so
     * thread-id equality alone is not enough to classify "the shell".
     * Restrict shell-special signal routing to the actual shell pid. */
    int shell_pid = vprocGetShellSelfPid();
    bool shell_thread = (shell_pid > 0 && entry->pid == shell_pid &&
                         gShellSelfTidValid && pthread_equal(entry->tid, gShellSelfTid));
    VProc *active_vp = vprocCurrent();
    bool active_entry_thread = (active_vp &&
                                vprocPid(active_vp) == entry->pid &&
                                entry->tid &&
                                pthread_equal(entry->tid, self));

    if (sig == SIGTSTP && entry->stop_unsupported) {
        /* Some in-process helpers cannot be parked on a host thread stop.
         * Preserve Unix-like semantics by recording SIGTSTP as pending so the
         * foreground worker can cooperatively surface a stopped status
         * (128 + SIGTSTP). */
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    if (shell_thread && (sig == SIGINT || sig == SIGTSTP)) {
#if defined(PSCAL_TARGET_IOS)
        if (sig == SIGINT && request_runtime_sigint) {
            *request_runtime_sigint = true;
        }
#endif
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    if (active_entry_thread && requested_pid < 0 &&
        (sig == SIGINT || (sig == SIGTSTP && entry->stop_unsupported))) {
        /* Cooperative in-process frontends run on this same thread. Queue the
         * signal so polling paths can observe Ctrl-C/Z without tearing down
         * the thread immediately. Only keep SIGTSTP cooperative for entries
         * that explicitly opted out of hard-stop semantics. */
#if defined(PSCAL_TARGET_IOS)
        if (sig == SIGINT && request_runtime_sigint) {
            *request_runtime_sigint = true;
        }
#endif
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
    bool blocked = vprocSignalBlockedLocked(entry, sig);
    /* SIGINT is blocked on every task for what polls it. A task that ignores it does not see it. */
    if (sig == SIGINT && action == VPROC_SIG_IGNORE) {
        return true;
    }
    vprocDropOpposingPendingLocked(entry, sig);
    if (sig == SIGCONT && entry->stopped && (blocked || action != VPROC_SIG_CONT)) {
        /* Blocked, caught or ignored, a continue still continues. Only its handler waits. */
        entry->stopped = false;
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        entry->continued = true;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_CONT);
    }

    if (blocked) {
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    if (action == VPROC_SIG_HANDLER && !vprocEntryIsCurrentThreadLocked(entry)) {
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    /* A shell's own `kill $$` or `kill 0`, on its own thread: it was going on to its next command.
     * The same for a shell carried by a thread that can't be cancelled. Anywhere else it can be. */
    if (action == VPROC_SIG_KILL &&
        (entry->unwinds_itself ||
         (entry->shell_task && entry->tid &&
          (pthread_equal(entry->tid, self) || vprocThreadUnwindsItselfLocked(entry->tid))))) {
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    vprocApplySignalLocked(entry, sig);

    if (action == VPROC_SIG_KILL && entry->dying_since_ns != 0 && vprocEntryIsCurrentThreadLocked(entry)) {
        *die_here = true;
    }

    if (entry->exited || entry->dying_since_ns != 0) {
        if (entry->tid && !pthread_equal(entry->tid, self) && !vprocThreadUnwindsItselfLocked(entry->tid) &&
            !vprocThreadRunsOtherShellLocked(entry->tid, entry)) {
            vprocCancelListAdd(cancel_list, cancel_count, cancel_capacity, entry->tid);
        }
        for (size_t t = 0; t < entry->thread_count; ++t) {
            pthread_t tid = entry->threads[t];
            if (tid && !pthread_equal(tid, self) && !vprocThreadUnwindsItselfLocked(tid) &&
                !vprocThreadRunsOtherShellLocked(tid, entry)) {
                vprocCancelListAdd(cancel_list, cancel_count, cancel_capacity, tid);
            }
        }
    }
    /* With no thread to report its end through vprocMarkExit, this was it. */
    if (entry->exited && (entry->orphaned || vprocKernelReapsLocked(entry))) {
        vprocClearEntryLocked(entry);
    }
    return true;
}

int vprocKillTree(pid_t pid, int sig) {
    /* Gathered first: a delivery can move a task's children to another parent. */
    int pids[256];
    size_t count = 1;
    pids[0] = (int)pid;
    pthread_mutex_lock(&gVProcTasks.mu);
    for (size_t i = 0; i < count; ++i) {
        VProcTaskEntry *entry = vprocTaskFindLocked(pids[i]);
        if (!entry || !entry->children) {
            continue;
        }
        for (size_t c = 0; c < entry->child_count && count < sizeof(pids) / sizeof(pids[0]); ++c) {
            if (entry->children[c] > 0) {
                pids[count++] = entry->children[c];
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    int rc = vprocKillShim(pid, sig);
    int saved_errno = errno;
    for (size_t i = 1; i < count; ++i) {
        (void)vprocKillShim(pids[i], sig);
    }
    errno = saved_errno;
    return rc;
}

static int vprocKillTasks(pid_t pid, int sig, bool live_only, bool die_later) {
    bool dbg = vprocKillDebugEnabled();
    if (sig < 0 || sig >= 32) {
        if (dbg) {
            vprocDebugLogf("[vproc-kill] invalid signal=%d\n", sig);
        }
        errno = EINVAL;
        return -1;
    }
    /* The VM is the kernel: every pid here means its task table, whoever asks. Its ids are synthetic,
     * so to the host they are other processes, and host code on a thread of its own never gets here:
     * the app's interposer sends that to the real kill. */
    bool target_group = (pid <= 0);
    bool broadcast_all = (pid == -1);
    int target = target_group ? -pid : pid;
    int caller_for_zero = -1;

    if (pid == 0) {
        caller_for_zero = vprocGetPidShim();
        if (caller_for_zero <= 0) {
            caller_for_zero = vprocGetShellSelfPid();
        }
    }

    int rc = 0;
    pthread_t self = pthread_self();
    int self_pid = broadcast_all ? (int)vprocGetPidShim() : -1;
    pthread_t *cancel_list = NULL;
    size_t cancel_count = 0;
    size_t cancel_capacity = 0;
    bool request_runtime_sigint = false;
    bool request_runtime_sigtstp = false;
    bool die_here = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    if (pid == 0) {
        int caller_pgid = vprocTaskPgidByPidLocked(caller_for_zero);
        if (caller_pgid <= 0) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = ESRCH;
            return -1;
        }
        target_group = true;
        target = caller_pgid;
    }
    if (sig == 0) {
        /* Probe for existence: succeed if we find a matching entry. A caller is in its own group. */
        bool found = (pid == 0) || vprocHasKillTargetLocked(pid, self_pid);
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (found) {
            return 0;
        }
        errno = ESRCH;
        return -1;
    }
    if (dbg) {
        vprocDebugLogf( "[vproc-kill] target=%d group=%d broadcast=%d count=%zu\n",
                target, (int)target_group, (int)broadcast_all, gVProcTasks.count);
    }
    bool delivered = false;
    /* A task that has ended stays until it is reaped, and a signal to it succeeds and does nothing. */
    bool found_ended = false;
    /* kill(0, sig) is kill(-pgid, sig) from here on, for the caller's own thread too. */
    pid_t requested_pid = target_group ? (pid_t)(-target) : pid;

    if (!broadcast_all && !target_group) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target);
        if (entry && (entry->zombie || entry->exited)) {
            found_ended = true;
        } else if (entry && vprocKillDeliverEntryLocked(entry, pid, sig, dbg, self,
                                                        &cancel_list, &cancel_count, &cancel_capacity,
                                                        &request_runtime_sigint,
                                                        &request_runtime_sigtstp,
                                                        &die_here)) {
            delivered = true;
        }
    } else {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0) continue;
            if (dbg) {
                vprocDebugLogf( "[vproc-kill] scan pid=%d pgid=%d sid=%d exited=%d zombie=%d\n",
                        entry->pid, entry->pgid, entry->sid, entry->exited, entry->zombie);
            }

            if (broadcast_all) {
                if (!vprocBroadcastReachesLocked(entry, self_pid)) continue;
            } else if (target_group) {
                if (entry->pgid != target) continue;
            }

            if (entry->zombie || entry->exited) {
                found_ended = true;
                continue;
            }

            if (vprocKillDeliverEntryLocked(entry, requested_pid, sig, dbg, self,
                                            &cancel_list, &cancel_count, &cancel_capacity,
                                            &request_runtime_sigint,
                                            &request_runtime_sigtstp,
                                            &die_here)) {
                delivered = true;
            }
        }
    }
    
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    /* gVProcTasks.cv only wakes waiters parked on the task table (waitpid,
     * stop-wait). Threads blocked in poll/select sleep on the global poll-wake
     * pipe, and in-proc pipe waiters re-check on their timed wait -- poke the
     * wake pipe so a queued signal (Ctrl-C/Ctrl-Z) is observed promptly as
     * EINTR rather than only at the next cooperative checkpoint. */
    pscal_fd_poll_wakeup(NULL, POLLIN);

    for (size_t i = 0; i < cancel_count; ++i) {
        pthread_cancel(cancel_list[i]);
    }
    free(cancel_list);

    if (request_runtime_sigint && pscalRuntimeRequestSigint) {
        pscalRuntimeRequestSigint();
    }
    if (request_runtime_sigtstp && pscalRuntimeRequestSigtstp) {
        pscalRuntimeRequestSigtstp();
    }

    if (die_here && !die_later) {
        vprocDieHere(true);
    }

    if (delivered || (found_ended && !live_only)) return rc;

    if (dbg) {
        vprocDebugLogf( "[vproc-kill] no targets pid=%d target=%d group=%d broadcast=%d\n",
                (int)pid, target, (int)target_group, (int)broadcast_all);
    }
    errno = ESRCH;
    return -1;
}

static bool vprocKillPermitted(pid_t pid);

int vprocKillShim(pid_t pid, int sig) {
    if (!vprocKillPermitted(pid)) {
        errno = EPERM;
        return -1;
    }
    return vprocKillTasks(pid, sig, false, false);
}

int vprocKillLive(pid_t pid, int sig) {
    return vprocKillTasks(pid, sig, true, false);
}

/* A write to a pipe, FIFO or socket whose reader has gone. On Linux the writer takes SIGPIPE, which by
 * default ends it, and only a writer that ignores, blocks or catches it sees EPIPE. Here the writer may
 * hold a lock of its own, stdio's or the VM's, so a fatal one marks the task dying and it ends at its
 * next check or cancellation point, as when it is killed from elsewhere: a VM before its next
 * instruction, an applet when it returns, a shell before its next command. */
static void vprocRaiseClosedPipe(int pid) {
    if (pid <= 0) {
        return;
    }
    int saved_errno = errno;
    (void)vprocKillTasks(pid, SIGPIPE, true, true);
    errno = saved_errno;
}

/* ... (Remaining functions for getters/setters/shell-self-pid are unchanged logically) ... */
/* INCLUDED HERE FOR COMPLETENESS */

pid_t vprocGetPidShim(void) {
    VProc *vp = vprocCurrent();
    if (vp) {
        return vprocPid(vp);
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        return shell_pid;
    }
    return vprocHostGetpidRaw();
}

static inline bool vprocShimHasVirtualContext(void) {
    return vprocCurrent() != NULL || vprocGetShellSelfPid() > 0;
}

typedef struct {
    bool active;
    bool in_child;
    sigjmp_buf *parent_env;
    VProc *child_vp;
    int child_pid;
} VProcSimForkState;

typedef struct {
    VProc *vp;
    VProcExecEntryFn entry;
    int argc;
    char **argv;
    VProcEnv *env;
    int status;
} VProcSimExecCtx;

typedef struct {
    VProcFdKind kind;
    int source_host_fd;
    int cloned_host_fd;
    struct pscal_fd *pscal_fd;
} VProcForkFdCloneEntry;

static __thread VProcSimForkState gVProcSimForkState;

static void vprocSimForkResetState(VProcSimForkState *state) {
    if (!state) {
        return;
    }
    state->active = false;
    state->in_child = false;
    state->parent_env = NULL;
    state->child_vp = NULL;
    state->child_pid = 0;
}

static bool vprocSimForkDebugEnabled(void) {
    const char *tool_debug = getenv("PSCALI_TOOL_DEBUG");
    const char *ssh_debug = getenv("PSCALI_SSH_DEBUG");
    if ((tool_debug && *tool_debug && strcmp(tool_debug, "0") != 0) ||
        (ssh_debug && *ssh_debug && strcmp(ssh_debug, "0") != 0)) {
        return true;
    }
    return false;
}

static void vprocSimForkLog(const char *fmt, ...) {
    if (!vprocSimForkDebugEnabled() || !fmt) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(PSCAL_TARGET_IOS)
    if (&pscalRuntimeDebugLog != NULL) {
        pscalRuntimeDebugLog(buf);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
#else
    fprintf(stderr, "%s\n", buf);
#endif
}

static char **vprocSimDupArgv(char *const argv[], int *out_argc) {
    int argc = 0;
    if (argv) {
        while (argv[argc]) {
            if (argc == INT_MAX) {
                return NULL;
            }
            argc++;
        }
    }
    size_t argc_size = (size_t)argc;
    if (argc_size > (SIZE_MAX / sizeof(char *)) - 1) {
        return NULL;
    }
    char **copy = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!copy) {
        return NULL;
    }
    for (int i = 0; i < argc; ++i) {
        const char *src = argv[i] ? argv[i] : "";
        copy[i] = strdup(src);
        if (!copy[i]) {
            for (int j = 0; j < i; ++j) {
                free(copy[j]);
            }
            free(copy);
            return NULL;
        }
    }
    copy[argc] = NULL;
    if (out_argc) {
        *out_argc = argc;
    }
    return copy;
}

static void vprocSimFreeArgv(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static void vprocSimForkFdCloneCleanup(VProcForkFdCloneEntry *entries,
                                       size_t count,
                                       bool close_host_fds) {
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].pscal_fd) {
            pscal_fd_close(entries[i].pscal_fd);
            entries[i].pscal_fd = NULL;
        }
        if (close_host_fds && entries[i].cloned_host_fd >= 0) {
            vprocHostCloseRaw(entries[i].cloned_host_fd);
            entries[i].cloned_host_fd = -1;
        }
    }
    free(entries);
}

static void vprocCloseAllFdsLocked(VProc *vp) {
    if (!vp || !vp->entries) {
        return;
    }
    for (size_t i = 0; i < vp->capacity; ++i) {
        if (vp->entries[i].kind == VPROC_FD_PSCAL && vp->entries[i].pscal_fd) {
            pscal_fd_close(vp->entries[i].pscal_fd);
        } else if (vp->entries[i].kind == VPROC_FD_HOST &&
                   vp->entries[i].host_fd >= 0) {
            (void)vprocResourceRemoveLocked(vp, vp->entries[i].host_fd);
            vprocForgetSavedStdioLocked(vp, vp->entries[i].host_fd);
            vprocHostCloseRaw(vp->entries[i].host_fd);
        }
        vp->entries[i].host_fd = -1;
        vp->entries[i].pscal_fd = NULL;
        vp->entries[i].kind = VPROC_FD_NONE;
    }
    vprocResourceCloseAllLocked(vp);
    vp->stdin_host_fd = -1;
    vp->stdout_host_fd = -1;
    vp->stderr_host_fd = -1;
    vp->stdin_from_session = false;
}

static bool vprocSimForkCloneFdTable(VProc *child, VProc *parent) {
    if (!child || !parent) {
        errno = EINVAL;
        return false;
    }

    size_t parent_cap = 0;
    int parent_next_fd = 3;
    bool parent_stdin_from_session = false;
    VProcForkFdCloneEntry *clone_entries = NULL;

    pthread_mutex_lock(&parent->mu);
    parent_cap = parent->capacity;
    parent_next_fd = parent->next_fd;
    parent_stdin_from_session = parent->stdin_from_session;
    if (parent_cap > 0) {
        clone_entries =
                (VProcForkFdCloneEntry *)calloc(parent_cap, sizeof(VProcForkFdCloneEntry));
        if (!clone_entries) {
            pthread_mutex_unlock(&parent->mu);
            errno = ENOMEM;
            return false;
        }
        for (size_t i = 0; i < parent_cap; ++i) {
            VProcFdEntry src = parent->entries[i];
            clone_entries[i].kind = src.kind;
            clone_entries[i].source_host_fd = -1;
            clone_entries[i].cloned_host_fd = -1;
            clone_entries[i].pscal_fd = NULL;
            if (src.kind == VPROC_FD_HOST && src.host_fd >= 0) {
                clone_entries[i].source_host_fd = src.host_fd;
            } else if (src.kind == VPROC_FD_PSCAL && src.pscal_fd) {
                clone_entries[i].pscal_fd = pscal_fd_retain(src.pscal_fd);
                if (!clone_entries[i].pscal_fd) {
                    pthread_mutex_unlock(&parent->mu);
                    vprocSimForkFdCloneCleanup(clone_entries, parent_cap, false);
                    errno = ENOMEM;
                    return false;
                }
            } else {
                clone_entries[i].kind = VPROC_FD_NONE;
            }
        }
    }
    pthread_mutex_unlock(&parent->mu);

    for (size_t i = 0; i < parent_cap; ++i) {
        if (clone_entries[i].kind != VPROC_FD_HOST ||
            clone_entries[i].source_host_fd < 0) {
            continue;
        }
        int duped = vprocCloneFd(clone_entries[i].source_host_fd);
        if (duped < 0) {
            int saved_errno = (errno != 0) ? errno : EIO;
            /*
             * Some host descriptors owned by UIKit/WebKit/runtime threads are
             * not safely duplicable inside the app sandbox. Dropping those
             * non-stdio descriptors keeps fork-compatible applets (rsync/scp)
             * functional while preserving required stdin/stdout/stderr wiring.
             */
            if (i <= (size_t)STDERR_FILENO) {
                vprocSimForkFdCloneCleanup(clone_entries, parent_cap, true);
                errno = saved_errno;
                return false;
            }
            clone_entries[i].kind = VPROC_FD_NONE;
            clone_entries[i].source_host_fd = -1;
            if (vprocSimForkDebugEnabled()) {
                vprocSimForkLog("[vproc-fork] skip unclonable fd=%zu errno=%d",
                                i, saved_errno);
            }
            continue;
        }
        clone_entries[i].cloned_host_fd = duped;
        if (getenv("PSCALI_TOOL_DEBUG") != NULL && i < 8) {
            fprintf(stderr,
                    "[vproc-fork-clone] fd=%zu src=%d dup=%d\n",
                    i,
                    clone_entries[i].source_host_fd,
                    duped);
        }
    }

    pthread_mutex_lock(&child->mu);
    vprocCloseAllFdsLocked(child);
    if (parent_cap > child->capacity &&
        !vprocEnsureFdCapacityLocked(child, parent_cap)) {
        pthread_mutex_unlock(&child->mu);
        vprocSimForkFdCloneCleanup(clone_entries, parent_cap, true);
        return false;
    }
    for (size_t i = 0; i < child->capacity; ++i) {
        child->entries[i].host_fd = -1;
        child->entries[i].pscal_fd = NULL;
        child->entries[i].kind = VPROC_FD_NONE;
    }
    for (size_t i = 0; i < parent_cap; ++i) {
        if (clone_entries[i].kind == VPROC_FD_HOST &&
            clone_entries[i].cloned_host_fd >= 0) {
            child->entries[i].host_fd = clone_entries[i].cloned_host_fd;
            child->entries[i].pscal_fd = NULL;
            child->entries[i].kind = VPROC_FD_HOST;
            vprocResourceTrackLocked(child,
                                     clone_entries[i].cloned_host_fd,
                                     VPROC_RESOURCE_GENERIC);
            clone_entries[i].cloned_host_fd = -1;
        } else if (clone_entries[i].kind == VPROC_FD_PSCAL &&
                   clone_entries[i].pscal_fd) {
            child->entries[i].host_fd = -1;
            child->entries[i].pscal_fd = clone_entries[i].pscal_fd;
            child->entries[i].kind = VPROC_FD_PSCAL;
            clone_entries[i].pscal_fd = NULL;
        }
    }
    child->next_fd = (parent_next_fd >= 0 && (size_t)parent_next_fd < child->capacity)
                             ? parent_next_fd
                             : 3;
    child->stdin_fd = STDIN_FILENO;
    child->stdout_fd = STDOUT_FILENO;
    child->stderr_fd = STDERR_FILENO;
    child->stdin_host_fd =
            (child->capacity > (size_t)STDIN_FILENO &&
             child->entries[STDIN_FILENO].kind == VPROC_FD_HOST)
                    ? child->entries[STDIN_FILENO].host_fd
                    : -1;
    child->stdout_host_fd =
            (child->capacity > (size_t)STDOUT_FILENO &&
             child->entries[STDOUT_FILENO].kind == VPROC_FD_HOST)
                    ? child->entries[STDOUT_FILENO].host_fd
                    : -1;
    child->stderr_host_fd =
            (child->capacity > (size_t)STDERR_FILENO &&
             child->entries[STDERR_FILENO].kind == VPROC_FD_HOST)
                    ? child->entries[STDERR_FILENO].host_fd
                    : -1;
    child->stdin_from_session =
            parent_stdin_from_session && (child->stdin_host_fd >= 0);
    pthread_mutex_unlock(&child->mu);

    vprocSimForkFdCloneCleanup(clone_entries, parent_cap, true);
    return true;
}

static void vprocSimExecFinish(void *arg) {
    VProcSimExecCtx *ctx = (VProcSimExecCtx *)arg;
    /* This also runs on the normal way out, where a kill could stop it half done. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if (ctx->vp) {
        vprocMarkExit(ctx->vp, ctx->status);
        vprocDestroy(ctx->vp);
    }
    vprocEnvActivate(NULL);
    vprocEnvRelease(ctx->env);
    vprocSimFreeArgv(ctx->argv, ctx->argc);
    free(ctx);
}

/* True when the task was killed before this, its first thread, was there to be cancelled. */
static bool vprocSimExecAlreadyKilled(VProc *vp) {
    bool exited = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vp ? vprocTaskFindLocked(vp->pid) : NULL;
    if (entry) {
        exited = entry->exited;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return exited;
}

static void *vprocSimExecThread(void *arg) {
    VProcSimExecCtx *ctx = (VProcSimExecCtx *)arg;
    if (!ctx) {
        return (void *)(intptr_t)127;
    }
    int status = 127;
    ctx->status = status;
    /* A kill cancels this thread and the app's exit() is pthread_exit. Either way what the
     * child held has to close, or the pipes it held never reach end of file. */
    pthread_cleanup_push(vprocSimExecFinish, ctx);
    if (ctx->vp) {
        vprocSetSubshellPid(vprocPid(ctx->vp));
    }
    vprocEnvActivate(ctx->env);
    /* A signal from before the exec, which nothing else here would look at. */
    vprocDeliverPendingSignalsForCurrent();
    if (ctx->entry && !vprocSimExecAlreadyKilled(ctx->vp)) {
        ctx->status = ctx->entry(ctx->argc, ctx->argv);
    }
    status = ctx->status;
    pthread_cleanup_pop(1);
    return (void *)(intptr_t)status;
}

/* As exec does: the code a handler was in has gone, so a caught signal goes back to its default
 * action. What was ignored stays ignored, and the mask and what is pending stay as they are. */
static void vprocSimExecResetCaughtSignals(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    for (int sig = 1; entry && sig < 32; ++sig) {
        if (entry->actions[sig].sa_handler != SIG_IGN && entry->actions[sig].sa_handler != SIG_DFL) {
            sigemptyset(&entry->actions[sig].sa_mask);
            entry->actions[sig].sa_handler = SIG_DFL;
            entry->actions[sig].sa_flags = 0;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

static int vprocSimSpawnChild(VProc *vp, VProcExecEntryFn entry, char *const argv[]) {
    int argc = 0;
    char **argv_copy = vprocSimDupArgv(argv, &argc);
    if (!argv_copy) {
        errno = ENOMEM;
        return -1;
    }
    VProcSimExecCtx *ctx = (VProcSimExecCtx *)calloc(1, sizeof(VProcSimExecCtx));
    if (!ctx) {
        vprocSimFreeArgv(argv_copy, argc);
        errno = ENOMEM;
        return -1;
    }
    ctx->vp = vp;
    ctx->entry = entry;
    ctx->argc = argc;
    ctx->argv = argv_copy;
    /* A process's environment is its own from here on, as with fork. */
    ctx->env = vprocEnvCapture();

    pthread_t tid;
    int err = vprocSpawnThread(vp, vprocSimExecThread, ctx, &tid);
    if (err != 0) {
        vprocEnvRelease(ctx->env);
        vprocSimFreeArgv(argv_copy, argc);
        free(ctx);
        errno = err;
        vprocSimForkLog("[vproc-fork] spawn thread failed err=%d", err);
        return -1;
    }
    pthread_detach(tid);
    vprocSimForkLog("[vproc-fork] spawn thread ok");
    return 0;
}

pid_t vprocGetPpidShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetppidRaw();
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    pid_t parent = (pid_t)-1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        parent = (pid_t)entry->parent_pid;
        for (int hops = 0; parent > 0 && hops < 32; ++hops) {
            const VProcTaskEntry *above = vprocTaskFindLocked((int)parent);
            if (!above || !above->not_a_process) {
                break;
            }
            parent = (pid_t)above->parent_pid;
        }
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return parent;
}

pid_t vprocSimulatedFork(const char *label, bool inherit_parent_pgid) {
    return vprocSimulatedForkWithEnv(label, inherit_parent_pgid, NULL);
}

pid_t vprocSimulatedForkWithEnv(const char *label,
                                bool inherit_parent_pgid,
                                sigjmp_buf *parent_env) {
    VProcSimForkState *state = &gVProcSimForkState;
    const char *fork_label = (label && *label) ? label : "fork";
    vprocSimForkLog("[vproc-fork] fork enter active=%d in_child=%d",
                    state->active ? 1 : 0,
                    state->in_child ? 1 : 0);
    if (state->active) {
        errno = EAGAIN;
        return -1;
    }
    if (!parent_env) {
        errno = EINVAL;
        vprocSimForkLog("[vproc-fork] missing caller env");
        return -1;
    }

    VProcCommandScope scope;
    if (!vprocCommandScopeBegin(&scope, fork_label, true, inherit_parent_pgid)) {
        errno = ENOSYS;
        vprocSimForkLog("[vproc-fork] vprocCommandScopeBegin failed");
        return -1;
    }
    if (scope.prev && scope.vp) {
        if (!vprocSimForkCloneFdTable(scope.vp, scope.prev)) {
            int saved_errno = (errno != 0) ? errno : EIO;
            vprocCommandScopeEnd(&scope, 127);
            errno = saved_errno;
            vprocSimForkLog("[vproc-fork] fd clone failed errno=%d", saved_errno);
            return -1;
        }
    }

    state->active = true;
    state->in_child = true;
    state->parent_env = parent_env;
    state->child_vp = scope.vp;
    state->child_pid = scope.pid;
    vprocSimForkLog("[vproc-fork] fork child pid=%d", scope.pid);
    return 0;
}

pid_t vprocSimulatedForkParentResume(void) {
    VProcSimForkState *state = &gVProcSimForkState;
    if (!state->active || state->child_pid <= 0) {
        errno = ENOSYS;
        return (pid_t)-1;
    }
    int pid = state->child_pid;
    vprocSimForkLog("[vproc-fork] fork parent resume pid=%d", pid);
    vprocSimForkResetState(state);
    return (pid_t)pid;
}

void vprocSetCommandLabel(int pid, const char *label);

int vprocSimulatedExec(VProcExecEntryFn entry, char *const argv[]) {
    VProcSimForkState *state = &gVProcSimForkState;
    vprocSimForkLog("[vproc-fork] exec entry=%p active=%d in_child=%d child_vp=%p child_pid=%d",
                    (void *)entry,
                    state->active ? 1 : 0,
                    state->in_child ? 1 : 0,
                    (void *)state->child_vp,
                    state->child_pid);
    if (!state->active || !state->in_child || !state->child_vp ||
        !state->parent_env) {
        errno = ENOSYS;
        vprocSimForkLog("[vproc-fork] exec invalid fork state");
        return -1;
    }
    if (!entry) {
        vprocSimForkResetState(state);
        errno = ENOENT;
        vprocSimForkLog("[vproc-fork] exec missing entry");
        return -1;
    }
    if (state->child_pid > 0 && argv && argv[0] && argv[0][0]) {
        const char *label = argv[0];
        const char *slash = strrchr(label, '/');
        if (slash && slash[1] != '\0') {
            label = slash + 1;
        }
        vprocSetCommandLabel(state->child_pid, label);
    }
    vprocSimExecResetCaughtSignals(state->child_pid);
    if (vprocSimSpawnChild(state->child_vp, entry, argv) != 0) {
        if (errno == 0) {
            errno = EIO;
        }
        vprocSimForkLog("[vproc-fork] exec spawn failed errno=%d", errno);
        vprocSimForkResetState(state);
        return -1;
    }
    vprocSimForkLog("[vproc-fork] exec spawn ok, jumping to parent");
    vprocUnregisterThread(state->child_vp, pthread_self());
    vprocDeactivate();
    siglongjmp(*state->parent_env, 1);
    return -1;
}

static _Thread_local VProcProgramFrame *gVprocProgramFrame;

void vprocProgramFramePush(VProcProgramFrame *frame) {
    VProc *vp = vprocCurrent();
    frame->pid = vp ? vprocPid(vp) : 0;
    frame->handler_task = gVprocHandlerTask;
#if defined(__APPLE__)
    frame->cleanup_stack = pthread_self()->__cleanup_stack;
#endif
    frame->exec_ended = false;
    frame->exec_status = 0;
    frame->prev = gVprocProgramFrame;
    frame->pushed = true;
    gVprocProgramFrame = frame;
}

void vprocProgramFramePop(VProcProgramFrame *frame) {
    if (frame->pushed) {
        gVprocProgramFrame = frame->prev;
        frame->pushed = false;
    }
}

static int vprocKilledChildMain(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* The task's signal is what its waiter gets. */
    return 0;
}

/* A task has just been ended by a signal it sent or unblocked on its own thread, and on Linux the
 * call would not return. What that takes depends on what the thread is. `now` is for a call the
 * program made itself. Inside any other shim the caller may hold a lock of its own, a FILE's or the
 * VM's, so the thread ends at its next cancellation point instead, as when killed from outside. */
static void vprocDieHere(bool now) {
    VProc *vp = vprocCurrent();
    int pid = vp ? vprocPid(vp) : 0;
    if (pid <= 0) {
        return;
    }
    /* A simulated fork's child is on its parent's thread, and the parent goes on from its fork(). */
    VProcSimForkState *fork_state = &gVProcSimForkState;
    if (now && fork_state->active && fork_state->in_child && fork_state->child_pid == pid) {
        char *argv[] = { (char *)"killed", NULL };
        (void)vprocSimulatedExec(vprocKilledChildMain, argv);
    }
    pthread_t self = pthread_self();
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *self_entry = vprocTaskFindLocked(pid);
    bool shell_beneath = vprocThreadUnwindsItselfLocked(self) ||
                         (self_entry && vprocThreadRunsOtherShellLocked(self, self_entry));
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    int sig = entry ? entry->exit_signal : 0;
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (!shell_beneath) {
        /* A thread of its own ends as if killed from outside, its handlers doing the rest. With
         * cancels off, in setup or teardown, that happens when they go on. */
        pthread_cancel(self);
        if (now) {
            pthread_testcancel();
        }
        return;
    }
    VProcProgramFrame *frame = gVprocProgramFrame;
    if (now && frame && frame->pid == pid) {
        gVprocProgramFrame = frame->prev;
        frame->pushed = false;
        gVprocHandlerTask = frame->handler_task;
#if defined(__APPLE__)
        /* What the program pushed goes with its stack, which a later cancel must not walk. */
        self->__cleanup_stack = frame->cleanup_stack;
#endif
        longjmp(frame->env, 128 + (sig > 0 ? sig : SIGTERM));
    }
    /* Nothing can take it off a thread that has to go on. It stays dying, as a task killed from
     * another thread does here: a blocking call gives up, and its end comes from vprocMarkExit. */
}

pid_t vprocSpawnEntry(const char *label,
                      bool inherit_parent_pgid,
                      VProcExecEntryFn entry,
                      char *const argv[]) {
    if (!entry) {
        errno = ENOENT;
        return (pid_t)-1;
    }
    VProcCommandScope scope;
    if (!vprocCommandScopeBegin(&scope, label, true, inherit_parent_pgid)) {
        errno = ENOSYS;
        return (pid_t)-1;
    }
    if (scope.prev && !vprocSimForkCloneFdTable(scope.vp, scope.prev)) {
        int saved_errno = (errno != 0) ? errno : EIO;
        vprocCommandScopeEnd(&scope, 127);
        errno = saved_errno;
        return (pid_t)-1;
    }
    /* Off the child before it starts: a kill must not cancel this thread, and the child's
     * thread may have finished and destroyed the vproc by the time the spawn returns. */
    int pid = scope.pid;
    VProc *vp = scope.vp;
    vprocUnregisterThreadByPid(pid, pthread_self());
    vprocDeactivate();
    if (vprocSimSpawnChild(vp, entry, argv) != 0) {
        int saved_errno = (errno != 0) ? errno : EIO;
        vprocMarkExit(vp, 127);
        vprocDiscard(pid);
        vprocDestroy(vp);
        errno = saved_errno;
        return (pid_t)-1;
    }
    return (pid_t)pid;
}

bool vprocCommandScopeBegin(VProcCommandScope *scope,
                            const char *label,
                            bool force_new_vproc,
                            bool inherit_parent_pgid) {
    if (!scope) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    (void)vprocEnsureKernelPid();
#endif
    memset(scope, 0, sizeof(*scope));
    scope->prev = vprocCurrent();

    int shell_pid = vprocGetShellSelfPid();
    bool need_new = force_new_vproc ||
                    scope->prev == NULL ||
                    (shell_pid > 0 && scope->prev && vprocPid(scope->prev) == shell_pid);
    if (!need_new) {
        return false;
    }

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    if (scope->prev) {
        int host_in = vprocTranslateFd(scope->prev, STDIN_FILENO);
        int host_out = vprocTranslateFd(scope->prev, STDOUT_FILENO);
        int host_err = vprocTranslateFd(scope->prev, STDERR_FILENO);
        if (host_in >= 0) opts.stdin_fd = host_in;
        if (host_out >= 0) opts.stdout_fd = host_out;
        if (host_err >= 0) opts.stderr_fd = host_err;
    } else {
        opts.stdin_fd = STDIN_FILENO;
        opts.stdout_fd = STDOUT_FILENO;
        opts.stderr_fd = STDERR_FILENO;
    }

    VProc *vp = vprocCreate(&opts);
    if (!vp) {
        opts.stdin_fd = -2;
        vp = vprocCreate(&opts);
    }
    if (!vp) {
        return false;
    }

    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);
    scope->vp = vp;
    scope->pid = pid;
    vprocResetCaughtSignals(pid);

    int owner_pid = scope->prev ? vprocPid(scope->prev) : vprocGetShellSelfPid();
    int kernel_pid = vprocGetKernelPid();
    int parent_pid = owner_pid;
    if (parent_pid <= 0 || parent_pid == pid) {
        parent_pid = (kernel_pid > 0) ? kernel_pid : owner_pid;
    }
    if (parent_pid > 0 && parent_pid != pid) {
        vprocSetParent(pid, parent_pid);
    }

    if (inherit_parent_pgid) {
        int parent_pgid = (owner_pid > 0) ? vprocGetPgid(owner_pid) : -1;
        if (parent_pgid > 0) {
            vprocSetPgid(pid, parent_pgid);
        } else {
            vprocSetPgid(pid, pid);
        }
    } else {
        vprocSetPgid(pid, pid);
    }

    if (label && *label) {
        vprocSetCommandLabel(pid, label);
    }

    vprocActivate(vp);
    return true;
}

bool vprocCommandScopeTakeDescriptors(VProcCommandScope *scope) {
    if (!scope || !scope->vp || !scope->prev) {
        return false;
    }
    return vprocSimForkCloneFdTable(scope->vp, scope->prev);
}

void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code) {
    if (!scope || !scope->vp) {
        return;
    }

    VProc *vp = scope->vp;
    int pid = scope->pid > 0 ? scope->pid : vprocPid(vp);
    int stop_sig = 0;
    bool stop_status = false;
    if (exit_code >= 128 && exit_code < 128 + NSIG) {
        int sig = exit_code - 128;
        if (sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU) {
            stop_status = true;
            stop_sig = sig;
        }
    }

    vprocDeactivate();
    if (stop_status && pid > 0) {
        /* Scoped applets often execute on the shell thread; detach that thread
         * identity before preserving a synthetic stopped task entry. */
        vprocUnregisterThread(vp, pthread_self());
        /* Some scoped applets (notably watch) disable direct stops while
         * running; re-enable stop handling so scope teardown can publish a
         * stopped state to wait/jobs consumers. */
        vprocSetStopUnsupported(pid, false);
        if (stop_sig > 0) {
            (void)vprocKillShim(pid, stop_sig);
        }
    } else {
        vprocMarkExit(vp, exit_code);
        vprocDiscard(pid);
    }
    vprocDestroy(vp);

    scope->prev = NULL;
    scope->vp = NULL;
    scope->pid = 0;
}

pid_t vprocGetpgrpShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetpgrpRaw();
    }
    pid_t pid = vprocGetPidShim();
    int pgid = vprocGetPgid((int)pid);
    return (pid_t)pgid;
}

pid_t vprocGetpgidShim(pid_t pid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetpgidRaw(pid);
    }
    int target = (pid == 0) ? (int)vprocGetPidShim() : (int)pid;
    int pgid = vprocGetPgid(target);
    return (pid_t)pgid;
}

int vprocSetpgidShim(pid_t pid, pid_t pgid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostSetpgidRaw(pid, pgid);
    }
    return vprocSetPgid((int)pid, (int)pgid);
}

pid_t vprocGetsidShim(pid_t pid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetsidRaw(pid);
    }
    int target = (pid == 0) ? (int)vprocGetPidShim() : (int)pid;
    int sid = vprocGetSid(target);
    return (pid_t)sid;
}

pid_t vprocSetsidShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostSetsidRaw();
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    pid_t rc = (pid_t)-1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        errno = ESRCH;
        goto out;
    }
    if (entry->pgid == pid) {
        errno = EPERM;
        goto out;
    }
    entry->sid = pid;
    entry->pgid = pid;
    entry->session_leader = true;
    entry->fg_pgid = pid;
    entry->blocked_signals = 0;
    entry->pending_signals = 0;
    entry->gave_up_terminal = false;
    rc = (pid_t)pid;
out:
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

pid_t vprocTcgetpgrpShim(int fd) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostTcgetpgrpRaw(fd);
    }
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            dword_t fg = 0;
            int res = _ENOTTY;
            if (pscal_fd->ops && pscal_fd->ops->ioctl) {
                res = pscal_fd->ops->ioctl(pscal_fd, TIOCGPGRP_, &fg);
            }
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                vprocSetCompatErrno(res);
                return (pid_t)-1;
            }
            return (pid_t)fg;
        }
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    int sid = -1;
    int fg = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        sid = entry->sid;
        if (sid > 0) {
            VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
            if (leader) {
                fg = leader->fg_pgid;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (sid <= 0) {
        errno = ENOTTY;
        return (pid_t)-1;
    }
    if (fg <= 0) {
        errno = ESRCH;
        return (pid_t)-1;
    }
    return (pid_t)fg;
}

int vprocTcsetpgrpShim(int fd, pid_t pgid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostTcsetpgrpRaw(fd, pgid);
    }
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            dword_t fg = (dword_t)pgid;
            int res = _ENOTTY;
            if (pscal_fd->ops && pscal_fd->ops->ioctl) {
                res = pscal_fd->ops->ioctl(pscal_fd, TIOCSPGRP_, &fg);
            }
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                vprocSetCompatErrno(res);
                return -1;
            }
            return 0;
        }
    }
    if (pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int sid = -1;
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *self_entry = vprocTaskFindLocked(pid);
    if (self_entry) {
        sid = self_entry->sid;
    }
    if (sid <= 0) {
        errno = ENOTTY;
        goto out_tcset;
    }
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    bool group_ok = false;
    if (leader && leader->sid == sid && leader->pgid == (int)pgid) {
        group_ok = true;
    }
    if (!group_ok) {
        VProcTaskEntry *pgid_entry = vprocTaskFindLocked((int)pgid);
        if (pgid_entry &&
            pgid_entry->sid == sid &&
            pgid_entry->pgid == (int)pgid) {
            group_ok = true;
        }
    }
    if (!group_ok) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0) continue;
            if (entry->sid != sid) continue;
            if (entry->pgid == (int)pgid) {
                group_ok = true;
                break;
            }
        }
    }
    if (!leader) {
        errno = ESRCH;
        goto out_tcset;
    }
    if (!group_ok) {
        errno = EPERM;
        goto out_tcset;
    }
    leader->fg_pgid = (int)pgid;
    rc = 0;
out_tcset:
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rc == 0) {
        vprocSyncForegroundPgidToSessionTty(sid, (int)pgid);
    }
    return rc;
}

static int vprocSessionShellPidHint(VProcSessionStdio *session) {
    if (!session || vprocSessionStdioIsDefault(session)) {
        return 0;
    }
    if (session->shell_pid > 0) {
        return session->shell_pid;
    }
    if (session->session_id == 0) {
        return 0;
    }

    int shell_pid = 0;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session->session_id, NULL);
    if (entry && entry->shell_pid > 0) {
        shell_pid = entry->shell_pid;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    if (shell_pid > 0) {
        session->shell_pid = shell_pid;
    }
    return shell_pid;
}

void vprocSetShellSelfPid(int pid) {
    gShellSelfPid = pid;
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && !vprocSessionStdioIsDefault(session)) {
        session->shell_pid = pid;
        if (session->session_id != 0) {
            vprocSessionPtySetShellPid(session->session_id, pid);
        }
    } else if (pid > 0) {
        gShellSelfPidGlobal = pid;
    }
    if (pid > 0) {
        gVProcInterposeReady = 1;
        /* Ensure the shell pid always has a task entry so later synthetic
         * children can inherit session metadata (sid/pgid) deterministically. */
        pthread_mutex_lock(&gVProcTasks.mu);
        (void)vprocTaskEnsureSlotLocked(pid);
        pthread_mutex_unlock(&gVProcTasks.mu);
    }
}

int vprocGetShellSelfPid(void) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    int session_shell_pid = vprocSessionShellPidHint(session);
    if (session_shell_pid > 0) {
        gShellSelfPid = session_shell_pid;
        return session_shell_pid;
    }
    if (gShellSelfPid > 0) {
        return gShellSelfPid;
    }
    return gShellSelfPidGlobal;
}

int vprocThreadIsRegistered(pthread_t tid) {
    bool registered = false;
    int self_pid_hint = -1;
    if (pthread_equal(tid, pthread_self())) {
        VProc *current = vprocCurrent();
        if (current) {
            self_pid_hint = vprocPid(current);
        }
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    if (self_pid_hint > 0) {
        VProcTaskEntry *entry = vprocTaskFindLocked(self_pid_hint);
        registered = vprocTaskEntryHasThreadLocked(entry, tid);
    }
    if (!registered) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (vprocTaskEntryHasThreadLocked(entry, tid)) {
                registered = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return registered ? 1 : 0;
}

int vprocThreadIsRegisteredNonblocking(pthread_t tid) {
    bool registered = false;
    int self_pid_hint = -1;
    if (pthread_equal(tid, pthread_self())) {
        VProc *current = vprocCurrent();
        if (current) {
            self_pid_hint = vprocPid(current);
        }
    }
    if (pthread_mutex_trylock(&gVProcTasks.mu) != 0) {
        return 0;
    }
    if (self_pid_hint > 0) {
        VProcTaskEntry *entry = vprocTaskFindLocked(self_pid_hint);
        registered = vprocTaskEntryHasThreadLocked(entry, tid);
    }
    if (!registered) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (vprocTaskEntryHasThreadLocked(entry, tid)) {
                registered = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return registered ? 1 : 0;
}

static bool vprocInterposeBypassFindIndexLocked(pthread_t tid, size_t *out_index) {
    if (out_index) {
        *out_index = 0;
    }
    if (gVProcInterposeBypassRegistry.hint_valid &&
        gVProcInterposeBypassRegistry.hint_index < gVProcInterposeBypassRegistry.count &&
        pthread_equal(gVProcInterposeBypassRegistry.hint_tid, tid)) {
        size_t idx = gVProcInterposeBypassRegistry.hint_index;
        if (pthread_equal(gVProcInterposeBypassRegistry.items[idx], tid)) {
            if (out_index) {
                *out_index = idx;
            }
            return true;
        }
    }
    for (size_t i = 0; i < gVProcInterposeBypassRegistry.count; ++i) {
        if (!pthread_equal(gVProcInterposeBypassRegistry.items[i], tid)) {
            continue;
        }
        gVProcInterposeBypassRegistry.hint_valid = true;
        gVProcInterposeBypassRegistry.hint_tid = tid;
        gVProcInterposeBypassRegistry.hint_index = i;
        if (out_index) {
            *out_index = i;
        }
        return true;
    }
    return false;
}

int vprocThreadIsInterposeBypassed(pthread_t tid) {
    bool bypassed = false;
    pthread_mutex_lock(&gVProcInterposeBypassRegistry.mu);
    bypassed = vprocInterposeBypassFindIndexLocked(tid, NULL);
    pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
    return bypassed ? 1 : 0;
}

void vprocRegisterInterposeBypassThread(pthread_t tid) {
    pthread_mutex_lock(&gVProcInterposeBypassRegistry.mu);
    if (vprocInterposeBypassFindIndexLocked(tid, NULL)) {
        pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
        return;
    }
    if (gVProcInterposeBypassRegistry.count >= gVProcInterposeBypassRegistry.capacity) {
        size_t new_capacity = 0;
        if (!vprocComputeGrowthCapacity(gVProcInterposeBypassRegistry.capacity,
                                        gVProcInterposeBypassRegistry.count + 1,
                                        8,
                                        sizeof(*gVProcInterposeBypassRegistry.items),
                                        &new_capacity)) {
            pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
            return;
        }
        pthread_t *items = realloc(gVProcInterposeBypassRegistry.items, new_capacity * sizeof(*items));
        if (!items) {
            pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
            return;
        }
        gVProcInterposeBypassRegistry.items = items;
        gVProcInterposeBypassRegistry.capacity = new_capacity;
    }
    size_t idx = gVProcInterposeBypassRegistry.count++;
    gVProcInterposeBypassRegistry.items[idx] = tid;
    gVProcInterposeBypassRegistry.hint_valid = true;
    gVProcInterposeBypassRegistry.hint_tid = tid;
    gVProcInterposeBypassRegistry.hint_index = idx;
    pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
}

void vprocUnregisterInterposeBypassThread(pthread_t tid) {
    pthread_mutex_lock(&gVProcInterposeBypassRegistry.mu);
    size_t idx = 0;
    if (vprocInterposeBypassFindIndexLocked(tid, &idx)) {
        size_t last = gVProcInterposeBypassRegistry.count - 1;
        gVProcInterposeBypassRegistry.items[idx] = gVProcInterposeBypassRegistry.items[last];
        gVProcInterposeBypassRegistry.count--;
        gVProcInterposeBypassRegistry.hint_valid = false;
        gVProcInterposeBypassRegistry.hint_tid = 0;
        gVProcInterposeBypassRegistry.hint_index = 0;
        if (gVProcInterposeBypassRegistry.count == 0) {
            free(gVProcInterposeBypassRegistry.items);
            gVProcInterposeBypassRegistry.items = NULL;
            gVProcInterposeBypassRegistry.capacity = 0;
        }
    }
    pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
}

int vprocInterposeReady(void) {
    return gVProcInterposeReady != 0;
}

int vprocThreadHasActiveVproc(void) {
    if (!gVProcTlsReady) {
        return 0;
    }
    return gVProcCurrent != NULL;
}

void vprocInterposeBypassEnter(void) {
    gVProcInterposeBypassDepth++;
}

void vprocInterposeBypassExit(void) {
    if (gVProcInterposeBypassDepth > 0) {
        gVProcInterposeBypassDepth--;
    }
}

int vprocInterposeBypassActive(void) {
    if (!gVProcTlsReady) {
        return 0;
    }
    return gVProcInterposeBypassDepth > 0;
}

void vprocProtectKqueueCloseEnter(void) {
    __atomic_add_fetch(&gVProcProtectKqueueCloseDepth, 1, __ATOMIC_RELAXED);
}

void vprocProtectKqueueCloseExit(void) {
    sig_atomic_t depth = __atomic_load_n(&gVProcProtectKqueueCloseDepth, __ATOMIC_RELAXED);
    while (depth > 0) {
        if (__atomic_compare_exchange_n(&gVProcProtectKqueueCloseDepth,
                                        &depth,
                                        depth - 1,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            return;
        }
    }
}

int vprocProtectKqueueCloseActive(void) {
    return __atomic_load_n(&gVProcProtectKqueueCloseDepth, __ATOMIC_RELAXED) > 0;
}

enum {
    VPROC_FSTAB_MOUNT_RDONLY = 1ul << 0,
    VPROC_FSTAB_MOUNT_NOSUID = 1ul << 1,
    VPROC_FSTAB_MOUNT_NODEV = 1ul << 2,
    VPROC_FSTAB_MOUNT_NOEXEC = 1ul << 3,
    VPROC_FSTAB_MOUNT_REMOUNT = 1ul << 4,
    VPROC_FSTAB_MOUNT_BIND = 1ul << 5
};

static unsigned long vprocParseFstabMountFlags(const char *options) {
    unsigned long flags = 0;
    if (!options || options[0] == '\0') {
        return flags;
    }
    char *copy = strdup(options);
    if (!copy) {
        return flags;
    }
    char *saveptr = NULL;
    for (char *token = strtok_r(copy, ",", &saveptr);
         token;
         token = strtok_r(NULL, ",", &saveptr)) {
        while (*token && isspace((unsigned char)*token)) {
            token++;
        }
        size_t len = strlen(token);
        while (len > 0 && isspace((unsigned char)token[len - 1])) {
            token[--len] = '\0';
        }
        if (strcmp(token, "ro") == 0) flags |= VPROC_FSTAB_MOUNT_RDONLY;
        else if (strcmp(token, "rw") == 0) flags &= ~VPROC_FSTAB_MOUNT_RDONLY;
        else if (strcmp(token, "nosuid") == 0) flags |= VPROC_FSTAB_MOUNT_NOSUID;
        else if (strcmp(token, "suid") == 0) flags &= ~VPROC_FSTAB_MOUNT_NOSUID;
        else if (strcmp(token, "nodev") == 0) flags |= VPROC_FSTAB_MOUNT_NODEV;
        else if (strcmp(token, "dev") == 0) flags &= ~VPROC_FSTAB_MOUNT_NODEV;
        else if (strcmp(token, "noexec") == 0) flags |= VPROC_FSTAB_MOUNT_NOEXEC;
        else if (strcmp(token, "exec") == 0) flags &= ~VPROC_FSTAB_MOUNT_NOEXEC;
        else if (strcmp(token, "remount") == 0) flags |= VPROC_FSTAB_MOUNT_REMOUNT;
        else if (strcmp(token, "bind") == 0) flags |= VPROC_FSTAB_MOUNT_BIND;
    }
    free(copy);
    return flags;
}

static bool vprocDecodeFstabField(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0) {
        errno = EINVAL;
        return false;
    }
    size_t out_len = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)input[i];
        if (ch == '\\') {
            unsigned char next = (unsigned char)input[i + 1];
            if (next == '\0') {
                ch = '\\';
            } else if (next >= '0' && next <= '7') {
                int value = (int)(next - '0');
                i++;
                for (int digits = 1; digits < 3; ++digits) {
                    unsigned char oct = (unsigned char)input[i + 1];
                    if (oct < '0' || oct > '7') {
                        break;
                    }
                    i++;
                    value = (value * 8) + (int)(oct - '0');
                }
                ch = (unsigned char)value;
            } else {
                i++;
                switch (next) {
                    case 'n': ch = '\n'; break;
                    case 'r': ch = '\r'; break;
                    case 't': ch = '\t'; break;
                    default: ch = next; break;
                }
            }
        }
        if (out_len + 1 >= out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        out[out_len++] = (char)ch;
    }
    out[out_len] = '\0';
    return true;
}

#if defined(PSCAL_TARGET_IOS)
static int vprocEnsureMountSourceAccessFromRuntime(const char *path) {
    typedef int (*VprocEnsureMountSourceAccessFn)(const char *path);
    static VprocEnsureMountSourceAccessFn fn = NULL;
    static bool looked_up = false;
    if (!looked_up) {
        looked_up = true;
        fn = (VprocEnsureMountSourceAccessFn)dlsym(RTLD_DEFAULT, "pscalRuntimeEnsureMountSourceAccess");
    }
    if (!fn) {
        errno = ENOSYS;
        return -1;
    }
    return fn(path);
}
#endif

/* The folder an fstab label (UUID=...) names, from the runtime that made it. */
static bool vprocResolveFstabSourceLabel(const char *label, char *out, size_t out_size) {
    typedef char *(*VprocMountSourceForLabelFn)(const char *label);
    static VprocMountSourceForLabelFn fn = NULL;
    static bool looked_up = false;
    if (!looked_up) {
        looked_up = true;
        fn = (VprocMountSourceForLabelFn)dlsym(RTLD_DEFAULT, "pscalRuntimeMountSourceForLabel");
    }
    char *path = fn ? fn(label) : NULL;
    if (!path) {
        errno = fn ? ENOENT : ENOSYS;
        return false;
    }
    bool ok = strlen(path) < out_size;
    if (ok) {
        memcpy(out, path, strlen(path) + 1);
    } else {
        errno = ENAMETOOLONG;
    }
    free(path);
    return ok;
}

static bool vprocResolveFstabSourceDir(const char *source, char *source_real, size_t source_real_size) {
    char labelled[PATH_MAX];
    if (source && strncmp(source, "UUID=", 5) == 0) {
        if (!vprocResolveFstabSourceLabel(source, labelled, sizeof(labelled))) {
            return false;
        }
        source = labelled;
    }
    if (!source || source[0] != '/' || !source_real || source_real_size == 0) {
        errno = EINVAL;
        return false;
    }
    /* The app's own path first: mount -p writes a source in the app's storage that way, and the
     * same name can exist on the host. A host path (an fstab from before) is the fallback. */
    char expanded[PATH_MAX];
    const char *candidates[2] = { NULL, source };
    if (pathTruncateExpand(source, expanded, sizeof(expanded))) {
        candidates[0] = expanded;
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *candidate = candidates[i];
        if (!candidate) {
            continue;
        }
#if defined(PSCAL_TARGET_IOS)
        (void)vprocEnsureMountSourceAccessFromRuntime(candidate);
#endif
        char real_buf[PATH_MAX];
        if (!vprocHostRealpathRaw(candidate, real_buf)) {
            continue;
        }
        struct stat st;
        if (vprocHostStatRaw(real_buf, &st) != 0) {
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            continue;
        }
        size_t len = strlen(real_buf);
        if (len >= source_real_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(source_real, real_buf, len + 1);
        return true;
    }
    if (errno == 0) {
        errno = ENOENT;
    }
    return false;
}

static bool vprocResolveFstabTargetDir(const char *target, char *target_virtual, size_t target_virtual_size) {
    if (!target || target[0] != '/' || !target_virtual || target_virtual_size == 0) {
        errno = EINVAL;
        return false;
    }
    char expanded[PATH_MAX];
    if (!pathTruncateExpand(target, expanded, sizeof(expanded))) {
        return false;
    }
    char target_real[PATH_MAX];
    if (!vprocHostRealpathRaw(expanded, target_real)) {
        return false;
    }
    struct stat st;
    if (vprocHostStatRaw(target_real, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    if (!pathTruncateStrip(target_real, target_virtual, target_virtual_size)) {
        return false;
    }
    if (target_virtual[0] != '/') {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool vprocApplyFstabMountLine(char *line, size_t line_no) {
    if (!line) {
        return false;
    }
    char *comment = strchr(line, '#');
    if (comment) {
        *comment = '\0';
    }

    char *cursor = line;
    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0') {
        return true;
    }

    char *fields[6] = {0};
    size_t field_count = 0;
    char *saveptr = NULL;
    for (char *token = strtok_r(cursor, " \t\r\n", &saveptr);
         token;
         token = strtok_r(NULL, " \t\r\n", &saveptr)) {
        if (field_count < sizeof(fields) / sizeof(fields[0])) {
            fields[field_count++] = token;
        }
    }

    if (field_count < 2) {
        vprocDebugLogf("[vproc-fstab] line %zu ignored (needs source and target)", line_no);
        return false;
    }

    char source_field[PATH_MAX];
    char target_field[PATH_MAX];
    char type_field[sizeof(((PathTruncateMountEntry *)0)->type)];
    char options_field[sizeof(((PathTruncateMountEntry *)0)->options)];

    if (!vprocDecodeFstabField(fields[0], source_field, sizeof(source_field)) ||
        !vprocDecodeFstabField(fields[1], target_field, sizeof(target_field))) {
        int saved_errno = errno;
        vprocDebugLogf("[vproc-fstab] line %zu field decode failed: %s",
                       line_no,
                       strerror(saved_errno ? saved_errno : EINVAL));
        errno = saved_errno;
        return false;
    }

    if (field_count >= 3 && fields[2][0] != '\0' && strcmp(fields[2], "-") != 0) {
        if (!vprocDecodeFstabField(fields[2], type_field, sizeof(type_field))) {
            int saved_errno = errno;
            vprocDebugLogf("[vproc-fstab] line %zu type decode failed: %s",
                           line_no,
                           strerror(saved_errno ? saved_errno : EINVAL));
            errno = saved_errno;
            return false;
        }
    } else {
        strlcpy(type_field, "bind", sizeof(type_field));
    }

    if (field_count >= 4 && fields[3][0] != '\0' && strcmp(fields[3], "-") != 0) {
        if (!vprocDecodeFstabField(fields[3], options_field, sizeof(options_field))) {
            int saved_errno = errno;
            vprocDebugLogf("[vproc-fstab] line %zu options decode failed: %s",
                           line_no,
                           strerror(saved_errno ? saved_errno : EINVAL));
            errno = saved_errno;
            return false;
        }
    } else {
        strlcpy(options_field, "rw", sizeof(options_field));
    }

    const char *source = source_field;
    const char *target = target_field;
    const char *type = type_field;
    const char *options = options_field;
    if (type[0] == '\0' || strcmp(type, "-") == 0) {
        type = "bind";
    }
    if (options[0] == '\0' || strcmp(options, "-") == 0) {
        options = "rw";
    }

    unsigned long flags = vprocParseFstabMountFlags(options);
    if (strcmp(type, "bind") == 0) {
        flags |= VPROC_FSTAB_MOUNT_BIND;
    }

    char source_real[PATH_MAX];
    if (!vprocResolveFstabSourceDir(source, source_real, sizeof(source_real))) {
        int saved_errno = errno;
        vprocDebugLogf("[vproc-fstab] line %zu source %s rejected: %s",
                       line_no,
                       source,
                       strerror(saved_errno ? saved_errno : EINVAL));
        errno = saved_errno;
        return false;
    }

    char target_virtual[PATH_MAX];
    if (!vprocResolveFstabTargetDir(target, target_virtual, sizeof(target_virtual))) {
        int saved_errno = errno;
        vprocDebugLogf("[vproc-fstab] line %zu target %s rejected: %s",
                       line_no,
                       target,
                       strerror(saved_errno ? saved_errno : EINVAL));
        errno = saved_errno;
        return false;
    }

    if (!pathTruncateMountAdd(source_real, target_virtual, type, options, flags)) {
        int saved_errno = errno;
        vprocDebugLogf("[vproc-fstab] line %zu mount %s -> %s failed: %s",
                       line_no,
                       source_real,
                       target_virtual,
                       strerror(saved_errno ? saved_errno : EINVAL));
        errno = saved_errno;
        return false;
    }

    return true;
}

static void vprocEnsureStartupFstabMounts(void) {
    if (__atomic_load_n(&gFstabMountsLoaded, __ATOMIC_ACQUIRE)) {
        return;
    }

    pthread_mutex_lock(&gFstabMountsMu);
    if (gFstabMountsLoaded) {
        pthread_mutex_unlock(&gFstabMountsMu);
        return;
    }

    int fd = -1;
    char fstab_path[PATH_MAX] = "/etc/fstab";
    char root_path[PATH_MAX];
    if (pathTruncateExpand("/", root_path, sizeof(root_path))) {
        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/etc/fstab", root_path);
        if (n > 0 && n < (int)sizeof(candidate)) {
            fd = vprocHostOpenRawInternal(candidate, O_RDONLY, 0, false);
            if (fd >= 0) {
                strlcpy(fstab_path, candidate, sizeof(fstab_path));
            }
        }
    }
    if (fd < 0) {
        fd = vprocHostOpenVirtualized("/etc/fstab", O_RDONLY, 0);
    }
    if (fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR) {
            vprocDebugLogf("[vproc-fstab] open %s failed: %s", fstab_path, strerror(errno));
        }
        __atomic_store_n(&gFstabMountsLoaded, true, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gFstabMountsMu);
        return;
    }

    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        int saved_errno = errno;
        close(fd);
        vprocDebugLogf("[vproc-fstab] fdopen %s failed: %s",
                       fstab_path,
                       strerror(saved_errno ? saved_errno : EINVAL));
        __atomic_store_n(&gFstabMountsLoaded, true, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gFstabMountsMu);
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    size_t line_no = 0;
    while (getline(&line, &cap, fp) != -1) {
        line_no++;
        (void)vprocApplyFstabMountLine(line, line_no);
    }
    free(line);
    fclose(fp);

    __atomic_store_n(&gFstabMountsLoaded, true, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&gFstabMountsMu);
}

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void vprocResetStartupFstabStateForTests(void) {
    pthread_mutex_lock(&gFstabMountsMu);
    __atomic_store_n(&gFstabMountsLoaded, false, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&gFstabMountsMu);
}
#endif

static void vprocEnsurePathTruncationDefault(void) {
    if (__atomic_load_n(&gPathTruncateInit, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&gPathTruncateMu);
    if (gPathTruncateInit) {
        pthread_mutex_unlock(&gPathTruncateMu);
        return;
    }
    __atomic_store_n(&gPathTruncateInit, true, __ATOMIC_RELEASE);
    const char *disabled = getenv("PSCALI_PATH_TRUNCATE_DISABLED");
    if (disabled && disabled[0] != '\0') {
        pthread_mutex_unlock(&gPathTruncateMu);
        return;
    }
    const char *container_root = getenv("PSCALI_CONTAINER_ROOT");
    size_t container_root_len = (container_root && container_root[0] != '\0')
            ? strlen(container_root)
            : 0;
    const char *raw_prefix = getenv("PATH_TRUNCATE");
    if (!raw_prefix || raw_prefix[0] == '\0') {
        raw_prefix = container_root;
    }
    if (!raw_prefix || raw_prefix[0] != '/') {
        raw_prefix = getenv("HOME");
    }
    if (!raw_prefix || raw_prefix[0] != '/') {
        pthread_mutex_unlock(&gPathTruncateMu);
        return;
    }
    char prefbuf[PATH_MAX];
    const char *prefix = raw_prefix;
    /* On iOS, most runtime assets live under <container>/Documents. When PATH
     * truncation is anchored at the container root, /bin expands to
     * <container>/bin which does not exist. Prefer the Documents root so /bin
     * maps to <container>/Documents/bin where exsh assets are staged. */
    if (container_root_len > 0 && !pathTruncatePrefixIsRoot(raw_prefix) &&
        strncmp(raw_prefix, container_root, container_root_len) == 0) {
        size_t n = snprintf(prefbuf, sizeof(prefbuf), "%s/Documents", raw_prefix);
        if (n > 0 && n < sizeof(prefbuf)) {
            prefix = prefbuf;
        }
    }
    pathTruncateApplyEnvironment(prefix);
    pthread_mutex_unlock(&gPathTruncateMu);
}

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void vprocRunPathTruncationDefaultForTests(void) {
    __atomic_store_n(&gPathTruncateInit, false, __ATOMIC_RELEASE);
    vprocEnsurePathTruncationDefault();
}
#endif

void vprocApplyPathTruncation(const char *prefix) {
    pthread_mutex_lock(&gPathTruncateMu);
    __atomic_store_n(&gPathTruncateInit, true, __ATOMIC_RELEASE);
    if (prefix && prefix[0] == '/') {
        unsetenv("PSCALI_PATH_TRUNCATE_DISABLED");
        const char *container_root = getenv("PSCALI_CONTAINER_ROOT");
        size_t container_root_len = (container_root && container_root[0] != '\0')
                ? strlen(container_root)
                : 0;
        char prefbuf[PATH_MAX];
        const char *use_prefix = prefix;
        if (container_root_len > 0 && !pathTruncatePrefixIsRoot(prefix) &&
            strncmp(prefix, container_root, container_root_len) == 0) {
            size_t n = snprintf(prefbuf, sizeof(prefbuf), "%s/Documents", prefix);
            if (n > 0 && n < sizeof(prefbuf)) {
                use_prefix = prefbuf;
            }
        }
        pathTruncateApplyEnvironment(use_prefix);
    } else {
        setenv("PSCALI_PATH_TRUNCATE_DISABLED", "1", 1);
        pathTruncateApplyEnvironment(NULL);
    }
    pthread_mutex_unlock(&gPathTruncateMu);
}

void vprocSetShellSelfTid(pthread_t tid) {
    gShellSelfTid = tid;
    gShellSelfTidValid = true;
}

bool vprocIsShellSelfThread(void) {
    return gShellSelfTidValid && pthread_equal(pthread_self(), gShellSelfTid);
}

static void *vprocKernelThreadMain(void *arg) {
    int pid = (int)(intptr_t)arg;
    sigset_t mask;
    sigfillset(&mask);
    vprocHostPthreadSigmaskRaw(SIG_BLOCK, &mask, NULL);
    vprocRegisterInterposeBypassThread(pthread_self());
    if (pid > 0) {
        vprocSetKernelPid(pid);
        vprocRegisterTidHint(pid, pthread_self());
    }
#if defined(__APPLE__)
    pthread_setname_np("kernel");
#else
    pthread_setname_np(pthread_self(), "kernel");
#endif
    pthread_mutex_lock(&gKernelThreadMu);
    gKernelThreadReady = true;
    pthread_cond_broadcast(&gKernelThreadCv);
    pthread_mutex_unlock(&gKernelThreadMu);
    for (;;) {
        VProcKernelControlEvent ev;
        bool have_event = false;
        pthread_mutex_lock(&gKernelThreadMu);
        while (gKernelControlQueueCount == 0) {
            pthread_cond_wait(&gKernelThreadCv, &gKernelThreadMu);
        }
        if (gKernelControlQueue && gKernelControlQueueCount > 0 &&
            gKernelControlQueueHead < gKernelControlQueueCap) {
            ev = gKernelControlQueue[gKernelControlQueueHead];
            gKernelControlQueueHead = (gKernelControlQueueHead + 1) % gKernelControlQueueCap;
            gKernelControlQueueCount--;
            have_event = true;
        }
        pthread_mutex_unlock(&gKernelThreadMu);

        if (!have_event) {
            continue;
        }

        switch (ev.type) {
            case VPROC_KERNEL_EVENT_CONTROL_SIGNAL:
                if (ev.sig == SIGINT || ev.sig == SIGTSTP) {
                    (void)vprocDispatchControlSignalToForeground(ev.shell_pid,
                                                                 ev.sig,
                                                                 ev.allow_runtime_fallback);
                }
                break;
            case VPROC_KERNEL_EVENT_CONTINUE_PGID:
                if (ev.pgid > 0) {
                    (void)vprocKillShim(-ev.pgid, SIGCONT);
                }
                break;
            case VPROC_KERNEL_EVENT_SIGCHLD_AGGREGATE:
                if (ev.target_pid > 0) {
                    pthread_mutex_lock(&gVProcTasks.mu);
                    VProcTaskEntry *entry = vprocTaskFindLocked(ev.target_pid);
                    if (entry) {
                        entry->sigchld_delivery_queued = false;
                        if (!entry->sigchld_blocked) {
                            (void)vprocDeliverPendingSignalsLocked(entry);
                        }
                    }
                    pthread_mutex_unlock(&gVProcTasks.mu);
                }
                break;
            default:
                break;
        }
    }
    return NULL;
}

static void vprocWaitForKernelThreadReady(void) {
    pthread_mutex_lock(&gKernelThreadMu);
    if (!gKernelThreadReady) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
            deadline.tv_sec += 2;
            while (!gKernelThreadReady) {
                if (pthread_cond_timedwait(&gKernelThreadCv, &gKernelThreadMu, &deadline) == ETIMEDOUT) {
                    break;
                }
            }
        } else {
            while (!gKernelThreadReady) {
                pthread_cond_wait(&gKernelThreadCv, &gKernelThreadMu);
            }
        }
    }
    pthread_mutex_unlock(&gKernelThreadMu);
}

static void vprocEnsureKernelThread(int pid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gKernelThreadMu);
    if (gKernelThreadStarted) {
        pthread_mutex_unlock(&gKernelThreadMu);
        vprocWaitForKernelThreadReady();
        return;
    }
    gKernelThreadStarted = true;
    gKernelThreadReady = false;
    pthread_mutex_unlock(&gKernelThreadMu);

    pthread_t tid;
    int rc = vprocHostPthreadCreateRaw(&tid, NULL, vprocKernelThreadMain, (void *)(intptr_t)pid);
    if (rc != 0) {
        pthread_mutex_lock(&gKernelThreadMu);
        gKernelThreadStarted = false;
        gKernelThreadReady = false;
        pthread_mutex_unlock(&gKernelThreadMu);
        return;
    }
    pthread_detach(tid);
    pthread_mutex_lock(&gKernelThreadMu);
    gKernelThread = tid;
    pthread_mutex_unlock(&gKernelThreadMu);
    vprocWaitForKernelThreadReady();
}

static bool vprocKernelQueueEventInternal(const VProcKernelControlEvent *event, bool ensure_kernel) {
    if (!event) {
        return false;
    }
    if (ensure_kernel && vprocEnsureKernelPid() <= 0) {
        return false;
    }
    bool queued = false;
    pthread_mutex_lock(&gKernelThreadMu);
    if (gKernelThreadStarted && gKernelThreadReady) {
        if (gKernelControlQueueCount >= gKernelControlQueueCap) {
            size_t new_cap = 0;
            if (vprocComputeGrowthCapacity(gKernelControlQueueCap,
                                           gKernelControlQueueCount + 1,
                                           16,
                                           sizeof(VProcKernelControlEvent),
                                           &new_cap)) {
                VProcKernelControlEvent *resized =
                    (VProcKernelControlEvent *)calloc(new_cap, sizeof(VProcKernelControlEvent));
                if (resized) {
                    for (size_t i = 0; i < gKernelControlQueueCount; ++i) {
                        size_t src = (gKernelControlQueueHead + i) % gKernelControlQueueCap;
                        resized[i] = gKernelControlQueue[src];
                    }
                    free(gKernelControlQueue);
                    gKernelControlQueue = resized;
                    gKernelControlQueueCap = new_cap;
                    gKernelControlQueueHead = 0;
                }
            }
        }
        if (gKernelControlQueue && gKernelControlQueueCount < gKernelControlQueueCap) {
            size_t tail = (gKernelControlQueueHead + gKernelControlQueueCount) % gKernelControlQueueCap;
            gKernelControlQueue[tail] = *event;
            gKernelControlQueueCount++;
            queued = true;
            pthread_cond_signal(&gKernelThreadCv);
        }
    }
    pthread_mutex_unlock(&gKernelThreadMu);
    return queued;
}

static bool vprocRequestControlSignalForShellInternal(int shell_pid,
                                                      int sig,
                                                      bool allow_runtime_fallback) {
    if (vprocSoftSignalingDisabledByEnv()) {
        return false;
    }
    if (sig != SIGINT && sig != SIGTSTP) {
        return false;
    }
    if (shell_pid <= 0) {
        int shell_pgid = -1;
        int sid = -1;
        int fg_pgid = -1;
        (void)vprocGetShellJobControlState(&shell_pid, &shell_pgid, &sid, &fg_pgid);
    }
    return vprocDispatchControlSignalToForeground(shell_pid, sig, allow_runtime_fallback);
}

bool vprocRequestControlSignalForShell(int shell_pid, int sig) {
    return vprocRequestControlSignalForShellInternal(shell_pid, sig, false);
}

bool vprocRequestControlSignal(int sig) {
    int shell_pid = vprocGetShellSelfPid();
    return vprocRequestControlSignalForShellInternal(shell_pid, sig, false);
}

static bool vprocRequestControlSignalForSessionInternal(uint64_t session_id,
                                                        int sig,
                                                        bool allow_runtime_fallback) {
    if (session_id == 0 || (sig != SIGINT && sig != SIGTSTP)) {
        return false;
    }
    if (vprocSoftSignalingDisabledByEnv()) {
        return false;
    }

    int shell_pid = 0;
    int sid = -1;
    int fg_pgid = -1;
    bool session_passthrough = false;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *session_entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (session_entry) {
        shell_pid = session_entry->shell_pid;
        session_passthrough = session_entry->control_bytes_passthrough;
    }
    if (session_entry && session_entry->pty_slave && session_entry->pty_slave->tty) {
        struct tty *tty = session_entry->pty_slave->tty;
        lock(&tty->lock);
        sid = (int)tty->session;
        fg_pgid = (int)tty->fg_group;
        unlock(&tty->lock);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);

    if (session_passthrough) {
        return false;
    }

    if (sid > 0) {
        int mapped_fg = vprocGetForegroundPgid(sid);
        if (mapped_fg > 0) {
            fg_pgid = mapped_fg;
        }
    }

    if (vprocForegroundGroupPrefersControlBytes(sid, fg_pgid, shell_pid, sig)) {
        return false;
    }

    if (shell_pid <= 0 && sid > 0) {
        /* Legacy fallback: session leaders are commonly pid==sid in this VM. */
        shell_pid = sid;
    }

    if (shell_pid > 0 &&
        vprocRequestControlSignalForShellInternal(shell_pid, sig, allow_runtime_fallback)) {
        return true;
    }

    /* Only in this session's own group: tty->session and the foreground group read
     * from it can be another tab's. See vprocTerminateSessionById. */
    if (fg_pgid > 0 && (shell_pid <= 0 || vprocGetSid(shell_pid) == sid)) {
        if (vprocKillLive(-fg_pgid, sig) == 0) {
            return true;
        }
    }

    return false;
}

bool vprocRequestControlSignalForSession(uint64_t session_id, int sig) {
    return vprocRequestControlSignalForSessionInternal(session_id, sig, false);
}

static bool vprocKernelQueueContinuePgid(int pgid) {
    if (pgid <= 0) {
        return false;
    }
    VProcKernelControlEvent event;
    memset(&event, 0, sizeof(event));
    event.type = VPROC_KERNEL_EVENT_CONTINUE_PGID;
    event.pgid = pgid;
    return vprocKernelQueueEventInternal(&event, true);
}

static bool vprocKernelQueueSigchldAggregate(int target_pid, bool ensure_kernel) {
    if (target_pid <= 0) {
        return false;
    }
    VProcKernelControlEvent event;
    memset(&event, 0, sizeof(event));
    event.type = VPROC_KERNEL_EVENT_SIGCHLD_AGGREGATE;
    event.target_pid = target_pid;
    return vprocKernelQueueEventInternal(&event, ensure_kernel);
}

void vprocSetKernelPid(int pid) {
    gKernelPid = pid;
    if (pid > 0) {
        gKernelPidGlobal = pid;
    }
}

int vprocGetKernelPid(void) {
    return (gKernelPid > 0) ? gKernelPid : gKernelPidGlobal;
}

void vprocClearKernelPidGlobal(void) {
    gKernelPidGlobal = 0;
}

int vprocGetSessionKernelPid(void) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    return session ? session->kernel_pid : 0;
}

void vprocSetSessionKernelPid(int pid) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session) {
        session->kernel_pid = pid;
    }
}

int vprocEnsureKernelPid(void) {
    bool created = false;
    vprocEnsurePathTruncationDefault();
    pthread_mutex_lock(&gKernelVprocMu);
    int pid = vprocGetKernelPid();
    if (pid > 0) {
        pthread_mutex_unlock(&gKernelVprocMu);
        vprocEnsureKernelThread(pid);
        vprocEnsureStartupFstabMounts();
        return pid;
    }

    VProcOptions kopts = vprocDefaultOptions();
    kopts.stdin_fd = -2;
    kopts.stdout_fd = -2;
    kopts.stderr_fd = -2;
    int kpid_hint = vprocReservePid();
    if (kpid_hint > 0) {
        kopts.pid_hint = kpid_hint;
    }
    gKernelVproc = vprocCreate(&kopts);
    if (gKernelVproc) {
        pid = vprocPid(gKernelVproc);
    } else if (kpid_hint > 0) {
        pid = kpid_hint;
    }
    if (pid > 0) {
        vprocSetKernelPid(pid);
        vprocSetParent(pid, 0);
        (void)vprocSetSid(pid, pid);
        vprocSetCommandLabel(pid, "kernel");
        created = true;
    }
    pthread_mutex_unlock(&gKernelVprocMu);
    vprocEnsureKernelThread(pid);
    if (created && pid > 0) {
        pthread_mutex_lock(&gVProcTasks.mu);
        /* The kernel is root's, as init is, to kill's check. Its vproc keeps the login account's
         * ids, so nothing made under it starts as root. */
        VProcTaskEntry *kernel_entry = vprocTaskFindLocked(pid);
        if (kernel_entry) {
            kernel_entry->cred_ruid = kernel_entry->cred_euid = kernel_entry->cred_suid = 0;
        }
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0) {
                continue;
            }
            if (entry->pid == pid) {
                continue;
            }
            if (entry->parent_pid != pid) {
                vprocUpdateParentLocked(entry->pid, pid);
            }
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
    }
    if (pid > 0) {
        vprocEnsureStartupFstabMounts();
    }
    return pid;
}

static void vprocSessionStdioInitDefaultOnce(void) {
    vprocSessionStdioInit(&gSessionStdioDefault, vprocGetKernelPid());
}

VProcSessionStdio *vprocSessionStdioCurrent(void) {
    if (gSessionStdioTls) {
        return gSessionStdioTls;
    }
    pthread_once(&gSessionStdioDefaultOnce, vprocSessionStdioInitDefaultOnce);
    return &gSessionStdioDefault;
}

static int vprocSessionHostFdForStd(int std_fd);
static bool vprocSessionStdioMatchFd(int session_fd, int std_fd);

void vprocSessionStdioInit(VProcSessionStdio *stdio_ctx, int kernel_pid) {
    if (!stdio_ctx) {
        return;
    }
    VProcSessionInput *input = stdio_ctx->input;
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->shell_pid = 0;
    /* Duplicate current host stdio so this session owns stable copies. */
    int host_in = vprocSessionHostFdForStd(STDIN_FILENO);
    int host_out = vprocSessionHostFdForStd(STDOUT_FILENO);
    int host_err = vprocSessionHostFdForStd(STDERR_FILENO);
    int in = (host_in >= 0) ? fcntl(host_in, F_DUPFD_CLOEXEC, 0) : -1;
    int out = (host_out >= 0) ? fcntl(host_out, F_DUPFD_CLOEXEC, 0) : -1;
    int err = (host_err >= 0) ? fcntl(host_err, F_DUPFD_CLOEXEC, 0) : -1;
    if (in < 0 && errno == EINVAL && host_in >= 0) in = vprocHostDupRaw(host_in);
    if (out < 0 && errno == EINVAL && host_out >= 0) out = vprocHostDupRaw(host_out);
    if (err < 0 && errno == EINVAL && host_err >= 0) err = vprocHostDupRaw(host_err);
    if (in >= 0) fcntl(in, F_SETFD, FD_CLOEXEC);
    if (out >= 0) fcntl(out, F_SETFD, FD_CLOEXEC);
    if (err >= 0) fcntl(err, F_SETFD, FD_CLOEXEC);
    stdio_ctx->stdin_host_fd = in;
    stdio_ctx->stdout_host_fd = out;
    stdio_ctx->stderr_host_fd = err;
    stdio_ctx->input = input;
    stdio_ctx->stdin_pscal_fd = NULL;
    stdio_ctx->stdout_pscal_fd = NULL;
    stdio_ctx->stderr_pscal_fd = NULL;
    stdio_ctx->pty_master = NULL;
    stdio_ctx->pty_slave = NULL;
    stdio_ctx->pty_out_thread = 0;
    stdio_ctx->pty_active = false;
    stdio_ctx->session_id = 0;
}

void vprocSessionStdioActivate(VProcSessionStdio *stdio_ctx) {
    gSessionStdioTls = stdio_ctx ? stdio_ctx : &gSessionStdioDefault;
    vprocIoTrace("[vproc-io] activate stdio=%p session=%llu pty_active=%d host=(%d,%d,%d) pscal=(%p,%p,%p)",
                 (void *)gSessionStdioTls,
                 (unsigned long long)(gSessionStdioTls ? gSessionStdioTls->session_id : 0),
                 (gSessionStdioTls && gSessionStdioTls->pty_active) ? 1 : 0,
                 gSessionStdioTls ? gSessionStdioTls->stdin_host_fd : -1,
                 gSessionStdioTls ? gSessionStdioTls->stdout_host_fd : -1,
                 gSessionStdioTls ? gSessionStdioTls->stderr_host_fd : -1,
                 gSessionStdioTls ? (void *)gSessionStdioTls->stdin_pscal_fd : NULL,
                 gSessionStdioTls ? (void *)gSessionStdioTls->stdout_pscal_fd : NULL,
                 gSessionStdioTls ? (void *)gSessionStdioTls->stderr_pscal_fd : NULL);
    /* Ensure the session PTY is marked as the controlling terminal for this
     * session so /dev/tty resolves for non-leader vprocs (pipeline stages). Only
     * for a thread that can say whose session that is. A tab's first thread can't
     * yet: it would give the terminal to the host's session, or to the shell of
     * the last tab, and the shell's own TIOCSCTTY would then be refused. */
    if (gSessionStdioTls && gSessionStdioTls->pty_slave && gSessionStdioTls->pty_slave->tty &&
        (vprocForThread() || vprocSessionShellPidHint(gSessionStdioTls) > 0)) {
        struct tty *tty = gSessionStdioTls->pty_slave->tty;
        /* With no task on this thread, getsid(0) answers for whoever the process last
         * called the shell, which can be another tab's: ask for this tab's shell's. */
        const int hint = vprocSessionShellPidHint(gSessionStdioTls);
        int sid = vprocForThread() ? pscalTtyCurrentSid() : vprocGetSid(hint);
        if (sid > 0) {
            const int recorded = vprocGetForegroundPgid(sid);
            lock(&tty->lock);
            if (tty->session == 0) {
                tty->session = (pid_t_)sid;
                tty->fg_group = (pid_t_)(recorded > 0 ? recorded : sid);
            }
            unlock(&tty->lock);
            pscalTtySetControlling(tty);
        }
    }
    if (!gSessionStdioTls || vprocSessionStdioIsDefault(gSessionStdioTls)) {
        gShellSelfPid = 0;
    } else {
        int shell_pid = vprocSessionShellPidHint(gSessionStdioTls);
        gShellSelfPid = (shell_pid > 0) ? shell_pid : 0;
    }
}

void vprocSessionStdioInitWithFds(VProcSessionStdio *stdio_ctx,
                                  int stdin_fd,
                                  int stdout_fd,
                                  int stderr_fd,
                                  int kernel_pid) {
    if (!stdio_ctx) {
        return;
    }
    VProcSessionInput *input = stdio_ctx->input;
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->shell_pid = 0;
    int in = (stdin_fd >= 0) ? fcntl(stdin_fd, F_DUPFD_CLOEXEC, 0) : -1;
    int out = (stdout_fd >= 0) ? fcntl(stdout_fd, F_DUPFD_CLOEXEC, 0) : -1;
    int err = (stderr_fd >= 0) ? fcntl(stderr_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (in < 0 && errno == EINVAL && stdin_fd >= 0) in = vprocHostDupRaw(stdin_fd);
    if (out < 0 && errno == EINVAL && stdout_fd >= 0) out = vprocHostDupRaw(stdout_fd);
    if (err < 0 && errno == EINVAL && stderr_fd >= 0) err = vprocHostDupRaw(stderr_fd);
    if (in >= 0) fcntl(in, F_SETFD, FD_CLOEXEC);
    if (out >= 0) fcntl(out, F_SETFD, FD_CLOEXEC);
    if (err >= 0) fcntl(err, F_SETFD, FD_CLOEXEC);
    stdio_ctx->stdin_host_fd = in;
    stdio_ctx->stdout_host_fd = out;
    stdio_ctx->stderr_host_fd = err;
    stdio_ctx->input = input;
    stdio_ctx->stdin_pscal_fd = NULL;
    stdio_ctx->stdout_pscal_fd = NULL;
    stdio_ctx->stderr_pscal_fd = NULL;
    stdio_ctx->pty_master = NULL;
    stdio_ctx->pty_slave = NULL;
    stdio_ctx->pty_out_thread = 0;
    stdio_ctx->pty_active = false;
    stdio_ctx->control_bytes_passthrough = false;
    stdio_ctx->session_id = 0;
}

int vprocSessionStdioInitWithPty(VProcSessionStdio *stdio_ctx,
                                 struct pscal_fd *pty_slave,
                                 struct pscal_fd *pty_master,
                                 uint64_t session_id,
                                 int kernel_pid) {
    if (!stdio_ctx || !pty_slave || !pty_master) {
        errno = EINVAL;
        return -1;
    }
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->shell_pid = 0;
    stdio_ctx->session_id = session_id;
    stdio_ctx->stdin_host_fd = -1;
    stdio_ctx->stdout_host_fd = -1;
    stdio_ctx->stderr_host_fd = -1;
    stdio_ctx->input = NULL;

    stdio_ctx->pty_master = pty_master;
    stdio_ctx->pty_slave = pty_slave;
    stdio_ctx->stdin_pscal_fd = pty_slave;
    stdio_ctx->stdout_pscal_fd = pscal_fd_retain(pty_slave);
    stdio_ctx->stderr_pscal_fd = pscal_fd_retain(pty_slave);
    stdio_ctx->pty_active = true;
    stdio_ctx->control_bytes_passthrough = false;
    pscalTtySetSessionReader(pty_slave);
    vprocIoTrace("[vproc-io] stdio init session=%llu master=%p slave=%p",
                 (unsigned long long)session_id,
                 (void *)pty_master,
                 (void *)pty_slave);
    vprocPtyTrace("[PTY] init session=%llu master=%p slave=%p",
                  (unsigned long long)session_id,
                  (void *)pty_master,
                  (void *)pty_slave);

    int rc = 0;
    rc = vprocHostPthreadCreate(&stdio_ctx->pty_out_thread, NULL, vprocSessionPtyOutputThread, stdio_ctx);
    if (rc != 0) {
        vprocPtyTrace("[PTY] output thread create failed rc=%d", rc);
        stdio_ctx->pty_active = false;
        if (stdio_ctx->stdout_pscal_fd) {
            pscal_fd_close(stdio_ctx->stdout_pscal_fd);
            stdio_ctx->stdout_pscal_fd = NULL;
        }
        if (stdio_ctx->stderr_pscal_fd) {
            pscal_fd_close(stdio_ctx->stderr_pscal_fd);
            stdio_ctx->stderr_pscal_fd = NULL;
        }
        stdio_ctx->stdin_pscal_fd = NULL;
        stdio_ctx->pty_master = NULL;
        stdio_ctx->pty_slave = NULL;
        errno = rc;
        return -1;
    }

    vprocSessionPtyRegister(session_id, pty_slave, pty_master);
    return 0;
}

VProcSessionStdio *vprocSessionStdioCreate(void) {
    VProcSessionStdio *session = (VProcSessionStdio *)calloc(1, sizeof(VProcSessionStdio));
    if (!session) {
        return NULL;
    }
    session->stdin_host_fd = -1;
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    session->kernel_pid = 0;
    session->shell_pid = 0;
    session->input = NULL;
    session->stdin_pscal_fd = NULL;
    session->stdout_pscal_fd = NULL;
    session->stderr_pscal_fd = NULL;
    session->pty_master = NULL;
    session->pty_slave = NULL;
    session->pty_out_thread = 0;
    session->pty_active = false;
    session->control_bytes_passthrough = false;
    session->session_id = 0;
    return session;
}

void vprocSessionStdioDestroy(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return;
    }
    if (gSessionStdioTls == stdio_ctx) {
        gSessionStdioTls = &gSessionStdioDefault;
    }
    if (stdio_ctx->session_id) {
        vprocSessionPtyUnregister(stdio_ctx->session_id);
    }
    if (stdio_ctx->pty_master && stdio_ctx->pty_master->tty) {
        lock(&stdio_ctx->pty_master->tty->lock);
        tty_hangup(stdio_ctx->pty_master->tty);
        unlock(&stdio_ctx->pty_master->tty->lock);
    }
    stdio_ctx->pty_active = false;
    if (stdio_ctx->pty_out_thread) {
        pthread_join(stdio_ctx->pty_out_thread, NULL);
        stdio_ctx->pty_out_thread = 0;
    }
    VProcSessionInput *input = stdio_ctx->input;
    if (input) {
        pthread_mutex_lock(&input->mu);
        input->stop_requested = true;
        input->off = 0;
        input->len = 0;
        input->eof_mark_count = 0;
        input->eof = true;
        input->interrupt_pending = false;
        input->interrupt_key = false;
        pthread_cond_broadcast(&input->cv);
        pthread_mutex_unlock(&input->mu);
    }
    if (stdio_ctx->stdin_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdin_host_fd);
    }
    if (stdio_ctx->stdout_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdout_host_fd);
    }
    if (stdio_ctx->stderr_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stderr_host_fd);
    }
    stdio_ctx->stdin_host_fd = -1;
    stdio_ctx->stdout_host_fd = -1;
    stdio_ctx->stderr_host_fd = -1;
    if (stdio_ctx->stdout_pscal_fd) {
        pscal_fd_close(stdio_ctx->stdout_pscal_fd);
        stdio_ctx->stdout_pscal_fd = NULL;
    }
    if (stdio_ctx->stderr_pscal_fd) {
        pscal_fd_close(stdio_ctx->stderr_pscal_fd);
        stdio_ctx->stderr_pscal_fd = NULL;
    }
    if (stdio_ctx->stdin_pscal_fd) {
        pscal_fd_close(stdio_ctx->stdin_pscal_fd);
        stdio_ctx->stdin_pscal_fd = NULL;
    }
    stdio_ctx->pty_slave = NULL;
    if (stdio_ctx->pty_master) {
        pscal_fd_close(stdio_ctx->pty_master);
        stdio_ctx->pty_master = NULL;
    }
    if (input) {
        pthread_mutex_lock(&input->mu);
        while (input->reader_active) {
            vprocCondWait(&input->cv, &input->mu);
        }
        pthread_mutex_unlock(&input->mu);
        pthread_mutex_destroy(&input->mu);
        pthread_cond_destroy(&input->cv);
        free(input->buf);
        free(input);
        stdio_ctx->input = NULL;
    }
    free(stdio_ctx);
}

void vprocSessionStdioSetDefault(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return;
    }
    gSessionStdioDefault = *stdio_ctx;
    if (!gSessionStdioTls || gSessionStdioTls == &gSessionStdioDefault) {
        gSessionStdioTls = &gSessionStdioDefault;
    }
}

bool vprocSessionStdioIsDefault(VProcSessionStdio *stdio_ctx) {
    return stdio_ctx == &gSessionStdioDefault;
}

bool vprocSessionStdioNeedsRefresh(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return true;
    }
    if (stdio_ctx->pty_active || stdio_ctx->stdin_pscal_fd) {
        return false;
    }
    if (!vprocSessionStdioMatchFd(stdio_ctx->stdin_host_fd, STDIN_FILENO)) {
        return true;
    }
    if (!vprocSessionStdioMatchFd(stdio_ctx->stdout_host_fd, STDOUT_FILENO)) {
        return true;
    }
    if (!vprocSessionStdioMatchFd(stdio_ctx->stderr_host_fd, STDERR_FILENO)) {
        return true;
    }
    return false;
}

void vprocSessionStdioRefresh(VProcSessionStdio *stdio_ctx, int kernel_pid) {
    if (!stdio_ctx || !vprocSessionStdioNeedsRefresh(stdio_ctx)) {
        return;
    }
    if (vprocToolDebugEnabled()) {
        vprocDebugLogf(
                "[session-stdio] refresh stdin=%d stdout=%d stderr=%d\n",
                stdio_ctx->stdin_host_fd,
                stdio_ctx->stdout_host_fd,
                stdio_ctx->stderr_host_fd);
    }
    VProcSessionInput *input = stdio_ctx->input;
    if (input) {
        pthread_mutex_lock(&input->mu);
        input->stop_requested = true;
        input->off = 0;
        input->len = 0;
        input->eof_mark_count = 0;
        input->eof = false;
        input->interrupt_pending = false;
        input->interrupt_key = false;
        pthread_cond_broadcast(&input->cv);
        pthread_mutex_unlock(&input->mu);
    }
    if (stdio_ctx->stdin_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdin_host_fd);
    }
    if (stdio_ctx->stdout_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdout_host_fd);
    }
    if (stdio_ctx->stderr_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stderr_host_fd);
    }
    stdio_ctx->stdin_host_fd = -1;
    stdio_ctx->stdout_host_fd = -1;
    stdio_ctx->stderr_host_fd = -1;
    if (input) {
        pthread_mutex_lock(&input->mu);
        while (input->reader_active) {
            vprocCondWait(&input->cv, &input->mu);
        }
        input->reader_fd = -1;
        pthread_mutex_unlock(&input->mu);
    }
    vprocSessionStdioInit(stdio_ctx, kernel_pid);
}

void vprocSetJobId(int pid, int job_id) {
    pthread_mutex_lock(&gVProcTasks.mu);
    /* The job table clears these for a job it has reaped. That must not bring the pid back. */
    VProcTaskEntry *entry = job_id ? vprocTaskEnsureSlotLocked(pid) : vprocTaskFindLocked(pid);
    if (entry) {
        entry->job_id = job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocGetJobId(int pid) {
    int id = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        id = entry->job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return id;
}

void vprocSetCommandLabel(int pid, const char *label) {
    char thread_name[16];
    bool rename_thread = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    /* A name is no process: a pid the kernel never gave out (the app's own, from getpid with
     * no task) became a row of its own, an `exsh` with no terminal that never ended. */
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        free(entry->label);
        entry->label = NULL;
        if (label && *label) {
            entry->label = strdup(label);
        }
        vprocSetCommLocked(entry, label);
        rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
}

bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return false;
    }
    bool ok = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->label && entry->label[0]) {
        strncpy(buf, entry->label, buf_len - 1);
        buf[buf_len - 1] = '\0';
        ok = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return ok;
}

bool vprocSigchldPending(int pid) {
    bool pending = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        pending = entry->sigchld_events > 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pending;
}

int vprocSetSigchldBlocked(int pid, bool block) {
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sigchld_blocked = block;
        if (block) {
            entry->sigchld_delivery_queued = false;
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    if (rc == 0 && !block) {
        if (!entry->sigchld_delivery_queued) {
            if (vprocKernelQueueSigchldAggregate(pid, false)) {
                entry->sigchld_delivery_queued = true;
            } else {
                vprocDeliverPendingSignalsLocked(entry);
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocClearSigchldPending(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sigchld_events = 0;
        entry->pending_signals &= ~vprocSigMask(SIGCHLD);
        if (vprocSigIndexValid(SIGCHLD)) {
            entry->pending_counts[SIGCHLD] = 0;
        }
        entry->sigchld_delivery_queued = false;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetRusage(int pid, int utime, int stime) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->rusage_utime = utime;
        entry->rusage_stime = stime;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocBlockSignals(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = ((uint32_t)mask) & ~unmaskable;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->blocked_signals &= ~unmaskable;
        entry->blocked_signals |= mask_bits;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocUnblockSignals(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->blocked_signals &= ~unmaskable;
        entry->blocked_signals &= ~mask_bits;
        vprocDeliverPendingSignalsLocked(entry);
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocIgnoreSignal(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    if (mask_bits & unmaskable) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->ignored_signals &= ~unmaskable;
        entry->ignored_signals |= mask_bits;
        entry->pending_signals &= ~mask_bits;
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t bit = vprocSigMask(sig);
            if (bit & mask_bits) {
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_flags = 0;
                entry->actions[sig].sa_handler = SIG_IGN;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocDefaultSignal(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->ignored_signals &= ~unmaskable;
        entry->ignored_signals &= ~mask_bits;
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t bit = vprocSigMask(sig);
            if (bit & mask_bits) {
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_flags = 0;
                entry->actions[sig].sa_handler = SIG_DFL;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

static struct sigaction vprocGetSigactionLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = SIG_DFL;
        return sa;
    }
    return entry->actions[sig];
}

int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old) {
    if (!vprocSigIndexValid(sig) || sig == SIGKILL || sig == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    uint32_t mask = vprocSigMask(sig);
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        if (old) {
            *old = vprocGetSigactionLocked(entry, sig);
        }
        if (act) {
            entry->actions[sig] = *act;
            if (act->sa_handler == SIG_IGN) {
                entry->ignored_signals |= mask;
                entry->pending_signals &= ~mask;
            } else {
                entry->ignored_signals &= ~mask;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

static VProc *vprocForThread(void) {
    if (!gVProcTlsReady) {
        return NULL;
    }
    VProc *vp = gVProcCurrent;
    if (!vp) {
        return NULL;
    }
    if (!vprocRegistryContainsValidated(vp)) {
        return NULL;
    }
    return vp;
}

static VProc *vprocFindByPid(int pid) {
    if (pid <= 0) {
        return NULL;
    }
    VProc *match = NULL;
    pthread_mutex_lock(&gVProcRegistryMu);
    if (gVProcRegistryHint && gVProcRegistryHint->pid == pid) {
        match = gVProcRegistryHint;
    } else {
        for (size_t i = 0; i < gVProcRegistryCount; ++i) {
            VProc *entry = gVProcRegistry[i];
            if (entry && entry->pid == pid) {
                match = entry;
                gVProcRegistryHint = entry;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return match;
}

static int vprocPtyNumFromPscalFd(const struct pscal_fd *pscal_fd) {
    if (!pscal_fd || !pscal_fd->tty) {
        return -1;
    }
    const struct tty *tty = pscal_fd->tty;
    if (tty->type != TTY_PSEUDO_MASTER_MAJOR && tty->type != TTY_PSEUDO_SLAVE_MAJOR) {
        return -1;
    }
    return tty->num;
}

static int vprocPtyNumFromVProcLocked(const VProc *vp) {
    if (!vp) {
        return -1;
    }
    const int stdio_fds[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    for (size_t i = 0; i < sizeof(stdio_fds) / sizeof(stdio_fds[0]); ++i) {
        int fd = stdio_fds[i];
        if ((size_t)fd >= vp->capacity) {
            continue;
        }
        const VProcFdEntry *entry = &vp->entries[fd];
        if (entry->kind != VPROC_FD_PSCAL || !entry->pscal_fd) {
            continue;
        }
        int pty_num = vprocPtyNumFromPscalFd(entry->pscal_fd);
        if (pty_num >= 0) {
            return pty_num;
        }
    }
    return -1;
}

static int vprocPtyNumForPid(int pid) {
    if (pid <= 0) {
        return -1;
    }
    int pty_num = -1;
    pthread_mutex_lock(&gVProcRegistryMu);
    VProc *vp = NULL;
    if (gVProcRegistryHint && gVProcRegistryHint->pid == pid) {
        vp = gVProcRegistryHint;
    } else {
        for (size_t i = 0; i < gVProcRegistryCount; ++i) {
            VProc *entry = gVProcRegistry[i];
            if (entry && entry->pid == pid) {
                vp = entry;
                gVProcRegistryHint = entry;
                break;
            }
        }
    }
    if (vp) {
        pthread_mutex_lock(&vp->mu);
        pty_num = vprocPtyNumFromVProcLocked(vp);
        pthread_mutex_unlock(&vp->mu);
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return pty_num;
}

static int vprocFindPrimaryPidForThread(pthread_t tid) {
    int pid = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) {
            continue;
        }
        if (entry->tid && pthread_equal(entry->tid, tid)) {
            pid = entry->pid;
            break;
        }
    }
    if (pid <= 0) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (!entry || entry->pid <= 0) {
                continue;
            }
            if (vprocTaskEntryHasThreadLocked(entry, tid)) {
                pid = entry->pid;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pid;
}

static VProc *vprocResolveShellVprocForPathOps(void) {
    if (gVprocSubshellPid > 0) {
        VProc *subshell_vp = vprocFindByPid(gVprocSubshellPid);
        if (subshell_vp) {
            return subshell_vp;
        }
    }
    /*
     * Prefer session-scoped shell identity over thread hints. Worker threads
     * can be reused across tabs/sessions and may retain stale pid hints, while
     * gSessionStdioTls reflects the active tab/session binding.
     */
    int shell_pid = vprocSessionShellPidHint(gSessionStdioTls);
    if (shell_pid > 0) {
        VProc *session_vp = vprocFindByPid(shell_pid);
        if (session_vp) {
            return session_vp;
        }
    }
    int thread_pid = vprocFindPrimaryPidForThread(pthread_self());
    if (thread_pid > 0) {
        VProc *thread_vp = vprocFindByPid(thread_pid);
        if (thread_vp) {
            return thread_vp;
        }
    }
    if (shell_pid <= 0) {
        shell_pid = gShellSelfPid;
    }
    if (shell_pid <= 0) {
        shell_pid = gShellSelfPidGlobal;
    }
    if (shell_pid <= 0) {
        return NULL;
    }
    return vprocFindByPid(shell_pid);
}

static void vprocDeliverPendingSignalsAt(bool safe_point) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    bool exit_current = false;
    if (entry) {
        if (vprocTaskExpireRealTimerLocked(entry, vprocNowMonoNs())) {
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
        exit_current = vprocDeliverPendingSignalsLocked(entry);
        /* Killed from another thread where it couldn't be cancelled: it hears of that here. */
        if (entry->dying_since_ns != 0 && vprocCurrentIsEntryLocked(entry) && vprocEntryIsCurrentThreadLocked(entry)) {
            exit_current = true;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (exit_current) {
        vprocDieHere(safe_point);
    }
}

static void vprocDeliverPendingSignalsForCurrent(void) {
    vprocDeliverPendingSignalsAt(false);
}

void vprocDeliverSignalsHere(void) {
    vprocDeliverPendingSignalsAt(true);
}

int vprocTaskExitSignal(int pid) {
    int sig = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        sig = entry->exit_signal;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return sig;
}

int vprocDeliverTaskSignals(int pid) {
    if (pid <= 0) {
        return 0;
    }
    int fatal = 0;
    bool exit_current = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->tid && pthread_equal(entry->tid, pthread_self())) {
        if (vprocTaskExpireRealTimerLocked(entry, vprocNowMonoNs())) {
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
        exit_current = vprocDeliverPendingSignalsLocked(entry);
        for (int sig = 1; (entry->unwinds_itself || entry->shell_task) && !fatal && sig < 32; ++sig) {
            if ((entry->pending_signals & vprocSigMask(sig)) && !vprocSignalBlockedLocked(entry, sig) &&
                vprocEffectiveSignalActionLocked(entry, sig) == VPROC_SIG_KILL) {
                entry->pending_signals &= ~vprocSigMask(sig);
                entry->pending_counts[sig] = 0;
                fatal = sig;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (exit_current) {
        vprocDieHere(true);
    }
    return fatal;
}

void vprocNoteCaughtSignal(int pid, int sig) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->caught_signals |= vprocSigMask(sig);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

uint32_t vprocCaughtSignals(int pid) {
    uint32_t caught = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->tid && pthread_equal(entry->tid, pthread_self())) {
        caught = entry->caught_signals;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return caught;
}

bool vprocTakeCaughtSignal(int pid, int sig) {
    bool taken = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->tid && pthread_equal(entry->tid, pthread_self()) &&
        (entry->caught_signals & vprocSigMask(sig))) {
        entry->caught_signals &= ~vprocSigMask(sig);
        taken = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return taken;
}

int vprocHaltSession(void) {
    int sid = vprocShimHasVirtualContext() ? vprocGetSid(0) : -1;
    if (sid <= 1) {
        errno = ESRCH;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *leader = vprocTaskFindLocked(sid);
    if (leader) {
        leader->halting = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return vprocKillShim(sid, SIGKILL);
}

bool vprocTaskHalting(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    bool halting = entry && entry->halting;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return halting;
}

bool vprocTakePendingSignal(int pid, int sig) {
    bool taken = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->tid && pthread_equal(entry->tid, pthread_self())) {
        taken = vprocConsumePendingSignalLocked(entry, sig);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return taken;
}

int vprocCurrentDyingSignal(void) {
    VProc *vp = vprocCurrent();
    int pid = vp ? vprocPid(vp) : 0;
    int sig = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = pid > 0 ? vprocTaskFindLocked(pid) : NULL;
    if (entry && entry->dying_since_ns != 0) {
        sig = entry->exit_signal;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return sig;
}

void vprocDefaultCaughtActions(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    for (int sig = 1; entry && sig < 32; ++sig) {
        if (sig != SIGINT && sig != SIGQUIT && entry->actions[sig].sa_handler != SIG_DFL &&
            entry->actions[sig].sa_handler != SIG_IGN) {
            sigemptyset(&entry->actions[sig].sa_mask);
            entry->actions[sig].sa_handler = SIG_DFL;
            entry->actions[sig].sa_flags = 0;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

bool vprocSetShellTask(int pid, bool enabled) {
    bool previous = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        previous = entry->shell_task;
        entry->shell_task = enabled;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return previous;
}

void vprocSetUnwindsItself(int pid, bool enabled) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->unwinds_itself = enabled;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocUnwindAsThreadDoes(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && !entry->unwinds_itself && vprocThreadUnwindsItselfLocked(pthread_self())) {
        entry->unwinds_itself = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetSilentToParent(int pid, bool silent) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->silent_to_parent = silent;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetNotAProcess(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->not_a_process = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocResetCaughtSignals(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    for (int sig = 1; entry && sig < 32; ++sig) {
        if (sig == SIGINT || sig == SIGQUIT || entry->actions[sig].sa_handler == SIG_DFL ||
            entry->actions[sig].sa_handler == SIG_IGN) {
            continue;
        }
        sigemptyset(&entry->actions[sig].sa_mask);
        entry->actions[sig].sa_handler = SIG_DFL;
        entry->actions[sig].sa_flags = 0;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocSigpending(int pid, sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    sigemptyset(set);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        if (vprocTaskExpireRealTimerLocked(entry, vprocNowMonoNs())) {
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
        uint32_t pending = entry->pending_signals;
        for (int sig = 1; sig < 32; ++sig) {
            if ((pending & vprocSigMask(sig)) || entry->pending_counts[sig] > 0) {
                sigaddset(set, sig);
            }
        }
    } else {
        errno = ESRCH;
        pthread_mutex_unlock(&gVProcTasks.mu);
        return -1;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

int vprocSigsuspend(int pid, const sigset_t *mask) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    uint32_t original_blocked = entry->blocked_signals;
    bool exit_current = false;
    if (mask) {
        entry->blocked_signals = 0;
        for (int sig = 1; sig < 32; ++sig) {
            if (sigismember(mask, sig)) {
                entry->blocked_signals |= vprocSigMask(sig);
            }
        }
    }
    while (true) {
        if (vprocTaskExpireRealTimerLocked(entry, vprocNowMonoNs())) {
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
        uint32_t orig_pending = entry->pending_signals;
        exit_current = vprocDeliverPendingSignalsLocked(entry);
        if (orig_pending != 0) {
            break;
        }
        (void)vprocWaitUntilTimerOrSignalLocked(entry, NULL, NULL);
    }
    entry->blocked_signals = original_blocked;
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (exit_current) {
        vprocDieHere(true);
    }
    errno = EINTR;
    return -1;
}

int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    if (oldset) {
        sigemptyset(oldset);
        for (int sig = 1; sig < 32; ++sig) {
            if (entry->blocked_signals & vprocSigMask(sig)) {
                sigaddset(oldset, sig);
            }
        }
    }
    if (!set) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return 0;
    }
    uint32_t mask_bits = 0;
    for (int sig = 1; sig < 32; ++sig) {
        if (sigismember(set, sig)) {
            mask_bits |= vprocSigMask(sig);
        }
    }
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    mask_bits &= ~unmaskable;
    if (how == SIG_BLOCK) {
        entry->blocked_signals |= mask_bits;
    } else if (how == SIG_UNBLOCK) {
        entry->blocked_signals &= ~mask_bits;
    } else if (how == SIG_SETMASK) {
        entry->blocked_signals = mask_bits;
    } else {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = EINVAL;
        return -1;
    }
    bool exit_current = vprocDeliverPendingSignalsLocked(entry);
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (exit_current) {
        vprocDieHere(true);
    }
    return 0;
}

int vprocSigwait(int pid, const sigset_t *set, int *sig) {
    if (!set || !sig) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    while (true) {
        if (vprocTaskExpireRealTimerLocked(entry, vprocNowMonoNs())) {
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
        for (int s = 1; s < 32; ++s) {
            if (!sigismember(set, s)) continue;
            uint32_t bit = vprocSigMask(s);
            if (entry->pending_counts[s] > 0 || (entry->pending_signals & bit)) {
                if (entry->pending_counts[s] > 0) {
                    entry->pending_counts[s]--;
                }
                if (entry->pending_counts[s] <= 0) {
                    entry->pending_signals &= ~bit;
                    entry->pending_counts[s] = 0;
                }
                *sig = s;
                pthread_mutex_unlock(&gVProcTasks.mu);
                return 0;
            }
        }
        (void)vprocWaitUntilTimerOrSignalLocked(entry, NULL, NULL);
    }
}

int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig) {
    if (!set || !sig) {
        errno = EINVAL;
        return -1;
    }
    struct timespec deadline = {0, 0};
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return -1;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            return -1;
        }
        deadline.tv_sec = now.tv_sec + timeout->tv_sec;
        deadline.tv_nsec = now.tv_nsec + timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    while (true) {
        if (vprocTaskExpireRealTimerLocked(entry, vprocNowMonoNs())) {
            pthread_cond_broadcast(&gVProcTasks.cv);
        }
        for (int s = 1; s < 32; ++s) {
            if (!sigismember(set, s)) continue;
            uint32_t bit = vprocSigMask(s);
            if (entry->pending_counts[s] > 0 || (entry->pending_signals & bit)) {
                if (entry->pending_counts[s] > 0) {
                    entry->pending_counts[s]--;
                }
                if (entry->pending_counts[s] <= 0) {
                    entry->pending_signals &= ~bit;
                    entry->pending_counts[s] = 0;
                }
                *sig = s;
                pthread_mutex_unlock(&gVProcTasks.mu);
                return s;
            }
        }
        if (timeout) {
            struct timespec now_rt;
            if (clock_gettime(CLOCK_REALTIME, &now_rt) == 0 &&
                vprocTimespecCmp(&now_rt, &deadline) >= 0) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = EAGAIN;
                return -1;
            }
        }
        bool timeout_hit = false;
        (void)vprocWaitUntilTimerOrSignalLocked(entry, timeout ? &deadline : NULL, &timeout_hit);
        if (timeout_hit && timeout) {
            struct timespec now_rt;
            if (clock_gettime(CLOCK_REALTIME, &now_rt) == 0 &&
                vprocTimespecCmp(&now_rt, &deadline) >= 0) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = EAGAIN;
                return -1;
            }
        }
    }
}

static int vprocResolveFdForShim(VProc *vp, int fd, int allow_real, struct pscal_fd **out_pscal) {
    if (out_pscal) {
        *out_pscal = NULL;
    }
    if (vprocFdIsTaggedHost(fd)) {
        return fd & ~VPROC_HOST_FD_TAG;
    }
    if (!vp) {
        return allow_real ? fd : -1;
    }

    int host = -1;
    if (fd >= 0) {
        pthread_mutex_lock(&vp->mu);
        if ((size_t)fd < vp->capacity) {
            VProcFdEntry entry = vp->entries[fd];
            if (entry.kind == VPROC_FD_PSCAL && entry.pscal_fd) {
                if (out_pscal) {
                    *out_pscal = pscal_fd_retain(entry.pscal_fd);
                }
            } else if (entry.kind == VPROC_FD_HOST) {
                host = entry.host_fd;
            }
        }
        pthread_mutex_unlock(&vp->mu);
    }

    if (host < 0 && allow_real && fd >= 0 &&
        (vp == NULL || vprocAllowRealFdFallback(vp, fd))) {
        struct stat st;
        if (vprocHostFstatRaw(fd, &st) == 0) {
            return fd;
        }
    }
    if (host < 0 && (!out_pscal || !*out_pscal)) {
        errno = EBADF;
    }
    return host;
}

static int shimTranslate(int fd, int allow_real) {
    VProc *vp = vprocForThread();
    return vprocResolveFdForShim(vp, fd, allow_real, NULL);
}

int vprocFilenoShim(FILE *stream) {
    int fd = (fileno)(stream);
    /* A stream on a device has no host number, and its slot is what the shims take. */
    if (fd < 0) {
        return pscalPathVirtualized_streamSlot(stream);
    }
    /* The standard streams' numbers are slots already. */
    if (stream == stdin || stream == stdout || stream == stderr) {
        return fd;
    }
    return vprocTagHostFd(fd);
}

int vprocFtruncateShim(int fd, off_t length) {
    int host = shimTranslate(fd, 1);
    return host < 0 ? -1 : ftruncate(host, length);
}

int vprocFutimensShim(int fd, const struct timespec times[2]) {
    int host = shimTranslate(fd, 1);
    return host < 0 ? -1 : futimens(host, times);
}

/* A vproc's cwd is a path of its own, which the host's fchdir would not move. */
int vprocFchdirShim(int fd) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
#if defined(F_GETPATH)
    char path[PATH_MAX];
    if (fcntl(host, F_GETPATH, path) != 0) {
        return -1;
    }
    return vprocChdirShim(path);
#else
    return fchdir(host);
#endif
}

/* One open file sits under the whole session's terminal: its reader thread and
 * every later command use it, and nothing would put O_NONBLOCK back after a
 * command left it set. Reads of it come through the session's queue, which ignores
 * the flag anyway, while tty_write would stop waiting for room, so it stays blocking. */
static bool vprocFdIsSessionStdio(struct pscal_fd *pscal_fd, int host) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        return false;
    }
    if (pscal_fd) {
        return pscal_fd == session->stdin_pscal_fd ||
               pscal_fd == session->stdout_pscal_fd ||
               pscal_fd == session->stderr_pscal_fd ||
               pscal_fd == session->pty_slave;
    }
    return vprocSessionFdMatchesHost(host, session->stdin_host_fd) ||
           vprocSessionFdMatchesHost(host, session->stdout_host_fd) ||
           vprocSessionFdMatchesHost(host, session->stderr_host_fd);
}

/* An in-process pipe, PTY or the location device has no host descriptor. What
 * fcntl means on one: the file status flags are the pscal_fd's own, of which only
 * O_NONBLOCK can change; vprocSimulatedExec keeps every slot, so close-on-exec is
 * accepted and not kept; and none of them can be locked. */
static int vprocFcntlPscalFd(struct pscal_fd *pscal_fd, int cmd, int arg) {
    switch (cmd) {
        case F_GETFD:
        case F_SETFD:
            return 0;
        case F_GETFL: {
            lock(&pscal_fd->lock);
            unsigned flags = pscal_fd->flags;
            unlock(&pscal_fd->lock);
            int mode = O_RDONLY;
            if (pscal_fd->ops->read && pscal_fd->ops->write) {
                mode = (int)(flags & O_ACCMODE);
            } else if (pscal_fd->ops->write) {
                mode = O_WRONLY;
            }
            return mode | (int)(flags & O_NONBLOCK);
        }
        case F_SETFL:
            if (vprocFdIsSessionStdio(pscal_fd, -1)) {
                return 0;
            }
            lock(&pscal_fd->lock);
            pscal_fd->flags = (pscal_fd->flags & ~(unsigned)O_NONBLOCK) | ((unsigned)arg & O_NONBLOCK);
            unlock(&pscal_fd->lock);
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

/* A closed descriptor's link is not there, on Linux, and that is what the path says. */
static bool vprocNamedFdOpen(VProc *vp, int fd) {
    if (vprocSlotIsPscal(vp, fd) || vprocTranslateFd(vp, fd) >= 0) {
        return true;
    }
    errno = ENOENT;
    return false;
}

/* Opening /proc/self/fd/N on Linux opens the file again: its own offset, and O_TRUNC and
 * O_APPEND mean what they say, whatever the descriptor was opened with. So `> /dev/stdout` on a
 * file redirection truncates it, and `cat /dev/stdin` reads from the top. The name is only used
 * for the file it still names, so the truncation waits until that is known. A pipe or a socket
 * has no name, and what it can be is a copy, in the direction it already goes. */
static int vprocReopenHostFd(int host, int flags) {
    struct stat st;
    if (vprocHostFstatRaw(host, &st) != 0) {
        return -1;
    }
    int access_mode = flags & O_ACCMODE;
    bool truncates = (flags & O_TRUNC) && access_mode != O_RDONLY;
#if defined(F_GETPATH)
    if (!S_ISFIFO(st.st_mode) && !S_ISSOCK(st.st_mode)) {
        char path[PATH_MAX];
        if ((fcntl)(host, F_GETPATH, path) == 0) {
            int plain = flags & ~(O_CREAT | O_EXCL | O_TRUNC);
            int reopened = vprocHostOpenRawInternal(path, S_ISCHR(st.st_mode) ? (plain | O_NOCTTY) : plain, 0, false);
            if (reopened < 0 && errno != ENOENT) {
                return -1;
            }
            if (reopened >= 0) {
                struct stat now;
                if (vprocHostFstatRaw(reopened, &now) == 0 &&
                    now.st_dev == st.st_dev && now.st_ino == st.st_ino) {
                    if (truncates && S_ISREG(st.st_mode)) {
                        (void)ftruncate(reopened, 0);
                    }
                    return reopened;
                }
                vprocHostCloseRaw(reopened);
            }
        }
    }
#endif
    /* No name, or the name is another file's by now. */
    int have = (fcntl)(host, F_GETFL);
    if (have < 0) {
        return -1;
    }
    have &= O_ACCMODE;
    if (have != O_RDWR && access_mode != have) {
        errno = EACCES;
        return -1;
    }
    int copy = vprocCloneFd(host);
    if (copy >= 0 && truncates && S_ISREG(st.st_mode)) {
        (void)ftruncate(copy, 0);
    }
    return copy;
}

/* A descriptor of another vproc, as /proc/<pid>/fd/N reaches it, copied to a slot of this one. */
/* The pid whose table is task `pid`'s now. A `.`, `source` or `eval` the shell runs is the shell
 * itself, as `$$` says, but it runs under a vproc of its own stacked above the shell's, and what it
 * redirects is there: so `{ echo a > /proc/$$/fd/1; } > f` in a sourced file must find f there, not
 * the shell's table below. Such a vproc is marked not a process and its parent is what it stands
 * on, so this follows those up from `pid`, to the one running now. */
static int vprocTaskTablePid(int pid) {
    for (int hops = 0; pid > 0 && hops < 32; ++hops) {
        int above = -1;
        pthread_mutex_lock(&gVProcTasks.mu);
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            const VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid > 0 && entry->not_a_process && entry->parent_pid == pid &&
                !entry->exited && !entry->zombie) {
                above = entry->pid;
            }
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (above <= 0 || !vprocFindByPid(above)) {
            break;
        }
        pid = above;
    }
    return pid;
}

static int vprocCopyFdOfTask(VProc *vp, int pid, int fd) {
    pid = vprocTaskTablePid(pid);
    struct pscal_fd *pscal_fd = NULL;
    int host = -1;
    /* fcntl is a cancellation point, and a thread killed there would keep both locks. */
    int cancel_state = PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        VProc *owner = gVProcRegistry[i];
        if (!owner || owner->pid != pid) {
            continue;
        }
        pthread_mutex_lock(&owner->mu);
        if ((size_t)fd < owner->capacity) {
            const VProcFdEntry *entry = &owner->entries[fd];
            if (entry->kind == VPROC_FD_PSCAL && entry->pscal_fd) {
                pscal_fd = pscal_fd_retain(entry->pscal_fd);
            } else if (entry->kind == VPROC_FD_HOST) {
                host = vprocCloneFd(entry->host_fd);
            }
        }
        pthread_mutex_unlock(&owner->mu);
        break;
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    int slot = -1;
    if (pscal_fd) {
        slot = vprocInsertPscalFd(vp, pscal_fd);
        /* The table retains. */
        pscal_fd_close(pscal_fd);
    } else if (host >= 0) {
        slot = vprocInsert(vp, host);
        if (slot < 0) {
            vprocHostClose(host);
        }
    } else {
        errno = ENOENT;
    }
    pthread_setcancelstate(cancel_state, NULL);
    return slot;
}

static int vprocOpenNamedFd(VProc *vp, int pid, int fd, int flags) {
    if (pid != vprocPid(vp)) {
        int copy = vprocCopyFdOfTask(vp, pid, fd);
        if (copy < 0) {
            return -1;
        }
        int slot = vprocOpenNamedFd(vp, vprocPid(vp), copy, flags);
        int err = errno;
        vprocCloseShim(copy);
        errno = err;
        return slot;
    }
    if (!vprocNamedFdOpen(vp, fd)) {
        return -1;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        errno = EEXIST;
        return -1;
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        /* A terminal does both. An end of an in-process pipe has one direction, where Linux
         * would open the other end. */
        int mode = vprocFcntlPscalFd(pscal_fd, F_GETFL, 0) & O_ACCMODE;
        int want = flags & O_ACCMODE;
        if (!pscal_fd->tty && mode != O_RDWR && want != mode) {
            pscal_fd_close(pscal_fd);
            errno = EACCES;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, pscal_fd);
        /* The table retains. */
        pscal_fd_close(pscal_fd);
        return slot;
    }
    int reopened = vprocReopenHostFd(vprocTranslateFd(vp, fd), flags);
    if (reopened < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, reopened);
    if (slot < 0) {
        vprocHostClose(reopened);
    }
    return slot;
}

static void vprocDescribeHostFd(int host, char *out, size_t size) {
    struct stat st;
    out[0] = '\0';
    if (vprocHostFstatRaw(host, &st) != 0) {
        return;
    }
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
        snprintf(out, size, "%s:[%llu]", S_ISFIFO(st.st_mode) ? "pipe" : "socket", (unsigned long long)st.st_ino);
        return;
    }
#if defined(F_GETPATH)
    char path[PATH_MAX];
    if ((fcntl)(host, F_GETPATH, path) == 0) {
        snprintf(out, size, "%s", path);
    }
#endif
}

static void vprocDescribePscalFd(struct pscal_fd *pscal_fd, char *out, size_t size) {
    if (pscalPtyIsMaster(pscal_fd)) {
        snprintf(out, size, "/dev/ptmx");
    } else if (pscal_fd->tty) {
        snprintf(out, size, pscalPtyIsSlave(pscal_fd) ? "/dev/pts/%d" : "/dev/tty%d", pscal_fd->tty->num);
    } else {
        snprintf(out, size, "pipe:[%llu]", (unsigned long long)(uintptr_t)pscal_fd);
    }
}

bool vprocFdLinkTarget(int pid, int fd, char *out, size_t size) {
    if (pid <= 0 || fd < 0 || !out || size == 0) {
        return false;
    }
    pid = vprocTaskTablePid(pid);
    out[0] = '\0';
    bool found = false;
    struct pscal_fd *pscal_fd = NULL;
    /* fcntl is a cancellation point, and a thread killed there would keep both locks. */
    int cancel_state = PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        VProc *vp = gVProcRegistry[i];
        if (!vp || vp->pid != pid) {
            continue;
        }
        pthread_mutex_lock(&vp->mu);
        if ((size_t)fd < vp->capacity) {
            const VProcFdEntry *entry = &vp->entries[fd];
            if (entry->kind == VPROC_FD_PSCAL && entry->pscal_fd) {
                pscal_fd = pscal_fd_retain(entry->pscal_fd);
                found = true;
            } else if (entry->kind == VPROC_FD_HOST) {
                vprocDescribeHostFd(entry->host_fd, out, size);
                found = true;
            }
        }
        pthread_mutex_unlock(&vp->mu);
        break;
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    if (pscal_fd) {
        vprocDescribePscalFd(pscal_fd, out, size);
        pscal_fd_close(pscal_fd);
    }
    pthread_setcancelstate(cancel_state, NULL);
    return found && out[0] != '\0';
}

int vprocFcntlShim(int fd, int cmd, ...) {
    /* As libc's fcntl does, take the argument whether or not this command has one.
     * Pointer-sized holds either kind: an int's upper half is undefined, so the
     * commands that take one narrow it again. */
    va_list ap;
    va_start(ap, cmd);
    intptr_t arg = va_arg(ap, intptr_t);
    va_end(ap);

    VProc *vp = vprocForThread();
    if (vp && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
        if ((int)arg < 0) {
            errno = EINVAL;
            return -1;
        }
        return vprocDupAbove(vp, fd, (int)arg);
    }
    struct pscal_fd *pscal_fd = NULL;
    int host = vprocResolveFdForShim(vp, fd, 1, &pscal_fd);
    if (pscal_fd) {
        int rc = vprocFcntlPscalFd(pscal_fd, cmd, (int)arg);
        int saved_errno = errno;
        pscal_fd_close(pscal_fd);
        errno = saved_errno;
        return rc;
    }
    if (host < 0) {
        return -1;
    }
    switch (cmd) {
        case F_GETFD:
        case F_GETFL:
            return fcntl(host, cmd);
        case F_SETFL: {
            int flags = (int)arg;
            if (vprocFdIsSessionStdio(NULL, host)) {
                int current = fcntl(host, F_GETFL);
                if (current < 0) {
                    return -1;
                }
                flags = (flags & ~O_NONBLOCK) | (current & O_NONBLOCK);
            }
            return fcntl(host, F_SETFL, flags);
        }
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
            return fcntl(host, cmd, (int)arg);
        default:
            return fcntl(host, cmd, arg);
    }
}

/* Pipes and terminals have no offset to read at. */
ssize_t vprocPreadShim(int fd, void *buf, size_t count, off_t offset) {
    struct pscal_fd *pscal_fd = NULL;
    int host = vprocResolveFdForShim(vprocForThread(), fd, 1, &pscal_fd);
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        errno = ESPIPE;
        return -1;
    }
    return host < 0 ? -1 : pread(host, buf, count, offset);
}

ssize_t vprocPwriteShim(int fd, const void *buf, size_t count, off_t offset) {
    struct pscal_fd *pscal_fd = NULL;
    int host = vprocResolveFdForShim(vprocForThread(), fd, 1, &pscal_fd);
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        errno = ESPIPE;
        return -1;
    }
    return host < 0 ? -1 : pwrite(host, buf, count, offset);
}

/* The registry lock keeps the vproc alive, since vprocDestroy takes it out of the
 * registry first, and its own lock keeps each host descriptor open while it is asked.
 * A pscal_fd has a lock of its own, so it is asked after both are let go. */
ssize_t vprocSnapshotFds(int pid, VProcFdSnapshot *out, size_t capacity) {
    if (pid <= 0 || !out || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    pid = vprocTaskTablePid(pid);
    struct pscal_fd **pscal_fds = (struct pscal_fd **)calloc(capacity, sizeof(*pscal_fds));
    if (!pscal_fds) {
        return -1;
    }
    ssize_t count = -1;
    /* fcntl is a cancellation point, and a thread killed there would keep both locks. */
    int cancel_state = PTHREAD_CANCEL_ENABLE;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        VProc *vp = gVProcRegistry[i];
        if (!vp || vp->pid != pid) {
            continue;
        }
        count = 0;
        pthread_mutex_lock(&vp->mu);
        for (size_t fd = 0; fd < vp->capacity && fd < capacity; ++fd) {
            const VProcFdEntry *entry = &vp->entries[fd];
            int open_flags = 0;
            off_t pos = 0;
            if (entry->kind == VPROC_FD_PSCAL && entry->pscal_fd) {
                pscal_fds[count] = pscal_fd_retain(entry->pscal_fd);
            } else if (entry->kind == VPROC_FD_HOST) {
                open_flags = fcntl(entry->host_fd, F_GETFL);
                pos = lseek(entry->host_fd, 0, SEEK_CUR);
            } else {
                continue;
            }
            out[count].fd = (int)fd;
            out[count].open_flags = open_flags < 0 ? 0 : open_flags;
            out[count].pos = pos < 0 ? 0 : pos;
            count++;
        }
        pthread_mutex_unlock(&vp->mu);
        break;
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    for (ssize_t i = 0; i < count; ++i) {
        if (pscal_fds[i]) {
            out[i].open_flags = vprocFcntlPscalFd(pscal_fds[i], F_GETFL, 0);
            pscal_fd_close(pscal_fds[i]);
        }
    }
    free(pscal_fds);
    pthread_setcancelstate(cancel_state, NULL);
    return count;
}

static int vprocSetCompatErrno(int err) {
    errno = pscalCompatErrno(err);
    return -1;
}

static void vprocPtyTrace(const char *format, ...) {
    static int enabled = -1;
    const char *env = getenv("PSCALI_PTY_TRACE");
    if (enabled < 0 || (enabled == 0 && env && *env && strcmp(env, "0") != 0)) {
        enabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    }
    if (!enabled || !format) {
        return;
    }
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
#if defined(__APPLE__)
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
        return;
    }
#endif
    if (pscalRuntimeDebugLog) {
        pscalRuntimeDebugLog(buf);
    }
}

static int vprocHostIsatty(int fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostIsattyRaw(fd);
#else
    return isatty(fd);
#endif
}

static struct pscal_fd *vprocSessionPscalFdForStd(int fd);

/* The name of the terminal behind a slot. The host's ttyname would look up whatever host
 * descriptor has the slot's number, and has never heard of the app's terminals. */
char *vprocTtynameShim(int fd) {
    static __thread char name[32];
    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = NULL;
    int host = -1;
    if (vp) {
        host = vprocResolveFdForShim(vp, fd, 0, &pscal_fd);
        if (!pscal_fd && host < 0) {
            /* Closed, as for isatty: not the session's terminal. */
            host = vprocResolveFdForShim(vp, fd, 1, NULL);
            return host >= 0 ? ttyname(host) : NULL;
        }
    }
#if defined(PSCAL_TARGET_IOS)
    if (!pscal_fd && host < 0 &&
        (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
        pscal_fd = vprocSessionPscalFdForStd(fd);
    }
#endif
    if (pscal_fd) {
        struct tty *tty = pscal_fd->tty;
        int type = tty ? tty->type : 0;
        int num = tty ? tty->num : -1;
        pscal_fd_close(pscal_fd);
        if (!tty) {
            errno = ENOTTY;
            return NULL;
        }
        if (type == TTY_PSEUDO_SLAVE_MAJOR) {
            snprintf(name, sizeof(name), "/dev/pts/%d", num);
        } else if (type == TTY_PSEUDO_MASTER_MAJOR) {
            snprintf(name, sizeof(name), "/dev/ptmx");
        } else if (type == TTY_CONSOLE_MAJOR) {
            snprintf(name, sizeof(name), "/dev/tty%d", num);
        } else {
            snprintf(name, sizeof(name), "/dev/tty");
        }
        return name;
    }
    if (host < 0) {
        host = shimTranslate(fd, 1);
    }
    if (host < 0) {
        return NULL;
    }
    return ttyname(host);
}

int vprocIsattyShim(int fd) {
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = NULL;
        int host_fd = vprocResolveFdForShim(vp, fd, 0, &pscal_fd);
        if (pscal_fd) {
            int res = (pscal_fd->tty != NULL) ? 1 : 0;
            pscal_fd_close(pscal_fd);
            return res;
        }
        if (host_fd >= 0) {
            return vprocHostIsatty(host_fd);
        }
        /* A closed slot is no terminal, not the session's: `exec <&-; [ -t 0 ]` is false.
         * Only a host number this vproc owns is still asked about. */
        host_fd = vprocResolveFdForShim(vp, fd, 1, NULL);
        return host_fd >= 0 ? vprocHostIsatty(host_fd) : 0;
    }
#if defined(PSCAL_TARGET_IOS)
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(fd);
        if (pscal_fd) {
            int res = (pscal_fd->tty != NULL) ? 1 : 0;
            pscal_fd_close(pscal_fd);
            return res;
        }
    }
#endif
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return 0;
    }
    return vprocHostIsatty(host);
}

bool vprocStdinIsRedirected(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return false;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    struct pscal_fd *pscal_fd = NULL;
    int host_fd = vprocResolveFdForShim(vp, STDIN_FILENO, 0, &pscal_fd);
    if (pscal_fd) {
        bool terminal = pscal_fd->tty != NULL ||
                        (session && (session->stdin_pscal_fd == pscal_fd ||
                                     session->pty_slave == pscal_fd));
        pscal_fd_close(pscal_fd);
        return !terminal;
    }
    if (host_fd < 0) {
        return false;
    }
    /* A session with no PTY takes its terminal input through a host pipe. */
    if (vp->stdin_from_session ||
        (session && vprocSessionFdMatchesHost(host_fd, session->stdin_host_fd))) {
        return false;
    }
    struct stat st;
    if (vprocHostFstatRaw(host_fd, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) ||
           (S_ISCHR(st.st_mode) && !vprocHostIsatty(host_fd));
}

int pscalTtyCurrentPid(void) {
    return (int)vprocGetPidShim();
}

int pscalTtyCurrentPgid(void) {
    return (int)vprocGetpgrpShim();
}

int pscalTtyCurrentSid(void) {
    return (int)vprocGetsidShim(0);
}

bool pscalTtyIsSessionLeader(void) {
    int pid = pscalTtyCurrentPid();
    if (pid <= 0) {
        return false;
    }
    int sid = vprocGetSid(pid);
    return sid > 0 && sid == pid;
}

int pscalTtySendGroupSignal(int pgid, int sig) {
    if (pgid <= 0) {
        return _ESRCH;
    }
    if (sig == SIGWINCH_) {
        vprocResizeLogf("[ssh-resize] send-group-signal pgid=%d sig=SIGWINCH", pgid);
    }
    if (vprocKillLive(-pgid, sig) < 0) {
        if (sig == SIGWINCH_) {
            vprocResizeLogf("[ssh-resize] send-group-signal failed pgid=%d sig=SIGWINCH errno=%d", pgid, errno);
        }
        return _ESRCH;
    }
    if (sig == SIGWINCH_) {
        vprocResizeLogf("[ssh-resize] send-group-signal ok pgid=%d sig=SIGWINCH", pgid);
    }
    return 0;
}

void pscalTtySetForegroundPgid(int sid, int fg_pgid) {
    if (sid <= 0 || fg_pgid <= 0) {
        return;
    }
    (void)vprocSetForegroundPgidInternal(sid, fg_pgid, false);
}

int pscalTtyGetForegroundPgid(int sid) {
    if (sid <= 0) {
        return -1;
    }
    return vprocGetForegroundPgid(sid);
}

bool pscalTtyCallerIsKernel(void) {
    return vprocForThread() == NULL;
}

bool pscalTtySignalIgnoredOrBlocked(int sig) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return false;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    const bool refused = entry && (vprocSignalBlockedLocked(entry, sig) || vprocSignalIgnoredLocked(entry, sig));
    pthread_mutex_unlock(&gVProcTasks.mu);
    return refused;
}

/* Orphaned: no live member has a parent alive in another group of the same session. A task
 * parented to the kernel counts as the session leader's, as waiting for it does. */
bool pscalTtyCurrentGroupOrphaned(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return false;
    }
    const int kernel_pid = vprocGetKernelPid();
    bool orphaned = true;
    pthread_mutex_lock(&gVProcTasks.mu);
    const VProcTaskEntry *self = vprocTaskFindLocked(vprocPid(vp));
    const int pgid = self ? self->pgid : 0;
    for (size_t i = 0; pgid > 0 && i < gVProcTasks.count; ++i) {
        const VProcTaskEntry *member = &gVProcTasks.items[i];
        if (member->pid <= 0 || member->pgid != pgid || member->exited || member->adopted) {
            continue;
        }
        const int parent_pid = (kernel_pid > 0 && member->parent_pid == kernel_pid) ? member->sid : member->parent_pid;
        const VProcTaskEntry *parent = vprocTaskFindLocked(parent_pid);
        if (parent && !parent->exited && parent->sid == member->sid && parent->pgid != pgid) {
            orphaned = false;
            break;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return orphaned;
}

/* The caller has just sent its own group this signal. If that stopped it, this is where it
 * stops, and what it was doing starts over when it is continued. */
int pscalTtyAfterJobSignal(int sig) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return _EINTR;
    }
    vprocDeliverPendingSignalsForCurrent();
    if (vprocWaitIfStopped(vp)) {
        return 0;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    const bool restarts = entry && vprocSigIndexValid(sig) &&
                          vprocEffectiveSignalActionLocked(entry, sig) == VPROC_SIG_HANDLER &&
                          (entry->actions[sig].sa_flags & SA_RESTART);
    pthread_mutex_unlock(&gVProcTasks.mu);
    return restarts ? 0 : _EINTR;
}

/* A terminal wait asking whether a signal ends it (L10). A stop stops here, and the call
 * goes on once continued; a handler runs here, and the call goes on only with SA_RESTART;
 * anything that ends the task ends the call, though a cancel usually gets there first. A
 * task that unwinds itself, or a shell's own task, keeps a fatal signal queued until it
 * looks, as vprocCurrentHasInterruptingSignal has it. */
int pscalTtySignalCheck(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return 0;
    }
    const int pid = vprocPid(vp);
    if (pid <= 0) {
        return 0;
    }
    bool interrupt = false;
    bool pending = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        interrupt = entry->dying_since_ns != 0;
        for (int sig = 1; !interrupt && sig < 32; ++sig) {
            if (!(entry->pending_signals & vprocSigMask(sig)) ||
                vprocSignalBlockedLocked(entry, sig) || vprocSignalIgnoredLocked(entry, sig)) {
                continue;
            }
            pending = true;
            switch (vprocEffectiveSignalActionLocked(entry, sig)) {
                case VPROC_SIG_IGNORE:
                case VPROC_SIG_STOP:
                case VPROC_SIG_CONT:
                    break;
                case VPROC_SIG_HANDLER:
                    interrupt = !(entry->actions[sig].sa_flags & SA_RESTART);
                    break;
                default:
                    /* One that stays queued until the task looks would only have a retried
                     * read spin: the task acts on it where it looks. */
                    interrupt = !(entry->unwinds_itself || entry->shell_task);
                    break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (pending && !interrupt) {
        vprocDeliverPendingSignalsForCurrent();
        (void)vprocWaitIfStopped(vp);
    } else if (pending) {
        /* Handlers run now; a fatal one is left for the caller's unwinding to act on. */
        vprocDeliverPendingSignalsForCurrent();
    }
    return interrupt ? _EINTR : 0;
}

int pscalTtyCheckGroup(int sid, int pgid) {
    int result = _ESRCH;
    pthread_mutex_lock(&gVProcTasks.mu);
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        const VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0 || entry->pgid != pgid || entry->exited) {
            continue;
        }
        if (entry->sid == sid) {
            result = 0;
            break;
        }
        result = _EPERM;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return result;
}

void pscalTtyCallerGiveUpTerminal(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    if (entry) {
        entry->gave_up_terminal = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

bool pscalTtyCallerGaveUpTerminal(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return false;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    const VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    const bool gave_up = entry && entry->gave_up_terminal;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return gave_up;
}

int PSCALRuntimeSetSessionWinsize(uint64_t session_id, int cols, int rows) {
    return vprocSetSessionWinsize(session_id, cols, rows);
}

int PSCALRuntimeGetSessionWinsize(uint64_t session_id, int *cols_out, int *rows_out) {
    return vprocGetSessionWinsize(session_id, cols_out, rows_out);
}

static int gVprocPipelineReadLogCount = 0;
static int gVprocPipelineWriteLogCount = 0;

/* The reference a read or write holds: left behind, the end it refers to would never close. */
static void vprocPscalFdCloseCancelled(void *fd) {
    pscal_fd_close((struct pscal_fd *)fd);
}

/* Is this descriptor the session's terminal? Another open of it (/dev/tty,
 * /dev/pts/N) counts, since the keys it would read are the same ones. */
static bool vprocSessionFdIsTerminal(VProcSessionStdio *session, struct pscal_fd *pscal_fd) {
    if (!session || !pscal_fd || vprocSessionStdioIsDefault(session)) {
        return false;
    }
    if (session->stdin_pscal_fd == pscal_fd || session->pty_slave == pscal_fd) {
        return true;
    }
    return pscal_fd->tty && session->pty_slave && session->pty_slave->tty == pscal_fd->tty;
}

/* The interrupt key, at the shell's own prompt, where the reader took it for a signal
 * to the foreground. The shell has that signal blocked, so it would only wait, and
 * the line being edited would run together with the next one. */
static bool vprocPromptTakesInterruptKey(void) {
    int pid = vprocGetPidShim();
    if (!vprocShellPromptReadActive(pid)) {
        return false;
    }
    sigset_t pending;
    sigemptyset(&pending);
    if (vprocSigpending(pid, &pending) == 0 && sigismember(&pending, SIGINT)) {
        sigset_t watch;
        sigemptyset(&watch);
        sigaddset(&watch, SIGINT);
        int signo = 0;
        (void)vprocSigwait(pid, &watch, &signo);
    }
    return true;
}

ssize_t vprocReadShim(int fd, void *buf, size_t count) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = NULL;
    int host = vprocResolveFdForShim(vp, fd, 1, &pscal_fd);
    if (pscal_fd) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (vprocSessionFdIsTerminal(session, pscal_fd)) {
            /* Whatever reads the terminal, a dup() of stdin as in OpenSSH or a
             * plain `cat`: a reader thread already drains this PTY stream, so
             * read the session's queue. Reading the PTY as well would give
             * each key to one of the two, and the ones the thread took would
             * come out later, at whatever read next. */
            if (gVprocPipelineStage) {
                int err = pscalTtyCheckBackgroundRead(pscal_fd);
                if (err < 0) {
                    pscal_fd_close(pscal_fd);
                    if (err == _EINTR) {
                        vprocDeliverPendingSignalsForCurrent();
                    }
                    return vprocSetCompatErrno(err);
                }
            }
            bool nonblocking = (pscal_fd->flags & O_NONBLOCK) != 0;
            const bool line_mode = gVprocPipelineStage && vprocTtyInLineMode(pscal_fd);
            pscal_fd_close(pscal_fd);
            /* The interrupt key, where a command takes the terminal as it comes.
             * Every read loop stops at the end of its input, and an applet on
             * the shell's thread has no other way out: nothing can kill it, and
             * many loops only read again on EINTR. The shell turns this into the
             * status of a command the key ended. */
            if (line_mode && gVprocTerminalInterrupted) {
                return 0;
            }
            gVprocInterruptKeyRead = false;
            ssize_t res = vprocSessionReadInputShimMode(buf, count, nonblocking);
            if (res < 0 && errno == EINTR && gVprocInterruptKeyRead) {
                if (line_mode) {
                    gVprocTerminalInterrupted = true;
                    return 0;
                }
                if (vprocPromptTakesInterruptKey()) {
                    *(unsigned char *)buf = 0x03;
                    return 1;
                }
            }
            return res;
        }
        if (gVprocPipelineStage &&
            vprocVprocDebugEnabled() &&
            gVprocPipelineReadLogCount < 32) {
            gVprocPipelineReadLogCount++;
            fprintf(stderr, "[vproc-read] fd=%d using pscal=%p count=%zu\n",
                    fd, (void *)pscal_fd, count);
        }
        ssize_t res;
        pthread_cleanup_push(vprocPscalFdCloseCancelled, pscal_fd);
        res = pscal_fd->ops->read(pscal_fd, buf, count);
        pthread_cleanup_pop(0);
        pscal_fd_close(pscal_fd);
        if (res < 0) {
            if ((int)res == _EINTR) {
                vprocDeliverPendingSignalsForCurrent();
            }
            return vprocSetCompatErrno((int)res);
        }
        return res;
    }
    if (host < 0) {
        return -1;
    }
    bool controlling_stdin = (vp && vp->stdin_host_fd >= 0 && fd == STDIN_FILENO && host == vp->stdin_host_fd);
    if (vprocToolDebugEnabled() && fd == STDIN_FILENO) {
        vprocDebugLogf(
                "[vproc-read] stdin host=%d stdin_host=%d controlling=%d from_session=%d\n",
                host,
                vp ? vp->stdin_host_fd : -1,
                (int)controlling_stdin,
                (int)(vp ? vp->stdin_from_session : 0));
    }
    if (controlling_stdin) {
        (void)vprocWaitIfStopped(vp);
    }
    /* stdin_host_fd follows stdin, so it may name a file the job redirected stdin from, or the
     * /dev/null a job starts with. Only the terminal stops a job that reads it: `cat < f &` stopped
     * and never ended. A host build's default session is no terminal, whatever its stdin is. */
    if (controlling_stdin && !gVprocPipelineStage &&
        (vprocHostIsatty(host) ||
         (vp->stdin_from_session && !vprocSessionStdioIsDefault(vprocSessionStdioCurrent()))) &&
        vprocShouldStopForBackgroundTty(vprocCurrent(), SIGTTIN)) {
        errno = EINTR;
        return -1;
    }
#if defined(PSCAL_TARGET_IOS)
    if (controlling_stdin) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session &&
            session->stdin_host_fd >= 0 &&
            vprocSessionFdMatchesHost(host, session->stdin_host_fd)) {
            /* In-process pipeline stages can still be foreground interactive
             * tools/frontends. Route controlling stdin through the shared
             * session buffer so virtual Ctrl-C/Z handling remains consistent. */
            if (vprocToolDebugEnabled()) {
                fprintf(stderr,
                        "[vproc-read] route session stdin host=%d session_stdin=%d\n",
                        host,
                        session->stdin_host_fd);
            }
            return vprocSessionReadInputShimMode(buf, count, false);
        }
    }
    if (vprocHostFdBlocks(host) && vprocWaitHostFd(host, false) < 0) {
        return -1;
    }
#endif
    return vprocHostRead(host, buf, count);
}

static _Thread_local int gVprocStdioWriteError[2];

void vprocStdioSetWriteError(int fd, int err) {
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        gVprocStdioWriteError[fd - STDOUT_FILENO] = err;
    }
}

int vprocStdioWriteError(int fd) {
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        return gVprocStdioWriteError[fd - STDOUT_FILENO];
    }
    return 0;
}

static ssize_t vprocWriteShimImpl(int fd, const void *buf, size_t count, bool raise_sigpipe) {
    /* The interrupt key ended this command's input. On Linux its SIGINT ends the command
     * there, so what it would write after, such as wc's counts, never appears. */
    if (gVprocTerminalInterrupted) {
        return (ssize_t)count;
    }
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = NULL;
    int host = vprocResolveFdForShim(vp, fd, 1, &pscal_fd);
    if (pscal_fd) {
        if (gVprocPipelineStage &&
            vprocVprocDebugEnabled() &&
            gVprocPipelineWriteLogCount < 32) {
            gVprocPipelineWriteLogCount++;
            fprintf(stderr, "[vproc-write] fd=%d using pscal=%p count=%zu\n",
                    fd, (void *)pscal_fd, count);
        }
        ssize_t res;
        pthread_cleanup_push(vprocPscalFdCloseCancelled, pscal_fd);
        res = pscal_fd->ops->write(pscal_fd, buf, count);
        pthread_cleanup_pop(0);
        pscal_fd_close(pscal_fd);
        if (res < 0) {
            if ((int)res == _EINTR) {
                vprocDeliverPendingSignalsForCurrent();
            }
            if ((int)res == _EPIPE) {
                gVprocClosedPipe = true;
                if (raise_sigpipe && vp) {
                    vprocRaiseClosedPipe(vprocPid(vp));
                }
            }
            return vprocSetCompatErrno((int)res);
        }
        return res;
    }
    /* A vproc always starts with its stdio, so an empty one was closed, and nothing below may
     * find it another way out. */
    if (vp && host < 0 && fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        errno = EBADF;
        return -1;
    }
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    bool is_stdout = false;
    bool is_stderr = false;
    vprocSessionResolveOutputFd(session, fd, &is_stdout, &is_stderr);
    bool session_has_virtual = false;
    if (session) {
        session_has_virtual =
            (session->stdout_pscal_fd != NULL) ||
            (session->stderr_pscal_fd != NULL) ||
            (session->pty_slave != NULL);
    }
    bool host_is_tty = (host >= 0 && isatty(host));
    /* Some TUI libraries open /dev/tty directly instead of using fd 1/2.
     * Treat those writes as stdout when a session virtual terminal exists. */
    if (!is_stdout && !is_stderr && session && session_has_virtual && host_is_tty) {
        is_stdout = true;
    }
    int session_host_fd = -1;
    if (is_stdout || is_stderr) {
        session_host_fd = is_stdout ? session->stdout_host_fd : session->stderr_host_fd;
    }
    bool use_session_output = false;
    if ((is_stdout || is_stderr) && session && session_has_virtual) {
        if (host < 0) {
            use_session_output = true;
        } else if (session_host_fd >= 0 &&
                   vprocSessionFdMatchesHost(host, session_host_fd)) {
            use_session_output = true;
        } else if (host_is_tty) {
            use_session_output = true;
        }
    }
    if (use_session_output && session) {
        struct pscal_fd *target =
            is_stdout ? session->stdout_pscal_fd : session->stderr_pscal_fd;
        if (!target) {
            target = session->pty_slave;
        }
        if (target && target->ops && target->ops->write) {
            ssize_t res = target->ops->write(target, buf, count);
            if (res < 0) {
                /* Do not leak interactive session output to host tty/stdout. */
                return vprocSetCompatErrno((int)res);
            }
            return res;
        }
    }
#else
    host = shimTranslate(fd, 1);
#endif
    /* If host resolution failed, fall back to the session stream or host fd so
     * stdout/stderr is not dropped when stdio is virtualised. */
    if (host < 0) {
        if (session && (is_stdout || is_stderr)) {
            struct pscal_fd *fallback =
                is_stdout ? session->stdout_pscal_fd : session->stderr_pscal_fd;
            if (!fallback) {
                fallback = session->pty_slave;
            }
            if (fallback && fallback->ops && fallback->ops->write) {
                ssize_t res = fallback->ops->write(fallback, buf, count);
                if (res < 0) {
                    return vprocSetCompatErrno((int)res);
                }
                return res;
            }
            if (session_host_fd >= 0) {
                host = session_host_fd;
            }
        }
    }
    if (host < 0) {
        host = shimTranslate(fd, 1);
    }
    int write_fd = host;
    if (write_fd < 0 && session_host_fd >= 0) {
        write_fd = session_host_fd;
    }
    if (write_fd < 0) {
        if (session && session_has_virtual) {
            errno = EPIPE;
            return -1;
        }
        int tty_fd = vprocHostOpen("/dev/tty", O_WRONLY);
        if (tty_fd < 0) {
            tty_fd = vprocHostOpen("/dev/tty", O_WRONLY | O_NONBLOCK);
        }
        write_fd = tty_fd;
    }
    if (write_fd < 0) {
        return -1;
    }
    if (vprocToolDebugEnabled()) {
        vprocDebugLogf( "[vwrite] fd=%d -> host=%d write_fd=%d count=%zu\n", fd, host, write_fd, count);
    }
    ssize_t res = vprocHostWriteKillable(write_fd, buf, count);
    if (res < 0 && errno == EPIPE) {
        gVprocClosedPipe = true;
        if (raise_sigpipe && vp) {
            vprocRaiseClosedPipe(vprocPid(vp));
        }
    }
    if (write_fd != host) {
        int saved_errno = errno;
        vprocHostClose(write_fd);
        errno = saved_errno;
    }
    return res;
}

ssize_t vprocWriteShim(int fd, const void *buf, size_t count) {
    return vprocWriteShimImpl(fd, buf, count, true);
}

ssize_t vprocWriteShimNoSignal(int fd, const void *buf, size_t count) {
    return vprocWriteShimImpl(fd, buf, count, false);
}

int vprocDupShim(int fd) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostDup(fd);
    }
    int duped = vprocDup(vp, fd);
    if (duped >= 0) {
        return duped;
    }
    int saved_errno = errno;
    if (saved_errno != EBADF) {
        return -1;
    }
    int host_fd = shimTranslate(fd, 1);
    if (host_fd < 0) {
        errno = saved_errno;
        return -1;
    }
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, cloned);
    if (slot < 0) {
        vprocHostClose(cloned);
        return -1;
    }
    return slot;
}

int vprocDup2Shim(int fd, int target) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostDup2(fd, target);
    }
    if (target >= 0 && (rlim_t)target >= vp->limits[RLIMIT_NOFILE].rlim_cur) {
        errno = EBADF;
        return -1;
    }
    int rc = vprocDup2(vp, fd, target);
    if (rc >= 0) {
        return rc;
    }
    int saved_errno = errno;
    if (saved_errno != EBADF) {
        return -1;
    }
    struct stat st;
    if (vprocHostFstatRaw(fd, &st) != 0) {
        errno = saved_errno;
        return -1;
    }
    return vprocRestoreHostFd(vp, target, fd);
}

static bool vprocHasFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        return false;
    }
    if (!vprocRegistryContainsValidated(vp)) {
        return false;
    }
    bool has = false;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity && vp->entries[fd].kind != VPROC_FD_NONE) {
        has = true;
    }
    pthread_mutex_unlock(&vp->mu);
    return has;
}

int vprocCloseShim(int fd) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostClose(fd);
    }
    if (vprocFdIsTaggedHost(fd)) {
        fd &= ~VPROC_HOST_FD_TAG;
    } else if (vprocHasFd(vp, fd)) {
        return vprocClose(vp, fd);
    }

    bool owns_host_fd = false;
    pthread_mutex_lock(&vp->mu);
    owns_host_fd = vprocOwnsHostFdLocked(vp, fd);
    pthread_mutex_unlock(&vp->mu);
    if (owns_host_fd) {
        return vprocHostClose(fd);
    }

    errno = EBADF;
    return -1;
}

int vprocFsyncShim(int fd) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFsyncRaw(fd);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        return 0;
    }
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostFsyncRaw(host);
}

int vprocPipeShim(int pipefd[2]) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostPipe(pipefd);
    }
    return vprocPipe(vp, pipefd);
}

int vprocSocketShim(int domain, int type, int protocol) {
    vprocDeliverPendingSignalsForCurrent();
    int fd = vprocHostSocket(domain, type, protocol);
    VProc *vp = vprocForThread();
    if (vp && fd >= 0) {
        vprocResourceTrack(vp, fd, VPROC_RESOURCE_SOCKET);
    }
    return fd;
}

int vprocAcceptShim(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    vprocDeliverPendingSignalsForCurrent();
    int res = vprocHostAccept(fd, addr, addrlen);
    VProc *vp = vprocForThread();
    if (vp && res >= 0) {
        vprocResourceTrack(vp, res, VPROC_RESOURCE_SOCKET);
    }
    return res;
}

int vprocSocketpairShim(int domain, int type, int protocol, int sv[2]) {
    vprocDeliverPendingSignalsForCurrent();
    int rc = vprocHostSocketpair(domain, type, protocol, sv);
    VProc *vp = vprocForThread();
    if (rc == 0 && vp) {
        int host0 = sv[0];
        int host1 = sv[1];
        int vfd0 = vprocAdoptHostFd(vp, host0);
        if (vfd0 < 0) {
            int saved_errno = errno;
            (void)vprocHostClose(host0);
            (void)vprocHostClose(host1);
            errno = saved_errno;
            return -1;
        }
        int vfd1 = vprocAdoptHostFd(vp, host1);
        if (vfd1 < 0) {
            int saved_errno = errno;
            (void)vprocClose(vp, vfd0);
            (void)vprocHostClose(host1);
            errno = saved_errno;
            return -1;
        }
        if (getenv("PSCALI_TOOL_DEBUG") != NULL) {
            fprintf(stderr,
                    "[vproc-sockpair] host0=%d host1=%d vfd0=%d vfd1=%d\n",
                    host0,
                    host1,
                    vfd0,
                    vfd1);
        }
        sv[0] = vfd0;
        sv[1] = vfd1;
    }
    return rc;
}

int vprocFstatShim(int fd, struct stat *st) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFstatRaw(fd, st);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        if (st) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFCHR | 0600;
            st->st_nlink = 1;
            if (pscal_fd->ops == &kVprocInprocPipeReadOps || pscal_fd->ops == &kVprocInprocPipeWriteOps) {
                /* A pipeline's pipe: both ends are one file, as on a host. */
                VprocInprocEnd *end = (VprocInprocEnd *)pscal_fd->userdata;
                st->st_mode = S_IFIFO | 0600;
                st->st_ino = (ino_t)(uintptr_t)(end ? end->pipe : NULL);
                st->st_uid = (uid_t)geteuid();
                st->st_gid = (gid_t)getegid();
            }
            if (pscal_fd->tty) {
                st->st_rdev = makedev(pscal_fd->tty->type, pscal_fd->tty->num);
                st->st_ino = (ino_t)(pscal_fd->tty->num + 3);
                st->st_uid = (uid_t)geteuid();
                st->st_gid = (gid_t)getegid();
                if (pscalPtyIsSlave(pscal_fd)) {
                    mode_t_ perms = 0620;
                    uid_t_ uid = 0;
                    gid_t_ gid = 0;
                    if (pscalPtyGetSlaveInfo(pscal_fd->tty->num, &perms, &uid, &gid) == 0) {
                        st->st_mode = S_IFCHR | (mode_t)perms;
                        st->st_uid = (uid_t)uid;
                        st->st_gid = (gid_t)gid;
                    } else {
                        st->st_mode = S_IFCHR | 0620;
                    }
                }
            }
        }
        pscal_fd_close(pscal_fd);
        return 0;
    }
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostFstatRaw(host, st);
}

#if defined(PSCAL_TARGET_IOS)
/* A terminal is on devpts and an in-process pipe on pipefs, as Linux reports them; neither has a
 * host descriptor to ask. */
static void vprocFstatfsPscal(struct pscal_fd *pscal_fd, struct statfs *buf) {
    memset(buf, 0, sizeof(*buf));
    bool tty = pscal_fd->tty != NULL;
    buf->f_bsize = 4096;
    buf->f_iosize = 4096;
    buf->f_type = tty ? 0x1CD1 : 0x50495045;
    snprintf(buf->f_fstypename, sizeof(buf->f_fstypename), "%s", tty ? "devpts" : "pipefs");
    snprintf(buf->f_mntfromname, sizeof(buf->f_mntfromname), "%s", tty ? "devpts" : "pipefs");
    snprintf(buf->f_mntonname, sizeof(buf->f_mntonname), "%s", tty ? "/dev/pts" : "/");
}

/* The host's figures for the file under the descriptor, named from the app's mount table. */
int vprocFstatfsShim(int fd, struct statfs *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = vp ? vprocGetPscalFd(vp, fd) : NULL;
    if (pscal_fd) {
        vprocFstatfsPscal(pscal_fd, buf);
        pscal_fd_close(pscal_fd);
        return 0;
    }
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    if (fstatfs(host, buf) != 0) {
        return -1;
    }
    char path[PATH_MAX];
    bool named = false;
#if defined(F_GETPATH)
    named = fcntl(host, F_GETPATH, path) == 0;
#endif
    pscalPathVirtualized_nameStatfs(named ? path : NULL, buf);
    return 0;
}

int vprocFstatvfsShim(int fd, struct statvfs *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    struct statfs sfs;
    if (vprocFstatfsShim(fd, &sfs) != 0) {
        return -1;
    }
    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = vp ? vprocGetPscalFd(vp, fd) : NULL;
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        memset(buf, 0, sizeof(*buf));
        buf->f_bsize = buf->f_frsize = sfs.f_bsize;
        buf->f_namemax = 255;
        return 0;
    }
    int host = shimTranslate(fd, 1);
    if (host < 0 || fstatvfs(host, buf) != 0) {
        return -1;
    }
    if (sfs.f_blocks == 0 && strcmp(sfs.f_fstypename, "proc") == 0) {
        buf->f_blocks = buf->f_bfree = buf->f_bavail = 0;
        buf->f_files = buf->f_ffree = buf->f_favail = 0;
    }
    return 0;
}
#endif

static int vprocStatFillChar(struct stat *st,
                             mode_t mode,
                             dev_t rdev,
                             ino_t ino,
                             uid_t uid,
                             gid_t gid) {
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | (mode & 0777);
    st->st_nlink = 1;
    st->st_rdev = rdev;
    st->st_ino = ino;
    st->st_uid = uid;
    st->st_gid = gid;
    return 0;
}

static int vprocStatDevptsRoot(struct stat *st) {
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 1;
    st->st_ino = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    return 0;
}

static int vprocStatPtySlave(int pty_num, struct stat *st) {
    mode_t_ perms = 0620;
    uid_t_ uid = 0;
    gid_t_ gid = 0;
    if (pscalPtyGetSlaveInfo(pty_num, &perms, &uid, &gid) != 0) {
        errno = ENOENT;
        return -1;
    }
    return vprocStatFillChar(st,
                             (mode_t)perms,
                             makedev(TTY_PSEUDO_SLAVE_MAJOR, pty_num),
                             (ino_t)(pty_num + 3),
                             (uid_t)uid,
                             (gid_t)gid);
}

static int vprocStatShimInternal(const char *path, struct stat *st, bool follow) {
    if (!path || !st) {
        errno = EINVAL;
        return -1;
    }
    VProc *vp = vprocForThread();
    if (!vp) {
        return follow ? vprocHostStatRaw(path, st) : vprocHostLstatRaw(path, st);
    }
    /* lstat goes on to the link itself, under the root's /dev and /proc. */
    int named_pid = 0;
    int named_fd = follow ? vprocPathNamedFdOf(path, &named_pid) : -1;
    if (named_fd >= 0) {
        if (named_pid != vprocPid(vp)) {
            int copy = vprocCopyFdOfTask(vp, named_pid, named_fd);
            if (copy < 0) {
                return -1;
            }
            int rc = vprocFstatShim(copy, st);
            int err = errno;
            vprocCloseShim(copy);
            errno = err;
            return rc;
        }
        return vprocNamedFdOpen(vp, named_fd) ? vprocFstatShim(named_fd, st) : -1;
    }
    char virtual_path_buf[PATH_MAX];
    if (!vprocResolveVirtualPath(vp, path, virtual_path_buf, sizeof(virtual_path_buf))) {
        return -1;
    }
    const char *virtual_path = virtual_path_buf;
    if (vprocPathIsSystemPath(virtual_path)) {
        return follow ? vprocHostStatRaw(virtual_path, st) : vprocHostLstatRaw(virtual_path, st);
    }
    if (vprocPathIsDevPtsRoot(virtual_path)) {
        return vprocStatDevptsRoot(st);
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(virtual_path, &pty_num)) {
        return vprocStatPtySlave(pty_num, st);
    }
    if (vprocPathIsDevTty(virtual_path)) {
        return vprocStatFillChar(st,
                                 0666,
                                 makedev(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR),
                                 2,
                                 0,
                                 0);
    }
    if (vprocPathIsDevConsole(virtual_path)) {
        return vprocStatFillChar(st,
                                 0666,
                                 makedev(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR),
                                 3,
                                 0,
                                 0);
    }
    if (vprocPathIsPtyMaster(virtual_path)) {
        return vprocStatFillChar(st,
                                 0666,
                                 makedev(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR),
                                 4,
                                 0,
                                 0);
    }
    int tty_num = -1;
    if (vprocPathParseConsoleTty(virtual_path, &tty_num)) {
        if (tty_num >= 1 && tty_num <= 7) {
            return vprocStatFillChar(st,
                                     0666,
                                     makedev(TTY_CONSOLE_MAJOR, tty_num),
                                     (ino_t)(10 + tty_num),
                                     0,
                                     0);
        }
        errno = ENOENT;
        return -1;
    }
    int rc = follow ? vprocHostStatVirtualized(virtual_path, st)
                    : vprocHostLstatVirtualized(virtual_path, st);
#if defined(PSCAL_TARGET_IOS)
    if (rc == 0) {
        char owner_expanded[PATH_MAX];
        const char *host = vprocPathExpandForShim(virtual_path, owner_expanded, sizeof(owner_expanded));
        vprocHostPathOwner(host ? host : virtual_path, st);
    }
#endif
    return rc;
}

int vprocStatShim(const char *path, struct stat *st) {
    return vprocStatShimInternal(path, st, true);
}

int vprocLstatShim(const char *path, struct stat *st) {
    return vprocStatShimInternal(path, st, false);
}

static VProc *vprocResolvePathOpsVproc(bool prefer_shell) {
    VProc *vp = NULL;
    if (prefer_shell) {
        vp = vprocResolveShellVprocForPathOps();
        if (!vp) {
            vp = vprocForThread();
        }
        return vp;
    }
    vp = vprocForThread();
    if (!vp) {
        vp = vprocResolveShellVprocForPathOps();
    }
    return vp;
}

static int vprocChdirForVproc(VProc *vp, const char *path) {
    char virtual_path[PATH_MAX];
    if (!vprocResolveVirtualPath(vp, path, virtual_path, sizeof(virtual_path))) {
        return -1;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(virtual_path, expanded, sizeof(expanded));
    if (!target) {
        errno = ENOENT;
        return -1;
    }
    struct stat st;
    if (vprocHostStatRaw(target, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    vprocSetVirtualCwd(vp, virtual_path);
    return 0;
}

static char *vprocGetcwdForVproc(VProc *vp, char *buffer, size_t size) {
    char cwd[PATH_MAX];
    vprocCurrentVirtualCwd(vp, cwd, sizeof(cwd));
    size_t len = strlen(cwd);
    bool allocated = false;
    if (!buffer) {
        if (size == 0) {
            size = len + 1;
        }
        buffer = (char *)malloc(size);
        if (!buffer) {
            errno = ENOMEM;
            return NULL;
        }
        allocated = true;
    }
    if (size == 0 || len + 1 > size) {
        if (allocated) {
            free(buffer);
        }
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, cwd, len + 1);
    return buffer;
}

struct VProcEnv {
    int refcount;
    pthread_mutex_t mu;
    char **view;    /* NULL-terminated, the shape of environ */
    size_t *caps;   /* each entry's allocation */
    size_t count;
    size_t capacity;
    /* getenv's result and an iterated view stay valid well past what POSIX asks, and a value that
     * fits overwrites in place. A tab's shell has one of these for days, so only so many stay. */
    void **retired;
    size_t retired_count;
    size_t retired_capacity;
    void *attachment;
    void (*free_attachment)(void *);
};

static _Thread_local VProcEnv *gVprocEnvTls = NULL;
/* libc's environment is the whole app's, and libc locks only its own calls. A capture walks it,
 * so that and what changes it here hold this. */
static pthread_mutex_t gVprocHostEnvMu = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int gVprocSubshellPid = 0;
static _Thread_local bool gVprocClosedPipe = false;
extern char **environ;

#define VPROC_ENV_RETIRED_MAX 4096

static void vprocEnvRetireLocked(VProcEnv *env, void *ptr) {
    if (env->retired_count == VPROC_ENV_RETIRED_MAX) {
        /* A function's `local` retires an entry with every call. */
        size_t dropped = VPROC_ENV_RETIRED_MAX / 2;
        for (size_t i = 0; i < dropped; ++i) {
            free(env->retired[i]);
        }
        memmove(env->retired, env->retired + dropped, (env->retired_count - dropped) * sizeof(void *));
        env->retired_count -= dropped;
    }
    if (env->retired_count == env->retired_capacity) {
        size_t cap = env->retired_capacity ? env->retired_capacity * 2 : 16;
        void **resized = (void **)realloc(env->retired, cap * sizeof(void *));
        if (!resized) {
            return;
        }
        env->retired = resized;
        env->retired_capacity = cap;
    }
    env->retired[env->retired_count++] = ptr;
}

static bool vprocEnvReserveLocked(VProcEnv *env, size_t needed) {
    if (needed + 1 <= env->capacity) {
        return true;
    }
    size_t cap = env->capacity ? env->capacity * 2 : 64;
    while (cap < needed + 1) {
        cap *= 2;
    }
    char **view = (char **)calloc(cap, sizeof(char *));
    size_t *caps = (size_t *)calloc(cap, sizeof(size_t));
    if (!view || !caps) {
        free(view);
        free(caps);
        return false;
    }
    if (env->count > 0) {
        memcpy(view, env->view, env->count * sizeof(char *));
        memcpy(caps, env->caps, env->count * sizeof(size_t));
    }
    if (env->view) {
        vprocEnvRetireLocked(env, env->view);
    }
    free(env->caps);
    env->view = view;
    env->caps = caps;
    env->capacity = cap;
    return true;
}

static ssize_t vprocEnvFindLocked(const VProcEnv *env, const char *name, size_t name_len) {
    for (size_t i = 0; i < env->count; ++i) {
        const char *entry = env->view[i];
        if (entry && strncmp(entry, name, name_len) == 0 && entry[name_len] == '=') {
            return (ssize_t)i;
        }
    }
    return -1;
}

char **vprocEnvEntries(VProcEnv *env) {
    return env ? env->view : NULL;
}

VProcEnv *vprocEnvCapture(void) {
    return vprocEnvCaptureFiltered(NULL, NULL);
}

/* Darwin's basename and dirname answer in one buffer each for the whole process, so one `dirname`
 * could print what another one running beside it asked for. A thread gets buffers of its own when
 * it first asks, on the heap, as an array in thread-local storage would cost every thread. */
typedef struct {
    char base[MAXPATHLEN];
    char dir[MAXPATHLEN];
} VProcPathNames;

static pthread_key_t gVprocPathNamesKey;
static pthread_once_t gVprocPathNamesOnce = PTHREAD_ONCE_INIT;

static void vprocPathNamesMakeKey(void) {
    (void)pthread_key_create(&gVprocPathNamesKey, free);
}

static VProcPathNames *vprocPathNamesForThread(void) {
    (void)pthread_once(&gVprocPathNamesOnce, vprocPathNamesMakeKey);
    VProcPathNames *names = (VProcPathNames *)pthread_getspecific(gVprocPathNamesKey);
    if (!names) {
        names = (VProcPathNames *)malloc(sizeof(VProcPathNames));
        if (names && pthread_setspecific(gVprocPathNamesKey, names) != 0) {
            free(names);
            names = NULL;
        }
    }
    return names;
}

char *vprocBasenameShim(char *path) {
#if defined(__APPLE__)
    VProcPathNames *names = vprocPathNamesForThread();
    return names ? basename_r(path, names->base) : NULL;
#else
    return basename(path);
#endif
}

char *vprocDirnameShim(char *path) {
#if defined(__APPLE__)
    VProcPathNames *names = vprocPathNamesForThread();
    return names ? dirname_r(path, names->dir) : NULL;
#else
    return dirname(path);
#endif
}

#if defined(__APPLE__)
/* The locale a thread made, freed when it makes another or ends. A thread using a locale holds
 * its own reference in libc, so this can go while that is still in use. */
static pthread_key_t gVprocLocaleKey;
static pthread_once_t gVprocLocaleOnce = PTHREAD_ONCE_INIT;
static pthread_mutex_t gVprocLocaleMu = PTHREAD_MUTEX_INITIALIZER;

static void vprocLocaleThreadEnd(void *locale) {
    (void)freelocale((locale_t)locale);
}

static void vprocLocaleMakeKey(void) {
    (void)pthread_key_create(&gVprocLocaleKey, vprocLocaleThreadEnd);
}

/* From here NEXT is this thread's to free, unless it is LC_GLOBAL_LOCALE. */
static void vprocLocaleInstall(locale_t next) {
    (void)pthread_once(&gVprocLocaleOnce, vprocLocaleMakeKey);
    locale_t previous = (locale_t)pthread_getspecific(gVprocLocaleKey);
    (void)uselocale(next);
    (void)pthread_setspecific(gVprocLocaleKey, next == LC_GLOBAL_LOCALE ? NULL : next);
    if (previous) {
        (void)freelocale(previous);
    }
}

static int vprocLocaleMask(int category, const char **env_name) {
    *env_name = NULL;
    switch (category) {
        case LC_ALL:
            return LC_ALL_MASK;
        case LC_COLLATE:
            *env_name = "LC_COLLATE";
            return LC_COLLATE_MASK;
        case LC_CTYPE:
            *env_name = "LC_CTYPE";
            return LC_CTYPE_MASK;
        case LC_MESSAGES:
            *env_name = "LC_MESSAGES";
            return LC_MESSAGES_MASK;
        case LC_MONETARY:
            *env_name = "LC_MONETARY";
            return LC_MONETARY_MASK;
        case LC_NUMERIC:
            *env_name = "LC_NUMERIC";
            return LC_NUMERIC_MASK;
        case LC_TIME:
            *env_name = "LC_TIME";
            return LC_TIME_MASK;
        default:
            return 0;
    }
}
#endif

char *vprocSetlocaleShim(int category, const char *name) {
#if defined(__APPLE__)
    const char *env_name = NULL;
    int mask = vprocLocaleMask(category, &env_name);
    if (mask == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (name) {
        if (!*name) {
            /* The program's environment, which libc's may not be. */
            const char *value = vprocGetenvShim("LC_ALL");
            if ((!value || !*value) && env_name) {
                value = vprocGetenvShim(env_name);
            }
            if (!value || !*value) {
                value = vprocGetenvShim("LANG");
            }
            name = (value && *value && !strchr(value, '/')) ? value : "C";
        }
        pthread_mutex_lock(&gVprocLocaleMu);
        locale_t made = newlocale(mask, name, uselocale((locale_t)0));
        pthread_mutex_unlock(&gVprocLocaleMu);
        if (!made) {
            return NULL;
        }
        vprocLocaleInstall(made);
    }
    return (char *)querylocale(mask, uselocale((locale_t)0));
#else
    return setlocale(category, name);
#endif
}

void *vprocLocaleLoad(int mask, const char *name) {
#if defined(__APPLE__)
    pthread_mutex_lock(&gVprocLocaleMu);
    locale_t made = newlocale(mask, name, (locale_t)0);
    pthread_mutex_unlock(&gVprocLocaleMu);
    return made;
#else
    return newlocale(mask, name, (locale_t)0);
#endif
}

void *vprocLocaleSave(void) {
#if defined(__APPLE__)
    locale_t current = uselocale((locale_t)0);
    /* A copy of its own, as a program inside this one may free what is current. */
    return current == LC_GLOBAL_LOCALE ? (void *)LC_GLOBAL_LOCALE : (void *)duplocale(current);
#else
    return NULL;
#endif
}

void vprocLocaleRestore(void *saved) {
#if defined(__APPLE__)
    locale_t locale = (locale_t)saved;
    if (locale && (locale != LC_GLOBAL_LOCALE || uselocale((locale_t)0) != LC_GLOBAL_LOCALE)) {
        vprocLocaleInstall(locale);
    }
#else
    (void)saved;
#endif
}

static VProcEnv *vprocEnvCopyView(char **source, bool (*keep)(const char *entry, void *arg), void *arg) {
    VProcEnv *env = (VProcEnv *)calloc(1, sizeof(VProcEnv));
    if (!env) {
        return NULL;
    }
    env->refcount = 1;
    pthread_mutex_init(&env->mu, NULL);
    size_t count = 0;
    while (source && source[count]) {
        count++;
    }
    if (!vprocEnvReserveLocked(env, count)) {
        vprocEnvRelease(env);
        return NULL;
    }
    /* The NULL is for what the lock can't cover: libc's own callers, in the system libraries. */
    for (size_t i = 0; i < count && source[i]; ++i) {
        if (keep && !keep(source[i], arg)) {
            continue;
        }
        char *copy = strdup(source[i]);
        if (!copy) {
            vprocEnvRelease(env);
            return NULL;
        }
        env->view[env->count] = copy;
        env->caps[env->count] = strlen(copy) + 1;
        env->count++;
    }
    return env;
}

VProcEnv *vprocEnvCaptureFiltered(bool (*keep)(const char *entry, void *arg), void *arg) {
    VProcEnv *current = gVprocEnvTls;
    pthread_mutex_t *mu = current ? &current->mu : &gVprocHostEnvMu;
    pthread_mutex_lock(mu);
    VProcEnv *env = vprocEnvCopyView(*vprocEnvironShim(), keep, arg);
    pthread_mutex_unlock(mu);
    return env;
}

VProcEnv *vprocEnvRetain(VProcEnv *env) {
    if (env) {
        __sync_add_and_fetch(&env->refcount, 1);
    }
    return env;
}

void vprocEnvRelease(VProcEnv *env) {
    if (!env || __sync_sub_and_fetch(&env->refcount, 1) != 0) {
        return;
    }
    for (size_t i = 0; i < env->count; ++i) {
        free(env->view[i]);
    }
    for (size_t i = 0; i < env->retired_count; ++i) {
        free(env->retired[i]);
    }
    free(env->retired);
    free(env->view);
    free(env->caps);
    if (env->free_attachment) {
        env->free_attachment(env->attachment);
    }
    pthread_mutex_destroy(&env->mu);
    free(env);
}

void *vprocEnvAttachment(VProcEnv *env) {
    return env ? env->attachment : NULL;
}

void vprocEnvSetAttachment(VProcEnv *env, void *data, void (*free_data)(void *)) {
    if (env) {
        env->attachment = data;
        env->free_attachment = free_data;
    }
}

VProcEnv *vprocEnvCurrent(void) {
    return gVprocEnvTls;
}

VProcEnv *vprocEnvActivate(VProcEnv *env) {
    VProcEnv *previous = gVprocEnvTls;
    gVprocEnvTls = env;
    return previous;
}

char *vprocGetenvShim(const char *name) {
    VProcEnv *env = gVprocEnvTls;
    if (!env) {
        return getenv(name);
    }
    if (!name) {
        return NULL;
    }
    size_t name_len = strlen(name);
    char *value = NULL;
    pthread_mutex_lock(&env->mu);
    ssize_t index = vprocEnvFindLocked(env, name, name_len);
    if (index >= 0) {
        value = env->view[index] + name_len + 1;
    }
    pthread_mutex_unlock(&env->mu);
    return value;
}

int vprocSetenvShim(const char *name, const char *value, int overwrite) {
    VProcEnv *env = gVprocEnvTls;
    if (!env) {
        pthread_mutex_lock(&gVprocHostEnvMu);
        int result = setenv(name, value, overwrite);
        pthread_mutex_unlock(&gVprocHostEnvMu);
        return result;
    }
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    const char *text = value ? value : "";
    size_t name_len = strlen(name);
    size_t needed = name_len + 1 + strlen(text) + 1;
    int rc = 0;
    pthread_mutex_lock(&env->mu);
    ssize_t index = vprocEnvFindLocked(env, name, name_len);
    if (index >= 0 && !overwrite) {
        pthread_mutex_unlock(&env->mu);
        return 0;
    }
    if (index >= 0 && needed <= env->caps[index]) {
        char *entry = env->view[index];
        memmove(entry + name_len + 1, text, needed - name_len - 1);
        pthread_mutex_unlock(&env->mu);
        return 0;
    }
    size_t cap = (index >= 0) ? env->caps[index] * 2 : needed;
    if (cap < needed) {
        cap = needed;
    }
    char *entry = (char *)malloc(cap);
    if (!entry || (index < 0 && !vprocEnvReserveLocked(env, env->count + 1))) {
        free(entry);
        errno = ENOMEM;
        rc = -1;
    } else {
        snprintf(entry, cap, "%s=%s", name, text);
        if (index >= 0) {
            vprocEnvRetireLocked(env, env->view[index]);
            env->view[index] = entry;
            env->caps[index] = cap;
        } else {
            env->caps[env->count] = cap;
            env->view[env->count + 1] = NULL;
            env->view[env->count] = entry;
            env->count++;
        }
    }
    pthread_mutex_unlock(&env->mu);
    return rc;
}

int vprocUnsetenvShim(const char *name) {
    VProcEnv *env = gVprocEnvTls;
    if (!env) {
        pthread_mutex_lock(&gVprocHostEnvMu);
        int result = unsetenv(name);
        pthread_mutex_unlock(&gVprocHostEnvMu);
        return result;
    }
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    size_t name_len = strlen(name);
    pthread_mutex_lock(&env->mu);
    ssize_t index = vprocEnvFindLocked(env, name, name_len);
    if (index >= 0) {
        vprocEnvRetireLocked(env, env->view[index]);
        for (size_t i = (size_t)index; i + 1 < env->count; ++i) {
            env->view[i] = env->view[i + 1];
            env->caps[i] = env->caps[i + 1];
        }
        env->count--;
        env->view[env->count] = NULL;
    }
    pthread_mutex_unlock(&env->mu);
    return 0;
}

char ***vprocEnvironShim(void) {
    return gVprocEnvTls ? &gVprocEnvTls->view : &environ;
}

int vprocSubshellPid(void) {
    return gVprocSubshellPid;
}

void vprocSetSubshellPid(int pid) {
    gVprocSubshellPid = pid;
}

bool vprocConsumeClosedPipe(void) {
    bool closed = gVprocClosedPipe;
    gVprocClosedPipe = false;
    return closed;
}

int vprocChdirShim(const char *path) {
    VProc *vp = vprocResolvePathOpsVproc(false);
    if (!vp) {
        return vprocHostChdirRaw(path);
    }
    return vprocChdirForVproc(vp, path);
}

char *vprocGetcwdShim(char *buffer, size_t size) {
    VProc *vp = vprocResolvePathOpsVproc(false);
    if (!vp) {
        return vprocHostGetcwdRaw(buffer, size);
    }
    return vprocGetcwdForVproc(vp, buffer, size);
}

/* A builtin, and a file it sources, each run under a vproc stacked on this thread, and
 * what they start inherits its cwd from the top of that stack. So cd has to reach all of
 * them, down to the vproc that owns the shell's directory, or a script's next rm acts on
 * the old directory while its pwd reports the new one. */
static void vprocCwdFollowOwner(VProc *owner) {
    char cwd[PATH_MAX];
    vprocCurrentVirtualCwd(owner, cwd, sizeof(cwd));
    VProc *vp = gVProcCurrent;
    size_t depth = gVProcStackDepth;
    while (vp && vp != owner) {
        if (vprocRegistryContains(vp)) {
            vprocSetVirtualCwd(vp, cwd);
        }
        vp = (depth > 0) ? *vprocStackSlot(--depth) : NULL;
    }
}

/* `exec 3> f` in a sourced file is the shell's, as its cd is: the slot goes down the same
 * stack, or it would close with the vproc the file ran under. */
void vprocShellFdFollow(int fd) {
    VProc *from = vprocForThread();
    VProc *owner = vprocResolveShellVprocForPathOps();
    if (!from || from == owner) {
        return;
    }
    VProc *vp = from;
    size_t depth = gVProcStackDepth;
    while (vp != owner && depth > 0) {
        vp = *vprocStackSlot(--depth);
        if (vp && vprocRegistryContains(vp)) {
            vprocCopyFdEntry(from, vp, fd);
        }
    }
}

int vprocShellChdirShim(const char *path) {
    VProc *vp = vprocResolvePathOpsVproc(true);
    if (!vp) {
        return vprocHostChdirRaw(path);
    }
    int rc = vprocChdirForVproc(vp, path);
    if (rc == 0) {
        vprocCwdFollowOwner(vp);
    }
    return rc;
}

/* What is left of a mode after `mask`, and whether the host's mask would take away more. */
static bool vprocModeApplyMask(mode_t mask, mode_t *mode) {
    *mode &= ~mask;
    return (*mode & vprocHostMaskGet()) != 0;
}

static bool vprocCreateModeApply(mode_t *mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return false;
    }
    return vprocModeApplyMask(vp->file_mask, mode);
}

mode_t vprocUmaskShim(mode_t mask) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostMaskSet(mask & 0777);
    }
    mode_t previous = vp->file_mask;
    vp->file_mask = mask & 0777;
    return previous;
}

/* The shell's, down the stack of vprocs its builtins run under, as cd's goes. A subshell's or a
 * job's stays with it. The session shell's is the host's too. */
mode_t vprocShellUmaskShim(mode_t mask) {
    VProc *owner = vprocResolvePathOpsVproc(true);
    if (!owner) {
        return vprocHostMaskSet(mask & 0777);
    }
    mode_t previous = owner->file_mask;
    owner->file_mask = mask & 0777;
    VProc *vp = gVProcCurrent;
    size_t depth = gVProcStackDepth;
    while (vp && vp != owner) {
        if (vprocRegistryContains(vp)) {
            vp->file_mask = mask & 0777;
        }
        vp = (depth > 0) ? *vprocStackSlot(--depth) : NULL;
    }
    if (gVprocSubshellPid <= 0) {
        vprocHostMaskSet(mask & 0777);
    }
    return previous;
}

int vprocGetrlimitShim(int resource, struct rlimit *limit) {
    if (resource < 0 || resource >= RLIM_NLIMITS || !limit) {
        errno = EINVAL;
        return -1;
    }
    VProc *vp = vprocForThread();
    if (!vp) {
        return getrlimit(resource, limit);
    }
    pthread_mutex_lock(&vp->mu);
    *limit = vp->limits[resource];
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

static void vprocLimitStore(VProc *vp, int resource, const struct rlimit *limit) {
    pthread_mutex_lock(&vp->mu);
    vp->limits[resource] = *limit;
    pthread_mutex_unlock(&vp->mu);
}

/* A hard limit only comes down: nothing in the app runs as root. */
int vprocShellSetrlimitShim(int resource, const struct rlimit *limit, bool restore) {
    if (resource < 0 || resource >= RLIM_NLIMITS || !limit) {
        errno = EINVAL;
        return -1;
    }
    VProc *owner = vprocResolvePathOpsVproc(true);
    if (!owner) {
        return setrlimit(resource, limit);
    }
    if (!restore) {
        if (limit->rlim_cur > limit->rlim_max) {
            errno = EINVAL;
            return -1;
        }
        struct rlimit current;
        pthread_mutex_lock(&owner->mu);
        current = owner->limits[resource];
        pthread_mutex_unlock(&owner->mu);
        if (limit->rlim_max > current.rlim_max) {
            errno = EPERM;
            return -1;
        }
    }
    vprocLimitStore(owner, resource, limit);
    VProc *vp = gVProcCurrent;
    size_t depth = gVProcStackDepth;
    while (vp && vp != owner) {
        if (vprocRegistryContains(vp)) {
            vprocLimitStore(vp, resource, limit);
        }
        vp = (depth > 0) ? *vprocStackSlot(--depth) : NULL;
    }
    return 0;
}

bool vprocFileMaskIsHostMask(void) {
    VProc *vp = vprocForThread();
    return !vp || vp->file_mask == vprocHostMaskGet();
}

/* What makes a directory, a FIFO or a node gets no descriptor, so it has only the name to put
 * back what the host took away. These fail if the name is already there, so it is theirs. */
int vprocHostMkdirMasked(const char *path, mode_t mode) {
    bool restore = vprocCreateModeApply(&mode);
    int rc = vprocHostMkdirRaw(path, mode);
    if (rc == 0 && restore) {
        (void)vprocHostChmodRaw(path, mode);
    }
    return rc;
}

int vprocHostMkfifoMasked(const char *path, mode_t mode) {
    if (vprocHostPathPermit(path, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    bool restore = vprocCreateModeApply(&mode);
    int rc = (mkfifo)(path, mode);
    if (rc == 0 && restore) {
        (void)vprocHostChmodRaw(path, mode);
    }
    return rc;
}

int vprocHostMknodMasked(const char *path, mode_t mode, dev_t dev) {
    /* A device node is root's to make. */
    if ((S_ISCHR(mode) || S_ISBLK(mode)) && vprocGeteuidShim() != 0) {
        errno = EPERM;
        return -1;
    }
    if (vprocHostPathPermit(path, VPROC_PERM_ENTRY) != 0) {
        return -1;
    }
    mode_t type = mode & S_IFMT;
    mode_t perms = mode & ~(mode_t)S_IFMT;
    bool restore = vprocCreateModeApply(&perms);
    int rc = (mknod)(path, type | perms, dev);
    if (rc == 0 && restore) {
        (void)vprocHostChmodRaw(path, perms);
    }
    return rc;
}

int vprocHostOpenMasked(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return vprocHostOpenRawInternal(path, flags, mode, (flags & O_CREAT) != 0);
}

int vprocHostOpenatMasked(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (dirfd == AT_FDCWD || (path && path[0] == '/')) {
        return vprocHostOpenRawInternal(path, flags, mode, (flags & O_CREAT) != 0);
    }
    if ((flags & O_CREAT) && vprocCreateModeApply(&mode)) {
        /* As vprocHostOpenRawInternal: only O_EXCL says the file is new. */
        for (int tries = 0; tries < 4; ++tries) {
            int fd = (openat)(dirfd, path, flags | O_EXCL, mode);
            if (fd >= 0) {
                (void)vprocHostFchmodRaw(fd, mode);
                return fd;
            }
            if (errno != EEXIST || (flags & O_EXCL)) {
                return -1;
            }
            fd = (openat)(dirfd, path, flags & ~O_CREAT);
            if (fd >= 0 || errno != ENOENT) {
                return fd;
            }
        }
    }
    return (flags & O_CREAT) ? (openat)(dirfd, path, flags, mode) : (openat)(dirfd, path, flags);
}

int vprocHostMkdiratMasked(int dirfd, const char *path, mode_t mode) {
    if (dirfd == AT_FDCWD || (path && path[0] == '/')) {
        return vprocHostMkdirMasked(path, mode);
    }
    bool restore = vprocCreateModeApply(&mode);
    int rc = (mkdirat)(dirfd, path, mode);
    if (rc == 0 && restore) {
        (void)fchmodat(dirfd, path, mode, 0);
    }
    return rc;
}

/* A thread with no task, such as one of the Go runtime's, makes files for the task that started
 * the program it runs: `mask` is that task's. Only O_EXCL says the file is new, so a file already
 * there keeps its mode. A negative mask is the host's own: the plain call. */
int vprocHostOpenatWithMask(int mask, int dirfd, const char *path, int flags, mode_t mode) {
    if (!(flags & O_CREAT)) {
        return (openat)(dirfd, path, flags);
    }
    if (mask >= 0 && vprocModeApplyMask((mode_t)mask & 0777, &mode)) {
        for (int tries = 0; tries < 4; ++tries) {
            int fd = (openat)(dirfd, path, flags | O_EXCL, mode);
            if (fd >= 0) {
                (void)vprocHostFchmodRaw(fd, mode);
                return fd;
            }
            if (errno != EEXIST || (flags & O_EXCL)) {
                return -1;
            }
            fd = (openat)(dirfd, path, flags & ~O_CREAT);
            if (fd >= 0 || errno != ENOENT) {
                return fd;
            }
        }
    }
    return (openat)(dirfd, path, flags, mode);
}

int vprocHostMkdiratWithMask(int mask, int dirfd, const char *path, mode_t mode) {
    bool restore = mask >= 0 && vprocModeApplyMask((mode_t)mask & 0777, &mode);
    int rc = (mkdirat)(dirfd, path, mode);
    if (rc == 0 && restore) {
        (void)fchmodat(dirfd, path, mode, 0);
    }
    return rc;
}

int vprocFopenModeFlags(const char *mode) {
    if (!mode) {
        return O_RDONLY;
    }
    int flags = O_RDONLY;
    if (mode[0] == 'w' || mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | (mode[0] == 'w' ? O_TRUNC : O_APPEND);
    }
    if (strchr(mode, '+')) {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    }
    return flags;
}

/* libc's fopen makes a file with the host's mask. When the task's differs, the stream is opened
 * on a descriptor that obeys the task's, as fopen's own would be: a host one. */
FILE *vprocHostFopenMasked(const char *path, const char *mode) {
    if (!mode || (mode[0] != 'w' && mode[0] != 'a') || vprocFileMaskIsHostMask()) {
        if (vprocHostPathPermitOpen(path, vprocFopenModeFlags(mode)) != 0) {
            return NULL;
        }
        return (fopen)(path, mode);
    }
    int flags = O_CREAT | (mode[0] == 'w' ? O_TRUNC : O_APPEND);
    flags |= strchr(mode, '+') ? O_RDWR : O_WRONLY;
    if (strchr(mode, 'x')) {
        flags |= O_EXCL;
    }
    if (strchr(mode, 'e')) {
        flags |= O_CLOEXEC;
    }
    int fd = vprocHostOpenRawInternal(path, flags, 0666, true);
    if (fd < 0) {
        return NULL;
    }
    FILE *stream = fdopen(fd, mode);
    if (!stream) {
        int saved_errno = errno;
        (void)vprocHostCloseRaw(fd);
        errno = saved_errno;
    }
    return stream;
}

/* The app's names: relative to the task's directory, and absolute ones under its root. */
int vprocMkfifoShim(const char *path, mode_t mode) {
    return pscalPathVirtualized_mkfifo(path, mode);
}

int vprocMknodShim(const char *path, mode_t mode, dev_t dev) {
    return pscalPathVirtualized_mknod(path, mode, dev);
}

char *vprocShellGetcwdShim(char *buffer, size_t size) {
    VProc *vp = vprocResolvePathOpsVproc(true);
    if (!vp) {
        return vprocHostGetcwdRaw(buffer, size);
    }
    return vprocGetcwdForVproc(vp, buffer, size);
}

int vprocAccessShim(const char *path, int mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostAccessRaw(path, mode);
    }
    /* What the link leads to answers, not how the descriptor was opened. */
    int named_fd = vprocPathNamedFd(path);
    if (named_fd >= 0) {
        struct stat st;
        if (vprocStatShim(path, &st) != 0) {
            return -1;
        }
        if (((mode & R_OK) && !(st.st_mode & S_IRUSR)) ||
            ((mode & W_OK) && !(st.st_mode & S_IWUSR)) ||
            ((mode & X_OK) && !(st.st_mode & S_IXUSR))) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostAccessRaw(target ? target : path, mode);
}

int vprocChmodShim(const char *path, mode_t mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostChmodRaw(path, mode);
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        int err = vprocPtyApplyAttrsByNum(pty_num, &mode, NULL, NULL);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostChmodRaw(target ? target : path, mode);
}

int vprocChownShim(const char *path, uid_t uid, gid_t gid) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostChownRaw(path, uid, gid);
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        int err = vprocPtyApplyAttrsByNum(pty_num, NULL, &uid, &gid);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocChownRecord(target ? target : path, uid, gid, true);
}

int vprocFchmodShim(int fd, mode_t mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFchmodRaw(fd, mode);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        int err = vprocPtyApplyAttrsForFd(pscal_fd, &mode, NULL, NULL);
        pscal_fd_close(pscal_fd);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        struct stat st;
        if (vprocHostFstatRaw(fd, &st) == 0) {
            return vprocHostFchmodRaw(fd, mode);
        }
        return -1;
    }
    return vprocHostFchmodRaw(host_fd, mode);
}

int vprocFchownShim(int fd, uid_t uid, gid_t gid) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFchownRaw(fd, uid, gid);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        int err = vprocPtyApplyAttrsForFd(pscal_fd, NULL, &uid, &gid);
        pscal_fd_close(pscal_fd);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        struct stat st;
        if (vprocHostFstatRaw(fd, &st) == 0) {
            return vprocHostFchownRaw(fd, uid, gid);
        }
        return -1;
    }
    return vprocHostFchownRaw(host_fd, uid, gid);
}

int vprocUtimesShim(const char *path, const struct timeval times[2]) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostUtimesRaw(path, times);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostUtimesRaw(target ? target : path, times);
}

/* lchown, utimensat, linkat, mkstemp and mkdtemp take the app's paths, as chown and utimes do.
 * They had no shim, so an applet given /home/f, or ~/f now that $HOME is /home, reached the host's
 * /home: `touch ~/f` on a file that exists, `cp -p`, `tar x`, `chown -h`, `ln -L`, `sed -i ~/f`. */
int vprocLchownShim(const char *path, uid_t uid, gid_t gid) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return lchown(path, uid, gid);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocChownRecord(target ? target : path, uid, gid, false);
}

int vprocUtimensatShim(int dirfd, const char *path, const struct timespec times[2], int flags) {
    if (dirfd != AT_FDCWD || !path || !vprocForThread()) {
        return utimensat(dirfd, path, times, flags);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return utimensat(dirfd, target ? target : path, times, flags);
}

int vprocLinkatShim(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    if (!vprocForThread()) {
        return linkat(olddirfd, oldpath, newdirfd, newpath, flags);
    }
    char old_expanded[PATH_MAX];
    char new_expanded[PATH_MAX];
    const char *old_target = oldpath;
    const char *new_target = newpath;
    if (olddirfd == AT_FDCWD && oldpath) {
        const char *t = vprocPathExpandForShim(oldpath, old_expanded, sizeof(old_expanded));
        if (t) old_target = t;
    }
    if (newdirfd == AT_FDCWD && newpath) {
        const char *t = vprocPathExpandForShim(newpath, new_expanded, sizeof(new_expanded));
        if (t) new_target = t;
    }
    return linkat(olddirfd, old_target, newdirfd, new_target, flags);
}

/* The template is filled in where it is: only its random part comes back, so the caller keeps the
 * name it gave, which the other shims take. The descriptor is the host's, as libc's is. */
static char *vprocMktempHostTemplate(char *templ, char *host, size_t host_size, size_t *out_xs) {
    size_t len = strlen(templ);
    size_t xs = 0;
    while (xs < len && templ[len - 1 - xs] == 'X') {
        xs++;
    }
    *out_xs = xs;
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(templ, expanded, sizeof(expanded));
    if (!target || target == templ || strcmp(target, templ) == 0) {
        return NULL;
    }
    size_t tlen = strlen(target);
    if (tlen + 1 > host_size || tlen < xs || strcmp(target + tlen - xs, templ + len - xs) != 0) {
        return NULL;
    }
    memcpy(host, target, tlen + 1);
    return host;
}

int vprocMkstempShim(char *templ) {
    if (!templ || !vprocForThread()) {
        return mkstemp(templ);
    }
    char host[PATH_MAX];
    size_t xs = 0;
    if (!vprocMktempHostTemplate(templ, host, sizeof(host), &xs)) {
        return mkstemp(templ);
    }
    int fd = mkstemp(host);
    if (fd >= 0) {
        size_t len = strlen(templ);
        memcpy(templ + len - xs, host + strlen(host) - xs, xs);
    }
    return fd;
}

char *vprocMkdtempShim(char *templ) {
    if (!templ || !vprocForThread()) {
        return mkdtemp(templ);
    }
    char host[PATH_MAX];
    size_t xs = 0;
    if (!vprocMktempHostTemplate(templ, host, sizeof(host), &xs)) {
        return mkdtemp(templ);
    }
    if (!mkdtemp(host)) {
        return NULL;
    }
    size_t len = strlen(templ);
    memcpy(templ + len - xs, host + strlen(host) - xs, xs);
    return templ;
}

int vprocFutimesShim(int fd, const struct timeval times[2]) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFutimesRaw(fd, times);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        errno = EBADF;
        return -1;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        struct stat st;
        if (vprocHostFstatRaw(fd, &st) == 0) {
            return vprocHostFutimesRaw(fd, times);
        }
        return -1;
    }
    return vprocHostFutimesRaw(host_fd, times);
}

int vprocMkdirShim(const char *path, mode_t mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostMkdirRaw(path, mode);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostMkdirMasked(target ? target : path, mode);
}

int vprocRmdirShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRmdirRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostRmdirRaw(target ? target : path);
}

int vprocUnlinkShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostUnlinkRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostUnlinkRaw(target ? target : path);
}

int vprocRemoveShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRemoveRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostRemoveRaw(target ? target : path);
}

int vprocRenameShim(const char *oldpath, const char *newpath) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRenameRaw(oldpath, newpath);
    }
    char expanded_old[PATH_MAX];
    char expanded_new[PATH_MAX];
    const char *old_target = vprocPathExpandForShim(oldpath, expanded_old, sizeof(expanded_old));
    const char *new_target = vprocPathExpandForShim(newpath, expanded_new, sizeof(expanded_new));
    return vprocHostRenameRaw(old_target ? old_target : oldpath,
                              new_target ? new_target : newpath);
}

DIR *vprocOpendirShim(const char *name) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostOpendirRaw(name);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(name, expanded, sizeof(expanded));
    return vprocHostOpendirRaw(target ? target : name);
}

int vprocSymlinkShim(const char *target, const char *linkpath) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostSymlinkRaw(target, linkpath);
    }
    char expanded_link[PATH_MAX];
    const char *link_target = vprocPathExpandForShim(linkpath, expanded_link, sizeof(expanded_link));
    return vprocHostSymlinkRaw(target, link_target ? link_target : linkpath);
}

ssize_t vprocReadlinkShim(const char *path, char *buf, size_t size) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostReadlinkRaw(path, buf, size);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    ssize_t res = vprocHostReadlinkRaw(target ? target : path, buf, size);
    if (res >= 0 && pathTruncateEnabled() && size > 0 && (size_t)res < size) {
        buf[res] = '\0';
        char stripped[PATH_MAX];
        if (pathTruncateStrip(buf, stripped, sizeof(stripped))) {
            size_t len = strlen(stripped);
            if (len < size) {
                memcpy(buf, stripped, len + 1);
                res = (ssize_t)len;
            }
        }
    }
    return res;
}

char *vprocRealpathShim(const char *path, char *resolved_path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRealpathRaw(path, resolved_path);
    }
    int named_pid = 0;
    int named_fd = vprocPathNamedFdOf(path, &named_pid);
    if (named_fd >= 0) {
        /* Where the link leads. On Linux a pipe's ends up as a name under /proc. */
        char target[PATH_MAX];
        char final[PATH_MAX];
        if (!vprocFdLinkTarget(named_pid, named_fd, target, sizeof(target))) {
            errno = ENOENT;
            return NULL;
        }
        if (target[0] != '/') {
            snprintf(final, sizeof(final), "/proc/%d/fd/%s", named_pid, target);
        } else if (!pathTruncateEnabled() || !pathTruncateStrip(target, final, sizeof(final))) {
            snprintf(final, sizeof(final), "%s", target);
        }
        if (!resolved_path) {
            return strdup(final);
        }
        memcpy(resolved_path, final, strlen(final) + 1);
        return resolved_path;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    char *res = vprocHostRealpathRaw(target ? target : path, resolved_path);
    if (res && pathTruncateEnabled()) {
        char stripped[PATH_MAX];
        if (pathTruncateStrip(res, stripped, sizeof(stripped))) {
            size_t len = strlen(stripped);
            memcpy(res, stripped, len + 1);
        }
    }
    return res;
}

/* Darwin keeps its speeds in numbers of bits per second, and its cflags in other bits. */
#if defined(__APPLE__)
static const struct {
    dword_t code;
    speed_t speed;
} kVprocSpeeds[] = {
    {0x0, 0},        {0x1, 50},        {0x2, 75},        {0x3, 110},      {0x4, 134},
    {0x5, 150},      {0x6, 200},       {0x7, 300},       {0x8, 600},      {0x9, 1200},
    {0xa, 1800},     {0xb, 2400},      {0xc, 4800},      {0xd, 9600},     {0xe, 19200},
    {0xf, 38400},    {0x1001, 57600},  {0x1002, 115200}, {0x1003, 230400},
};

static void vprocCflagsToHost(dword_t cflags, struct termios *dst) {
    dst->c_cflag = 0;
    switch (cflags & CSIZE_) {
        case CS5_: dst->c_cflag |= CS5; break;
        case CS6_: dst->c_cflag |= CS6; break;
        case CS7_: dst->c_cflag |= CS7; break;
        default: dst->c_cflag |= CS8; break;
    }
    if (cflags & CSTOPB_) dst->c_cflag |= CSTOPB;
    if (cflags & CREAD_) dst->c_cflag |= CREAD;
    if (cflags & PARENB_) dst->c_cflag |= PARENB;
    if (cflags & PARODD_) dst->c_cflag |= PARODD;
    if (cflags & HUPCL_) dst->c_cflag |= HUPCL;
    if (cflags & CLOCAL_) dst->c_cflag |= CLOCAL;
    for (size_t i = 0; i < sizeof(kVprocSpeeds) / sizeof(kVprocSpeeds[0]); i++) {
        if (kVprocSpeeds[i].code == (cflags & CBAUD_)) {
            dst->c_ispeed = kVprocSpeeds[i].speed;
            dst->c_ospeed = kVprocSpeeds[i].speed;
        }
    }
}

static dword_t vprocCflagsFromHost(const struct termios *src) {
    dword_t cflags = 0;
    switch (src->c_cflag & CSIZE) {
        case CS5: cflags |= CS5_; break;
        case CS6: cflags |= CS6_; break;
        case CS7: cflags |= CS7_; break;
        default: cflags |= CS8_; break;
    }
    if (src->c_cflag & CSTOPB) cflags |= CSTOPB_;
    if (src->c_cflag & CREAD) cflags |= CREAD_;
    if (src->c_cflag & PARENB) cflags |= PARENB_;
    if (src->c_cflag & PARODD) cflags |= PARODD_;
    if (src->c_cflag & HUPCL) cflags |= HUPCL_;
    if (src->c_cflag & CLOCAL) cflags |= CLOCAL_;
    dword_t speed = B38400_;
    for (size_t i = 0; i < sizeof(kVprocSpeeds) / sizeof(kVprocSpeeds[0]); i++) {
        if (kVprocSpeeds[i].speed == src->c_ospeed) {
            speed = kVprocSpeeds[i].code;
        }
    }
    return cflags | speed;
}
#else
static void vprocCflagsToHost(dword_t cflags, struct termios *dst) {
    dst->c_cflag = (tcflag_t)cflags;
}

static dword_t vprocCflagsFromHost(const struct termios *src) {
    return (dword_t)src->c_cflag;
}
#endif

static void vprocTermiosFromHost(const struct termios *src, struct termios_ *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->cflags = vprocCflagsFromHost(src);
#define MAP_IN(flag) \
    do { \
        if (src->c_iflag & (flag)) dst->iflags |= flag##_; \
    } while (0)
#define MAP_OUT(flag) \
    do { \
        if (src->c_oflag & (flag)) dst->oflags |= flag##_; \
    } while (0)
#define MAP_L(flag) \
    do { \
        if (src->c_lflag & (flag)) dst->lflags |= flag##_; \
    } while (0)
#ifdef INLCR
    MAP_IN(INLCR);
#endif
#ifdef IGNCR
    MAP_IN(IGNCR);
#endif
#ifdef ICRNL
    MAP_IN(ICRNL);
#endif
#ifdef IXON
    MAP_IN(IXON);
#endif
#ifdef IGNBRK
    MAP_IN(IGNBRK);
#endif
#ifdef BRKINT
    MAP_IN(BRKINT);
#endif
#ifdef IGNPAR
    MAP_IN(IGNPAR);
#endif
#ifdef PARMRK
    MAP_IN(PARMRK);
#endif
#ifdef INPCK
    MAP_IN(INPCK);
#endif
#ifdef ISTRIP
    MAP_IN(ISTRIP);
#endif
#ifdef IUCLC
    MAP_IN(IUCLC);
#endif
#ifdef IXANY
    MAP_IN(IXANY);
#endif
#ifdef IXOFF
    MAP_IN(IXOFF);
#endif
#ifdef IMAXBEL
    MAP_IN(IMAXBEL);
#endif
#ifdef IUTF8
    MAP_IN(IUTF8);
#endif
#ifdef OPOST
    MAP_OUT(OPOST);
#endif
#ifdef ONLCR
    MAP_OUT(ONLCR);
#endif
#ifdef OCRNL
    MAP_OUT(OCRNL);
#endif
#ifdef ONOCR
    MAP_OUT(ONOCR);
#endif
#ifdef ONLRET
    MAP_OUT(ONLRET);
#endif
#ifdef OLCUC
    MAP_OUT(OLCUC);
#endif
#ifdef OFILL
    MAP_OUT(OFILL);
#endif
#ifdef OFDEL
    MAP_OUT(OFDEL);
#endif
#ifdef ISIG
    MAP_L(ISIG);
#endif
#ifdef ICANON
    MAP_L(ICANON);
#endif
#ifdef ECHO
    MAP_L(ECHO);
#endif
#ifdef ECHOE
    MAP_L(ECHOE);
#endif
#ifdef ECHOK
    MAP_L(ECHOK);
#endif
#ifdef ECHOKE
    MAP_L(ECHOKE);
#endif
#ifdef NOFLSH
    MAP_L(NOFLSH);
#endif
#ifdef ECHOCTL
    MAP_L(ECHOCTL);
#endif
#ifdef IEXTEN
    MAP_L(IEXTEN);
#endif
#ifdef ECHONL
    MAP_L(ECHONL);
#endif
#ifdef TOSTOP
    MAP_L(TOSTOP);
#endif
#ifdef ECHOPRT
    MAP_L(ECHOPRT);
#endif
#ifdef FLUSHO
    MAP_L(FLUSHO);
#endif
#ifdef PENDIN
    MAP_L(PENDIN);
#endif
#ifdef XCASE
    MAP_L(XCASE);
#endif
#if defined(TABDLY) && defined(TAB3)
    switch (src->c_oflag & TABDLY) {
        case TAB1: dst->oflags |= TAB1_; break;
        case TAB2: dst->oflags |= TAB2_; break;
        case TAB3: dst->oflags |= TAB3_; break;
        default: break;
    }
#endif
#undef MAP_IN
#undef MAP_OUT
#undef MAP_L

#define MAP_CC(idx, name) \
    do { \
        if (name < (int)(sizeof(src->c_cc) / sizeof(src->c_cc[0]))) { \
            dst->cc[idx] = (byte_t)src->c_cc[name]; \
        } \
    } while (0)
#ifdef VINTR
    MAP_CC(VINTR_, VINTR);
#endif
#ifdef VQUIT
    MAP_CC(VQUIT_, VQUIT);
#endif
#ifdef VERASE
    MAP_CC(VERASE_, VERASE);
#endif
#ifdef VKILL
    MAP_CC(VKILL_, VKILL);
#endif
#ifdef VEOF
    MAP_CC(VEOF_, VEOF);
#endif
#ifdef VTIME
    MAP_CC(VTIME_, VTIME);
#endif
#ifdef VMIN
    MAP_CC(VMIN_, VMIN);
#endif
#ifdef VSTART
    MAP_CC(VSTART_, VSTART);
#endif
#ifdef VSTOP
    MAP_CC(VSTOP_, VSTOP);
#endif
#ifdef VSUSP
    MAP_CC(VSUSP_, VSUSP);
#endif
#ifdef VEOL
    MAP_CC(VEOL_, VEOL);
#endif
#ifdef VREPRINT
    MAP_CC(VREPRINT_, VREPRINT);
#endif
#ifdef VDISCARD
    MAP_CC(VDISCARD_, VDISCARD);
#endif
#ifdef VWERASE
    MAP_CC(VWERASE_, VWERASE);
#endif
#ifdef VLNEXT
    MAP_CC(VLNEXT_, VLNEXT);
#endif
#ifdef VEOL2
    MAP_CC(VEOL2_, VEOL2);
#endif
#undef MAP_CC
}

static void vprocTermiosToHost(const struct termios_ *src, struct termios *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    vprocCflagsToHost(src->cflags, dst);
#define MAP_IN(flag) \
    do { \
        if (src->iflags & flag##_) dst->c_iflag |= (flag); \
    } while (0)
#define MAP_OUT(flag) \
    do { \
        if (src->oflags & flag##_) dst->c_oflag |= (flag); \
    } while (0)
#define MAP_L(flag) \
    do { \
        if (src->lflags & flag##_) dst->c_lflag |= (flag); \
    } while (0)
#ifdef INLCR
    MAP_IN(INLCR);
#endif
#ifdef IGNCR
    MAP_IN(IGNCR);
#endif
#ifdef ICRNL
    MAP_IN(ICRNL);
#endif
#ifdef IXON
    MAP_IN(IXON);
#endif
#ifdef IGNBRK
    MAP_IN(IGNBRK);
#endif
#ifdef BRKINT
    MAP_IN(BRKINT);
#endif
#ifdef IGNPAR
    MAP_IN(IGNPAR);
#endif
#ifdef PARMRK
    MAP_IN(PARMRK);
#endif
#ifdef INPCK
    MAP_IN(INPCK);
#endif
#ifdef ISTRIP
    MAP_IN(ISTRIP);
#endif
#ifdef IUCLC
    MAP_IN(IUCLC);
#endif
#ifdef IXANY
    MAP_IN(IXANY);
#endif
#ifdef IXOFF
    MAP_IN(IXOFF);
#endif
#ifdef IMAXBEL
    MAP_IN(IMAXBEL);
#endif
#ifdef IUTF8
    MAP_IN(IUTF8);
#endif
#ifdef OPOST
    MAP_OUT(OPOST);
#endif
#ifdef ONLCR
    MAP_OUT(ONLCR);
#endif
#ifdef OCRNL
    MAP_OUT(OCRNL);
#endif
#ifdef ONOCR
    MAP_OUT(ONOCR);
#endif
#ifdef ONLRET
    MAP_OUT(ONLRET);
#endif
#ifdef OLCUC
    MAP_OUT(OLCUC);
#endif
#ifdef OFILL
    MAP_OUT(OFILL);
#endif
#ifdef OFDEL
    MAP_OUT(OFDEL);
#endif
#ifdef ISIG
    MAP_L(ISIG);
#endif
#ifdef ICANON
    MAP_L(ICANON);
#endif
#ifdef ECHO
    MAP_L(ECHO);
#endif
#ifdef ECHOE
    MAP_L(ECHOE);
#endif
#ifdef ECHOK
    MAP_L(ECHOK);
#endif
#ifdef ECHOKE
    MAP_L(ECHOKE);
#endif
#ifdef NOFLSH
    MAP_L(NOFLSH);
#endif
#ifdef ECHOCTL
    MAP_L(ECHOCTL);
#endif
#ifdef IEXTEN
    MAP_L(IEXTEN);
#endif
#ifdef ECHONL
    MAP_L(ECHONL);
#endif
#ifdef TOSTOP
    MAP_L(TOSTOP);
#endif
#ifdef ECHOPRT
    MAP_L(ECHOPRT);
#endif
#ifdef FLUSHO
    MAP_L(FLUSHO);
#endif
#ifdef PENDIN
    MAP_L(PENDIN);
#endif
#ifdef XCASE
    MAP_L(XCASE);
#endif
#if defined(TABDLY) && defined(TAB3)
    switch (src->oflags & TABDLY_) {
        case TAB1_: dst->c_oflag |= TAB1; break;
        case TAB2_: dst->c_oflag |= TAB2; break;
        case TAB3_: dst->c_oflag |= TAB3; break;
        default: break;
    }
#endif
#undef MAP_IN
#undef MAP_OUT
#undef MAP_L

#define MAP_CC(idx, name) \
    do { \
        if (name < (int)(sizeof(dst->c_cc) / sizeof(dst->c_cc[0]))) { \
            dst->c_cc[name] = (cc_t)src->cc[idx]; \
        } \
    } while (0)
#ifdef VINTR
    MAP_CC(VINTR_, VINTR);
#endif
#ifdef VQUIT
    MAP_CC(VQUIT_, VQUIT);
#endif
#ifdef VERASE
    MAP_CC(VERASE_, VERASE);
#endif
#ifdef VKILL
    MAP_CC(VKILL_, VKILL);
#endif
#ifdef VEOF
    MAP_CC(VEOF_, VEOF);
#endif
#ifdef VTIME
    MAP_CC(VTIME_, VTIME);
#endif
#ifdef VMIN
    MAP_CC(VMIN_, VMIN);
#endif
#ifdef VSTART
    MAP_CC(VSTART_, VSTART);
#endif
#ifdef VSTOP
    MAP_CC(VSTOP_, VSTOP);
#endif
#ifdef VSUSP
    MAP_CC(VSUSP_, VSUSP);
#endif
#ifdef VEOL
    MAP_CC(VEOL_, VEOL);
#endif
#ifdef VREPRINT
    MAP_CC(VREPRINT_, VREPRINT);
#endif
#ifdef VDISCARD
    MAP_CC(VDISCARD_, VDISCARD);
#endif
#ifdef VWERASE
    MAP_CC(VWERASE_, VWERASE);
#endif
#ifdef VLNEXT
    MAP_CC(VLNEXT_, VLNEXT);
#endif
#ifdef VEOL2
    MAP_CC(VEOL2_, VEOL2);
#endif
#undef MAP_CC
}

static void vprocWinsizeFromHost(const struct winsize *src, struct winsize_ *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->row = (word_t)src->ws_row;
    dst->col = (word_t)src->ws_col;
    dst->xpixel = (word_t)src->ws_xpixel;
    dst->ypixel = (word_t)src->ws_ypixel;
}

static void vprocWinsizeToHost(const struct winsize_ *src, struct winsize *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->ws_row = (unsigned short)src->row;
    dst->ws_col = (unsigned short)src->col;
    dst->ws_xpixel = (unsigned short)src->xpixel;
    dst->ws_ypixel = (unsigned short)src->ypixel;
}

/* A request in Linux's numbers, on a host that has numbers of its own, wants
 * Linux's struct, which is the terminal's own (smallclue's stty tries it first,
 * for iSH-AOK). The host's struct is twice the size and overruns the caller's. */
static bool vprocIoctlWantsKernelTermios(unsigned long request) {
#ifdef TIOCGETA
    return request == TCGETS_ || request == TCSETS_ ||
           request == TCSETSW_ || request == TCSETSF_;
#else
    (void)request;
    return false;
#endif
}

static int vprocIoctlTranslate(unsigned long cmd) {
    switch (cmd) {
#ifdef TIOCGWINSZ
        case TIOCGWINSZ:
            return TIOCGWINSZ_;
#endif
#ifdef TIOCSWINSZ
        case TIOCSWINSZ:
            return TIOCSWINSZ_;
#endif
#ifdef TIOCGETA
        case TIOCGETA:
            return TCGETS_;
#endif
#ifdef TIOCSETA
        case TIOCSETA:
            return TCSETS_;
#endif
#ifdef TIOCSETAW
        case TIOCSETAW:
            return TCSETSW_;
#endif
#ifdef TIOCSETAF
        case TIOCSETAF:
            return TCSETSF_;
#endif
#ifdef TCGETS
        case TCGETS:
            return TCGETS_;
#endif
#ifdef TCSETS
        case TCSETS:
            return TCSETS_;
#endif
#ifdef TCSETSW
        case TCSETSW:
            return TCSETSW_;
#endif
#ifdef TCSETSF
        case TCSETSF:
            return TCSETSF_;
#endif
#ifdef TIOCGPGRP
        case TIOCGPGRP:
            return TIOCGPGRP_;
#endif
#ifdef TIOCSPGRP
        case TIOCSPGRP:
            return TIOCSPGRP_;
#endif
#ifdef TIOCSCTTY
        case TIOCSCTTY:
            return TIOCSCTTY_;
#endif
#ifdef TIOCGPTN
        case TIOCGPTN:
            return TIOCGPTN_;
#endif
#ifdef TIOCGPTPEER
        case TIOCGPTPEER:
            return TIOCGPTPEER_;
#endif
#ifdef TIOCSPTLCK
        case TIOCSPTLCK:
            return TIOCSPTLCK_;
#endif
#ifdef TIOCPKT
        case TIOCPKT:
            return TIOCPKT_;
#endif
#ifdef TIOCGPKT
        case TIOCGPKT:
            return TIOCGPKT_;
#endif
#ifdef TCFLSH
        case TCFLSH:
            return TCFLSH_;
#endif
#ifdef FIONREAD
        case FIONREAD:
            return FIONREAD_;
#endif
#ifdef TIOCEXCL
        case TIOCEXCL:
            return TIOCEXCL_;
#endif
#ifdef TIOCNXCL
        case TIOCNXCL:
            return TIOCNXCL_;
#endif
#ifdef TIOCOUTQ
        case TIOCOUTQ:
            return TIOCOUTQ_;
#endif
#ifdef TIOCSTI
        case TIOCSTI:
            return TIOCSTI_;
#endif
#ifdef TIOCNOTTY
        case TIOCNOTTY:
            return TIOCNOTTY_;
#endif
#ifdef TIOCGSID
        case TIOCGSID:
            return TIOCGSID_;
#endif
#ifdef TIOCDRAIN
        case TIOCDRAIN:
            return TCSBRK_;
#endif
#ifdef TIOCSBRK
        case TIOCSBRK:
            return TCSBRK_;
#endif
#ifdef TIOCCBRK
        case TIOCCBRK:
            return TCSBRK_;
#endif
        default:
            return (int)cmd;
    }
}

static struct pscal_fd *vprocSessionPscalFdForStd(int fd) {
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return NULL;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session || vprocSessionStdioIsDefault(session)) {
        return NULL;
    }
    struct pscal_fd *candidate = NULL;
    if (fd == STDIN_FILENO) {
        candidate = session->stdin_pscal_fd ? session->stdin_pscal_fd : session->pty_slave;
    } else if (fd == STDOUT_FILENO) {
        candidate = session->stdout_pscal_fd ? session->stdout_pscal_fd : session->pty_slave;
    } else if (fd == STDERR_FILENO) {
        candidate = session->stderr_pscal_fd ? session->stderr_pscal_fd : session->pty_slave;
    }
    if (!candidate) {
        return NULL;
    }
    return pscal_fd_retain(candidate);
}

bool vprocSessionStdioFetchTermios(int fd, struct termios *termios) {
    if (!termios) {
        return false;
    }
    struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(fd);
    if (!pscal_fd) {
        return false;
    }
    int res = _ENOTTY;
    if (pscal_fd->ops && pscal_fd->ops->ioctl) {
        struct termios_ termios_val;
        res = pscal_fd->ops->ioctl(pscal_fd, TCGETS_, &termios_val);
        if (res == 0) {
            vprocTermiosToHost(&termios_val, termios);
        }
    }
    pscal_fd_close(pscal_fd);
    return res == 0;
}

bool vprocSessionStdioApplyTermios(int fd, int action, const struct termios *termios) {
    if (!termios) {
        return false;
    }
    int cmd = 0;
    switch (action) {
        case TCSANOW:
            cmd = TCSETS_;
            break;
        case TCSADRAIN:
            cmd = TCSETSW_;
            break;
        case TCSAFLUSH:
            cmd = TCSETSF_;
            break;
        default:
            cmd = TCSETS_;
            break;
    }
    struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(fd);
    if (!pscal_fd) {
        return false;
    }
    int res = _ENOTTY;
    if (pscal_fd->ops && pscal_fd->ops->ioctl) {
        struct termios_ termios_val;
        vprocTermiosFromHost(termios, &termios_val);
        res = pscal_fd->ops->ioctl(pscal_fd, cmd, &termios_val);
    }
    pscal_fd_close(pscal_fd);
    return res == 0;
}

struct VProcTerminalHold {
    struct pscal_fd *tty;
    int host_fd;
};

VProcTerminalHold *vprocTerminalHold(int fd) {
    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = vp ? vprocGetPscalFd(vp, fd) : NULL;
    if (!pscal_fd) {
        pscal_fd = vprocSessionPscalFdForStd(fd);
    }
    int host_fd = -1;
    if (pscal_fd) {
        if (!pscal_fd->tty) {
            pscal_fd_close(pscal_fd);
            return NULL;
        }
    } else {
        /* In the order a termios call tries them: the descriptor's own file, then the
         * process's descriptor of that number. */
        int host = shimTranslate(fd, 1);
        if (host < 0 || !vprocHostIsatty(host)) {
            host = fd;
            if (!vprocHostIsatty(host)) {
                return NULL;
            }
        }
        /* Unowned: the vproc that named it may be gone by the time this is used. */
        host_fd = vprocHostDupRaw(host);
        if (host_fd < 0) {
            return NULL;
        }
    }
    VProcTerminalHold *hold = (VProcTerminalHold *)malloc(sizeof(*hold));
    if (!hold) {
        if (pscal_fd) {
            pscal_fd_close(pscal_fd);
        } else {
            vprocHostCloseRaw(host_fd);
        }
        return NULL;
    }
    hold->tty = pscal_fd;
    hold->host_fd = host_fd;
    return hold;
}

bool vprocTerminalHoldFetchTermios(VProcTerminalHold *hold, struct termios *termios) {
    if (!hold || !termios) {
        return false;
    }
    if (!hold->tty) {
#if defined(TIOCGETA)
        return vprocHostIoctlRaw(hold->host_fd, TIOCGETA, termios) == 0;
#else
        return vprocHostIoctlRaw(hold->host_fd, TCGETS, termios) == 0;
#endif
    }
    struct termios_ termios_val;
    if (!hold->tty->ops || !hold->tty->ops->ioctl || hold->tty->ops->ioctl(hold->tty, TCGETS_, &termios_val) != 0) {
        return false;
    }
    vprocTermiosToHost(&termios_val, termios);
    return true;
}

bool vprocTerminalHoldApplyTermios(VProcTerminalHold *hold, const struct termios *termios) {
    if (!hold || !termios) {
        return false;
    }
    if (!hold->tty) {
#if defined(TIOCSETA)
        return vprocHostIoctlRaw(hold->host_fd, TIOCSETA, (void *)termios) == 0;
#else
        return vprocHostIoctlRaw(hold->host_fd, TCSETS, (void *)termios) == 0;
#endif
    }
    struct termios_ termios_val;
    vprocTermiosFromHost(termios, &termios_val);
    return hold->tty->ops && hold->tty->ops->ioctl && hold->tty->ops->ioctl(hold->tty, TCSETS_, &termios_val) == 0;
}

void vprocTerminalHoldRelease(VProcTerminalHold *hold) {
    if (!hold) {
        return;
    }
    if (hold->tty) {
        pscal_fd_close(hold->tty);
    } else {
        vprocHostCloseRaw(hold->host_fd);
    }
    free(hold);
}

int vprocIoctlShim(int fd, unsigned long request, ...) {
    vprocDeliverPendingSignalsForCurrent();
    uintptr_t arg_val = 0;
    va_list ap;
    va_start(ap, request);
    arg_val = va_arg(ap, uintptr_t);
    va_end(ap);

    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = NULL;
    if (vp) {
        pscal_fd = vprocGetPscalFd(vp, fd);
    }
    if (!pscal_fd) {
        pscal_fd = vprocSessionPscalFdForStd(fd);
    }
    if (pscal_fd) {
        int cmd = vprocIoctlTranslate(request);
#ifdef TIOCFLUSH
        /* tcflush on Darwin: a pointer to which of FREAD (1) and FWRITE (2), none meaning both. */
        if (request == TIOCFLUSH) {
            const int which = arg_val ? *(const int *)arg_val : 0;
            cmd = TCFLSH_;
            arg_val = (uintptr_t)(which == 1 ? TCIFLUSH_ : (which == 2 ? TCOFLUSH_ : TCIOFLUSH_));
        }
#endif
#if defined(TIOCSTOP) && defined(TIOCSTART)
        /* tcflow on Darwin: TCOOFF and TCOON are these. TCIOFF and TCION write the key. */
        if (request == TIOCSTOP || request == TIOCSTART) {
            cmd = TCXONC_;
            arg_val = (uintptr_t)(request == TIOCSTOP ? TCOOFF_ : TCOON_);
        }
#endif
#ifdef TCXONC
        if (request == TCXONC) {
            cmd = TCXONC_;
        }
#endif
        if (cmd == TIOCGPTPEER_) {
            int res = _ENOTTY;
            if (vp && pscal_fd->tty && pscalPtyIsMaster(pscal_fd)) {
                int flags = (int)arg_val;
                if (flags == 0) {
                    flags = O_RDWR;
                }
                struct pscal_fd *peer = NULL;
                int err = pscalPtyOpenSlave(pscal_fd->tty->num, flags, &peer);
                if (err < 0) {
                    res = err;
                } else {
                    int slot = vprocInsertPscalFd(vp, peer);
                    /* fd table retains; drop our reference. */
                    pscal_fd_close(peer);
                    res = (slot < 0) ? _EMFILE : slot;
                }
            }
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                return vprocSetCompatErrno(res);
            }
            return res;
        }
        int res = _ENOTTY;
        if (pscal_fd->ops && pscal_fd->ops->ioctl) {
            switch (cmd) {
                case TCGETS_: {
                    if (!arg_val) {
                        res = _EFAULT;
                        break;
                    }
                    if (vprocIoctlWantsKernelTermios(request)) {
                        res = pscal_fd->ops->ioctl(pscal_fd, cmd, (void *)arg_val);
                        break;
                    }
                    struct termios_ termios;
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &termios);
                    if (res == 0) {
                        vprocTermiosToHost(&termios, (struct termios *)arg_val);
                    }
                    break;
                }
                case TCSETS_:
                case TCSETSW_:
                case TCSETSF_: {
                    if (!arg_val) {
                        res = _EFAULT;
                        break;
                    }
                    if (vprocIoctlWantsKernelTermios(request)) {
                        res = pscal_fd->ops->ioctl(pscal_fd, cmd, (void *)arg_val);
                        break;
                    }
                    struct termios_ termios;
                    vprocTermiosFromHost((const struct termios *)arg_val, &termios);
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &termios);
                    break;
                }
                case TIOCGWINSZ_: {
                    if (!arg_val) {
                        res = _EFAULT;
                        break;
                    }
                    struct winsize_ ws;
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &ws);
                    if (res == 0) {
                        vprocWinsizeToHost(&ws, (struct winsize *)arg_val);
                    }
                    break;
                }
                case TIOCSWINSZ_: {
                    if (!arg_val) {
                        res = _EFAULT;
                        break;
                    }
                    struct winsize_ ws;
                    vprocWinsizeFromHost((const struct winsize *)arg_val, &ws);
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &ws);
                    break;
                }
                default:
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, (void *)arg_val);
                    break;
            }
        }
        pscal_fd_close(pscal_fd);
        if (res < 0) {
            return vprocSetCompatErrno(res);
        }
        return res;
    }

    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostIoctlRaw(host, request, (void *)arg_val);
}

off_t vprocLseekShim(int fd, off_t offset, int whence) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostLseek(host, offset, whence);
}

static short vprocPollMapReady(int pscal_events, short requested) {
    short ready = 0;
    if (pscal_events & POLL_READ) ready |= POLLIN;
    if (pscal_events & POLL_WRITE) ready |= POLLOUT;
    if (pscal_events & POLL_PRI) ready |= POLLPRI;
    if (pscal_events & POLL_ERR) ready |= POLLERR;
    if (pscal_events & POLL_HUP) ready |= POLLHUP;
    if (pscal_events & POLL_NVAL) ready |= POLLNVAL;
    short mask = (short)(requested | POLLERR | POLLHUP | POLLNVAL | POLLPRI);
    return (short)(ready & mask);
}

#define VPROC_POLL_STACK_FDS 128
#define VPROC_SELECT_STACK_FDS 128

typedef struct {
    struct pscal_fd **pscal_fds;
    int *host_index;
    struct pollfd *host_fds;
    size_t capacity;
} VProcPollScratch;

typedef struct {
    struct pollfd *pfds;
    int *fd_map;
    size_t capacity;
} VProcSelectScratch;

typedef struct {
    VProcPollScratch poll;
    VProcSelectScratch select;
} VProcScratchTls;

/* On the heap behind a key: on Darwin a thread's __thread values already read
 * as zero in a key destructor, so one there could never free them. */
static __thread VProcScratchTls *gVProcScratchTls = NULL;
static pthread_key_t gVProcScratchTlsKey;
static pthread_once_t gVProcScratchTlsKeyOnce = PTHREAD_ONCE_INIT;
static bool gVProcScratchTlsKeyReady = false;

/* Only after vprocScratchTlsForThread has given this thread its set. */
#define gVProcPollScratch (gVProcScratchTls->poll)
#define gVProcSelectScratch (gVProcScratchTls->select)

static void vprocScratchTlsFree(VProcScratchTls *scratch) {
    free(scratch->poll.pscal_fds);
    free(scratch->poll.host_index);
    free(scratch->poll.host_fds);
    free(scratch->select.pfds);
    free(scratch->select.fd_map);
    free(scratch);
}

static void vprocScratchTlsCleanupCurrentThread(void) {
    VProcScratchTls *scratch = gVProcScratchTls;
    if (!scratch) {
        return;
    }
    gVProcScratchTls = NULL;
    if (gVProcScratchTlsKeyReady) {
        (void)pthread_setspecific(gVProcScratchTlsKey, NULL);
    }
    vprocScratchTlsFree(scratch);
}

static void vprocScratchTlsDestructor(void *value) {
    vprocScratchTlsFree((VProcScratchTls *)value);
    /* A destructor after this one that polls gets a new set, and the key runs again. */
    gVProcScratchTls = NULL;
}

static void vprocScratchTlsKeyInit(void) {
    if (pthread_key_create(&gVProcScratchTlsKey, vprocScratchTlsDestructor) == 0) {
        gVProcScratchTlsKeyReady = true;
    }
}

static VProcScratchTls *vprocScratchTlsForThread(void) {
    if (gVProcScratchTls) {
        return gVProcScratchTls;
    }
    pthread_once(&gVProcScratchTlsKeyOnce, vprocScratchTlsKeyInit);
    if (!gVProcScratchTlsKeyReady) {
        return NULL;
    }
    VProcScratchTls *scratch = (VProcScratchTls *)calloc(1, sizeof(*scratch));
    if (!scratch) {
        return NULL;
    }
    if (pthread_setspecific(gVProcScratchTlsKey, scratch) != 0) {
        free(scratch);
        return NULL;
    }
    gVProcScratchTls = scratch;
    return scratch;
}

static bool vprocPollScratchEnsure(size_t needed) {
    if (!vprocScratchTlsForThread()) {
        errno = ENOMEM;
        return false;
    }
    if (needed <= gVProcPollScratch.capacity) {
        return true;
    }
    size_t new_cap = gVProcPollScratch.capacity ? gVProcPollScratch.capacity : (VPROC_POLL_STACK_FDS * 2u);
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            errno = ENOMEM;
            return false;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / sizeof(*gVProcPollScratch.pscal_fds) ||
        (new_cap + 1u) > SIZE_MAX / sizeof(*gVProcPollScratch.host_index) ||
        (new_cap + 1u) > SIZE_MAX / sizeof(*gVProcPollScratch.host_fds)) {
        errno = ENOMEM;
        return false;
    }

    struct pscal_fd **pscal_fds = calloc(new_cap, sizeof(*pscal_fds));
    int *host_index = calloc(new_cap + 1u, sizeof(*host_index));
    struct pollfd *host_fds = calloc(new_cap + 1u, sizeof(*host_fds));
    if (!pscal_fds || !host_index || !host_fds) {
        free(pscal_fds);
        free(host_index);
        free(host_fds);
        errno = ENOMEM;
        return false;
    }

    free(gVProcPollScratch.pscal_fds);
    free(gVProcPollScratch.host_index);
    free(gVProcPollScratch.host_fds);
    gVProcPollScratch.pscal_fds = pscal_fds;
    gVProcPollScratch.host_index = host_index;
    gVProcPollScratch.host_fds = host_fds;
    gVProcPollScratch.capacity = new_cap;
    return true;
}

static bool vprocSelectScratchEnsure(size_t needed) {
    if (!vprocScratchTlsForThread()) {
        errno = ENOMEM;
        return false;
    }
    if (needed <= gVProcSelectScratch.capacity) {
        return true;
    }
    size_t new_cap = gVProcSelectScratch.capacity ? gVProcSelectScratch.capacity : (VPROC_SELECT_STACK_FDS * 2u);
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            errno = ENOMEM;
            return false;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / sizeof(*gVProcSelectScratch.pfds) ||
        new_cap > SIZE_MAX / sizeof(*gVProcSelectScratch.fd_map)) {
        errno = ENOMEM;
        return false;
    }

    struct pollfd *pfds = calloc(new_cap, sizeof(*pfds));
    int *fd_map = calloc(new_cap, sizeof(*fd_map));
    if (!pfds || !fd_map) {
        free(pfds);
        free(fd_map);
        errno = ENOMEM;
        return false;
    }

    free(gVProcSelectScratch.pfds);
    free(gVProcSelectScratch.fd_map);
    gVProcSelectScratch.pfds = pfds;
    gVProcSelectScratch.fd_map = fd_map;
    gVProcSelectScratch.capacity = new_cap;
    return true;
}

static short vprocSessionInputPollEvents(VProcSessionStdio *session,
                                         int logical_fd,
                                         struct pscal_fd *pscal_fd,
                                         short requested_events) {
    (void)logical_fd;
    if (!vprocSessionFdIsTerminal(session, pscal_fd)) {
        return 0;
    }

    int read_mask = POLLIN;
#ifdef POLLPRI
    read_mask |= POLLPRI;
#endif
#ifdef POLLRDNORM
    read_mask |= POLLRDNORM;
#endif
#ifdef POLLRDBAND
    read_mask |= POLLRDBAND;
#endif
    if ((requested_events & read_mask) == 0) {
        return 0;
    }

    VProcSessionInput *input = vprocSessionInputEnsure(session,
                                                       vprocGetShellSelfPid(),
                                                       vprocGetKernelPid());
    if (!input) {
        return 0;
    }

    short ready = 0;
    pthread_mutex_lock(&input->mu);
    bool has_data = input->len > 0 || input->eof_mark_count > 0;
    bool is_eof = input->eof;
    bool has_interrupt = input->interrupt_pending;
    pthread_mutex_unlock(&input->mu);

    if (has_data || has_interrupt || is_eof) {
        ready |= (short)(requested_events & read_mask);
    }
    if (is_eof) {
        ready |= POLLHUP;
    }
    return ready;
}

/* Not through vprocReadShim: it acts on a pending signal, and SIGINT's default action
 * ends the thread there, with the VM's frames and any lock it holds. Background job
 * control does not apply either: a file or pipe is not the terminal. */
ssize_t vprocReadRedirectedStdin(void *buf, size_t count) {
    VProc *vp = vprocForThread();
    sigset_t pending;
    sigemptyset(&pending);
    if (vp && vprocSigpending(vprocPid(vp), &pending) == 0 &&
        (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTSTP))) {
        errno = EINTR;
        return -1;
    }
    vprocDeliverPendingSignalsForCurrent();
    struct pscal_fd *pscal_fd = NULL;
    int host = vprocResolveFdForShim(vp, STDIN_FILENO, 0, &pscal_fd);
    if (pscal_fd) {
        ssize_t res;
        int events = pscal_fd->ops && pscal_fd->ops->poll ? pscal_fd->ops->poll(pscal_fd) : POLL_ERR;
        if (vprocPollMapReady(events, POLLIN)) {
            res = pscal_fd->ops->read(pscal_fd, buf, count);
            if (res < 0) {
                res = vprocSetCompatErrno((int)res);
            }
        } else {
            errno = EAGAIN;
            res = -1;
        }
        pscal_fd_close(pscal_fd);
        return res;
    }
    if (host < 0) {
        return -1;
    }
    struct pollfd pfd = { .fd = host, .events = POLLIN, .revents = 0 };
    int ready = vprocHostPollRaw(&pfd, 1, 0);
    if (ready == 0 || (ready < 0 && errno == EINTR)) {
        errno = EAGAIN;
        return -1;
    }
    return vprocHostRead(host, buf, count);
}

#define VPROC_POLL_SIGNAL_CHECK_MS 100

int vprocPollShim(struct pollfd *fds, nfds_t nfds, int timeout) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostPollRaw(fds, nfds, timeout);
    }
    if (!fds || nfds == 0) {
        bool timer_limited = false;
        int wait_timeout = vprocClampTimeoutByCurrentRealTimer(vp, timeout, &timer_limited);
        int rc = vprocHostPollRaw(fds, nfds, wait_timeout);
        if (timer_limited && rc == 0) {
            vprocDeliverPendingSignalsForCurrent();
        }
        return rc;
    }
    if (nfds > (nfds_t)(INT_MAX - 1)) {
        errno = EINVAL;
        return -1;
    }

    struct pscal_fd *pscal_fds_stack[VPROC_POLL_STACK_FDS];
    int host_index_stack[VPROC_POLL_STACK_FDS + 1];
    struct pollfd host_fds_stack[VPROC_POLL_STACK_FDS + 1];
    bool use_stack = nfds <= VPROC_POLL_STACK_FDS;

    struct pscal_fd **pscal_fds = NULL;
    int *host_index = NULL;
    struct pollfd *host_fds = NULL;
    if (use_stack) {
        memset(pscal_fds_stack, 0, sizeof(pscal_fds_stack));
        memset(host_index_stack, 0, sizeof(host_index_stack));
        memset(host_fds_stack, 0, sizeof(host_fds_stack));
        pscal_fds = pscal_fds_stack;
        host_index = host_index_stack;
        host_fds = host_fds_stack;
    } else {
        if (!vprocPollScratchEnsure((size_t)nfds)) {
            return -1;
        }
        pscal_fds = gVProcPollScratch.pscal_fds;
        host_index = gVProcPollScratch.host_index;
        host_fds = gVProcPollScratch.host_fds;
        memset(pscal_fds, 0, ((size_t)nfds) * sizeof(*pscal_fds));
    }

    pthread_mutex_lock(&vp->mu);
    for (nfds_t i = 0; i < nfds; ++i) {
        host_index[i] = -1;
        if (fds[i].fd < 0) {
            continue;
        }
        if (vprocFdIsTaggedHost(fds[i].fd)) {
            host_index[i] = fds[i].fd & ~VPROC_HOST_FD_TAG;
            continue;
        }
        if ((size_t)fds[i].fd >= vp->capacity) {
            continue;
        }
        VProcFdEntry entry = vp->entries[fds[i].fd];
        if (entry.kind == VPROC_FD_PSCAL && entry.pscal_fd) {
            pscal_fds[i] = pscal_fd_retain(entry.pscal_fd);
            continue;
        }
        if (entry.kind == VPROC_FD_HOST && entry.host_fd >= 0) {
            host_index[i] = entry.host_fd;
        }
    }
    pthread_mutex_unlock(&vp->mu);

    int ready_count = 0;
    bool has_pscal = false;
    int host_count = 0;
    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    for (nfds_t i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;
        }
        struct pscal_fd *pscal_fd = pscal_fds[i];
        if (pscal_fd) {
            has_pscal = true;
            int pscal_events = pscal_fd->ops && pscal_fd->ops->poll
                ? pscal_fd->ops->poll(pscal_fd)
                : POLL_ERR;
            short ready = vprocPollMapReady(pscal_events, fds[i].events);
            ready |= vprocSessionInputPollEvents(session_stdio,
                                                 fds[i].fd,
                                                 pscal_fd,
                                                 fds[i].events);
            if (ready) {
                fds[i].revents = ready;
                ready_count++;
            }
            continue;
        }

        int host_fd = host_index[i];
        if (host_fd < 0 &&
            (vp == NULL || vprocAllowRealFdFallback(vp, fds[i].fd))) {
            struct stat st;
            if (vprocHostFstatRaw(fds[i].fd, &st) == 0) {
                host_fd = fds[i].fd;
            }
        }
        if (host_fd < 0) {
            fds[i].revents = POLLNVAL;
            ready_count++;
            continue;
        }
        host_fds[host_count] = fds[i];
        host_fds[host_count].fd = host_fd;
        host_index[host_count] = (int)i;
        host_count++;
    }

    if (ready_count == 0 && (host_count > 0 || has_pscal)) {
        const int host_count_static = host_count;
        int wake_fd = -1;
        bool wake_enabled = false;
        if (has_pscal) {
            wake_fd = pscalPollWakeFd();
            wake_enabled = (wake_fd >= 0);
        }

        uint64_t deadline_ns = 0;
        if (timeout > 0) {
            deadline_ns = vprocNowMonoNs() + ((uint64_t)timeout * 1000000ull);
        }
        int wait_timeout = timeout;

        for (;;) {
            int host_poll_count = host_count_static;
            bool wake_triggered = false;

            for (int j = 0; j < host_count_static; ++j) {
                host_fds[j].revents = 0;
            }
            if (wake_enabled) {
                host_fds[host_poll_count].fd = wake_fd;
                host_fds[host_poll_count].events = POLLIN;
                host_fds[host_poll_count].revents = 0;
                host_index[host_poll_count] = -1;
                host_poll_count++;
            }

            bool timer_limited = false;
            int effective_wait = vprocClampTimeoutByCurrentRealTimer(vp, wait_timeout, &timer_limited);
            /* A signal has to end this wait with EINTR. Only a pscal_fd brings the wake pipe, so
             * a wait on host descriptors alone looks for one every so often. */
            if (!wake_enabled && (effective_wait < 0 || effective_wait > VPROC_POLL_SIGNAL_CHECK_MS)) {
                effective_wait = VPROC_POLL_SIGNAL_CHECK_MS;
                timer_limited = false;
            }
            int host_ready = 0;
            if (host_poll_count > 0) {
                host_ready = vprocHostPollRaw(host_fds, (nfds_t)host_poll_count, effective_wait);
                if (host_ready < 0) {
                    if (errno == EINTR) {
                        if (effective_wait == 0) {
                            break;
                        }
                        goto vproc_poll_update_timeout;
                    }
                    for (nfds_t i = 0; i < nfds; ++i) {
                        if (pscal_fds[i]) {
                            pscal_fd_close(pscal_fds[i]);
                        }
                    }
                    return -1;
                }
            } else if (effective_wait > 0) {
                usleep((useconds_t)effective_wait * 1000u);
            } else if (effective_wait < 0) {
                usleep(1000u);
            }
            if (timer_limited && host_ready == 0) {
                vprocDeliverPendingSignalsForCurrent();
                if (wait_timeout < 0) {
                    ready_count = 0;
                    break;
                }
            }

            int host_ready_count = 0;
            for (int j = 0; j < host_count_static; ++j) {
                int orig = host_index[j];
                if (orig < 0) {
                    continue;
                }
                if (host_fds[j].revents) {
                    fds[orig].revents = host_fds[j].revents;
                    host_ready_count++;
                }
            }
            if (wake_enabled && host_fds[host_poll_count - 1].revents & POLLIN) {
                pscalPollDrain();
                wake_triggered = true;
            }

            int pscal_ready = 0;
            if (has_pscal && (wake_triggered || host_ready_count == 0)) {
                for (nfds_t i = 0; i < nfds; ++i) {
                    if (!pscal_fds[i]) {
                        continue;
                    }
                    int pscal_events = pscal_fds[i]->ops && pscal_fds[i]->ops->poll
                        ? pscal_fds[i]->ops->poll(pscal_fds[i])
                        : POLL_ERR;
                    short ready = vprocPollMapReady(pscal_events, fds[i].events);
                    ready |= vprocSessionInputPollEvents(session_stdio,
                                                         fds[i].fd,
                                                         pscal_fds[i],
                                                         fds[i].events);
                    fds[i].revents = ready;
                    if (ready) {
                        pscal_ready++;
                    }
                }
            }

            ready_count = host_ready_count + pscal_ready;
            if (ready_count == 0 && vprocCurrentHasInterruptingSignal()) {
                /* Woken (or timed out) with nothing ready but a deliverable
                 * signal pending: return EINTR so the blocked poll/select can
                 * be interrupted by Ctrl-C/Ctrl-Z instead of looping forever
                 * on an infinite timeout. */
                for (nfds_t k = 0; k < nfds; ++k) {
                    if (pscal_fds[k]) {
                        pscal_fd_close(pscal_fds[k]);
                    }
                }
                vprocDeliverPendingSignalsForCurrent();
                errno = EINTR;
                return -1;
            }
            if (ready_count > 0 || effective_wait == 0) {
                break;
            }

vproc_poll_update_timeout:
            if (wait_timeout < 0) {
                continue;
            }
            if (deadline_ns == 0) {
                break;
            }
            uint64_t now_ns = vprocNowMonoNs();
            if (now_ns >= deadline_ns) {
                break;
            }
            uint64_t remain_ns = deadline_ns - now_ns;
            wait_timeout = (int)((remain_ns + 999999ull) / 1000000ull);
            if (wait_timeout <= 0) {
                break;
            }
        }
    }

    for (nfds_t i = 0; i < nfds; ++i) {
        if (pscal_fds[i]) {
            pscal_fd_close(pscal_fds[i]);
        }
    }
    return ready_count;
}

int vprocSelectShim(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostSelectRaw(nfds, readfds, writefds, exceptfds, timeout);
    }
    if (nfds <= 0) {
        return vprocHostSelectRaw(nfds, readfds, writefds, exceptfds, timeout);
    }
    if ((readfds || writefds || exceptfds) && nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)) {
        errno = EINVAL;
        return -1;
    }

    int count = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (readfds && FD_ISSET(fd, readfds)) events |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds)) events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
        if (events != 0) {
            count++;
        }
    }

    int timeout_ms = -1;
    if (timeout) {
        int64_t total_ms = (int64_t)timeout->tv_sec * 1000 + (int64_t)(timeout->tv_usec / 1000);
        if (total_ms > INT_MAX) {
            total_ms = INT_MAX;
        }
        timeout_ms = (int)total_ms;
    }

    if (count == 0) {
        bool timer_limited = false;
        int wait_timeout = vprocClampTimeoutByCurrentRealTimer(vp, timeout_ms, &timer_limited);
        int res = vprocHostPollRaw(NULL, 0, wait_timeout);
        if (res < 0) {
            return -1;
        }
        if (timer_limited && res == 0) {
            vprocDeliverPendingSignalsForCurrent();
        }
        if (readfds) FD_ZERO(readfds);
        if (writefds) FD_ZERO(writefds);
        if (exceptfds) FD_ZERO(exceptfds);
        return 0;
    }

    struct pollfd pfds_stack[VPROC_SELECT_STACK_FDS];
    int fd_map_stack[VPROC_SELECT_STACK_FDS];
    bool use_stack = count <= VPROC_SELECT_STACK_FDS;
    struct pollfd *pfds = NULL;
    int *fd_map = NULL;
    if (use_stack) {
        memset(pfds_stack, 0, sizeof(pfds_stack));
        memset(fd_map_stack, 0, sizeof(fd_map_stack));
        pfds = pfds_stack;
        fd_map = fd_map_stack;
    } else {
        if (!vprocSelectScratchEnsure((size_t)count)) {
            return -1;
        }
        pfds = gVProcSelectScratch.pfds;
        fd_map = gVProcSelectScratch.fd_map;
    }

    int cursor = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (readfds && FD_ISSET(fd, readfds)) events |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds)) events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
        if (events == 0) {
            continue;
        }
        pfds[cursor].fd = fd;
        pfds[cursor].events = events;
        pfds[cursor].revents = 0;
        fd_map[cursor] = fd;
        cursor++;
    }
    int res = vprocPollShim(pfds, (nfds_t)count, timeout_ms);
    if (res < 0) {
        return -1;
    }

    if (readfds) FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);
    int ready = 0;
    for (int i = 0; i < count; ++i) {
        short revents = pfds[i].revents;
        int fd = fd_map[i];
        bool fd_ready = false;
        if (revents & POLLIN) {
            if (readfds) FD_SET(fd, readfds);
            fd_ready = true;
        }
        if (revents & POLLOUT) {
            if (writefds) FD_SET(fd, writefds);
            fd_ready = true;
        }
        if (revents & POLLPRI) {
            if (exceptfds) FD_SET(fd, exceptfds);
            fd_ready = true;
        }
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (exceptfds) FD_SET(fd, exceptfds);
            fd_ready = true;
        }
        if (fd_ready) {
            ready++;
        }
    }

    return ready;
}

int vprocOpenShim(const char *path, int flags, ...) {
    vprocDeliverPendingSignalsForCurrent();
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
    }
    int named_pid = 0;
    int named_fd = vprocPathNamedFdOf(path, &named_pid);
    if (named_fd >= 0) {
        return vprocOpenNamedFd(vp, named_pid, named_fd, flags);
    }
    if (vprocPathIsSystemPath(path)) {
        int host_fd = vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
        if (host_fd < 0) {
            return -1;
        }
        int slot = vprocInsert(vp, host_fd);
        if (slot < 0) {
            vprocHostClose(host_fd);
        }
        return slot;
    }
    if (vprocPathIsDevTty(path)) {
        struct pscal_fd *tty_fd = NULL;
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        /* A tab's commands carry its session, so its terminal is theirs. The controlling
         * terminal table goes by session id, and a tab whose thread took its terminal before
         * its shell had a task holds another tab's shell's id there: that table sent one tab's
         * /dev/tty to another, and to a closed one's after it closed. A process that let go
         * of its terminal has none, as on Linux. */
        if (!pscalTtyCallerGaveUpTerminal() &&
            session && !vprocSessionStdioIsDefault(session) && session->pty_active &&
            session->pty_slave && session->pty_slave->tty &&
            session->pty_slave->tty->driver == &pty_slave) {
            int err = pscalPtyOpenSlave(session->pty_slave->tty->num, flags | O_NOCTTY, &tty_fd);
            if (err < 0) {
                return vprocSetCompatErrno(err);
            }
            tty_fd->flags = (unsigned)flags;
            int slot = vprocInsertPscalFd(vp, tty_fd);
            /* fd table retains; drop caller ref regardless of outcome. */
            pscal_fd_close(tty_fd);
            return slot;
        }
        int err = pscalTtyOpenControlling(flags, &tty_fd);
        if (err == _EBUSY || (err == _ENXIO && pscalTtyCallerGaveUpTerminal())) {
            return vprocSetCompatErrno(err);
        }
        if (err == _ENXIO || err == _ENOTTY) {
            /* If this vproc never claimed a controlling TTY, bind the session
             * PTY as the controlling terminal for the current sid and retry. */
            int sid = vprocGetsidShim(0);
            if (session && session->pty_slave && session->pty_slave->tty && sid > 0) {
                struct tty *tty = session->pty_slave->tty;
                lock(&tty->lock);
                if (tty->session == 0 || tty->session == (pid_t_)sid) {
                    if (tty->session == 0) {
                        tty->session = (pid_t_)sid;
                        int fg = vprocGetForegroundPgid(sid);
                        tty->fg_group = (fg > 0) ? (pid_t_)fg : (pid_t_)sid;
                    }
                    unlock(&tty->lock);
                    pscalTtySetControlling(tty);
                    err = pscalTtyOpenControlling(flags, &tty_fd);
                } else {
                    unlock(&tty->lock);
                }
            }
        }
        if (err < 0) {
            /* Fall back to the session PTY if present so /dev/tty resolves for
             * pipeline stages that otherwise lack a controlling terminal. */
            if (session && session->pty_slave) {
                struct pscal_fd *retained = pscal_fd_retain(session->pty_slave);
                if (retained) {
                    int slot = vprocInsertPscalFd(vp, retained);
                    /* Table retains; release caller ref. */
                    pscal_fd_close(retained);
                    if (slot >= 0) {
                        return slot;
                    }
                }
            }
            /* A session on the host's own descriptors, as the iOS-flavoured shell has in a
             * host terminal, has the host's controlling terminal. */
            if (session && vprocSessionStdioIsDefault(session) && !session->pty_slave) {
                int host_fd = vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
                if (host_fd < 0) {
                    return -1;
                }
                int slot = vprocInsert(vp, host_fd);
                if (slot < 0) {
                    vprocHostClose(host_fd);
                }
                return slot;
            }
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(tty_fd);
            errno = ENOTSUP;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, tty_fd);
        /* fd table retains; drop caller ref regardless of outcome. */
        pscal_fd_close(tty_fd);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    if (vprocPathIsDevConsole(path)) {
        struct pscal_fd *tty_fd = NULL;
        int err = pscalTtyOpenControlling(flags, &tty_fd);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(tty_fd);
            errno = ENOTSUP;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, tty_fd);
        /* fd table retains; release caller ref. */
        pscal_fd_close(tty_fd);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    int tty_num = -1;
    if (vprocPathParseConsoleTty(path, &tty_num)) {
        if (tty_num == 1) {
            struct pscal_fd *tty_fd = NULL;
            int err = pscalTtyOpenControlling(flags, &tty_fd);
            if (err < 0) {
                return vprocSetCompatErrno(err);
            }
            if (!vp) {
                pscal_fd_close(tty_fd);
                errno = ENOTSUP;
                return -1;
            }
            int slot = vprocInsertPscalFd(vp, tty_fd);
            /* fd table retains; release caller ref. */
            pscal_fd_close(tty_fd);
            if (slot < 0) {
                return -1;
            }
            return slot;
        }
        const char *fallback = "/dev/null";
        if (!vp) {
            return vprocHostOpenVirtualized(fallback, flags, mode);
        }
        int host_fd = vprocHostOpenVirtualized(fallback, flags, mode);
        if (host_fd < 0) {
            return -1;
        }
        int slot = vprocInsert(vp, host_fd);
        if (slot < 0) {
            vprocHostClose(host_fd);
        }
        return slot;
    }
    if (vprocPathIsPtyMaster(path)) {
        struct pscal_fd *pty = NULL;
        int pty_num = -1;
        int err = pscalPtyOpenMaster(flags, &pty, &pty_num);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(pty);
            errno = ENOTSUP;
            return -1;
        }
        VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
        uint64_t session_id = (session_stdio && !vprocSessionStdioIsDefault(session_stdio))
                ? session_stdio->session_id
                : 0;
        struct pscal_fd *session_slave = NULL;
        if (session_id != 0) {
            int slave_flags = O_RDWR;
            if (flags & O_NONBLOCK) {
                slave_flags |= O_NONBLOCK;
            }
            int slave_err = pscalPtyOpenSlave(pty_num, slave_flags, &session_slave);
            if (slave_err < 0) {
                session_slave = NULL;
            }
        }
        int slot = vprocInsertPscalFd(vp, pty);
        if (slot < 0) {
            if (session_slave) {
                pscal_fd_close(session_slave);
            }
            /* drop caller ref */
            pscal_fd_close(pty);
            return -1;
        }
        if (session_id != 0 && session_slave) {
            /* register retains both ends */
            vprocSessionPtyRegister(session_id, session_slave, pty);
        }
        if (session_slave) {
            pscal_fd_close(session_slave);
        }
        /* fd table (and session registry) retain; drop caller ref last */
        pscal_fd_close(pty);
        return slot;
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        struct pscal_fd *pty = NULL;
        int err = pscalPtyOpenSlave(pty_num, flags, &pty);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(pty);
            errno = ENOTSUP;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, pty);
        /* fd table retains; release caller ref. */
        pscal_fd_close(pty);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    if (vprocPathIsLocationDevice(path)) {
        return vprocLocationDeviceOpen(vp, flags);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    const char *effective = target ? target : path;
    if (!vp) {
        /* Apply path virtualization even when vproc is inactive. */
        return vprocHostOpenVirtualized(effective, flags, mode);
    }
    bool dbg = vprocPipeDebugEnabled();
    int host_fd = vprocHostOpenVirtualized(effective, flags, mode);
#if defined(PSCAL_TARGET_IOS)
    if (host_fd < 0 && errno == ENOENT) {
        if (dbg) vprocDebugLogf( "[vproc-open] (shim) virtualized ENOENT for %s, fallback raw\n", effective);
        host_fd = vprocHostOpenRawInternal(effective, flags, (mode_t)mode, (flags & O_CREAT) != 0);
    }
    if (dbg && host_fd >= 0) {
        vprocDebugLogf( "[vproc-open] (shim) opened %s -> host_fd=%d flags=0x%x\n", effective, host_fd, flags);
    }
#endif
    if (host_fd < 0) {
        if (vprocToolDebugEnabled()) {
            vprocDebugLogf( "[vproc-open] path=%s flags=%d errno=%d\n", effective, flags, errno);
        }
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        vprocHostClose(host_fd);
    }
    return slot;
}

int vprocSigactionShim(int sig, const struct sigaction *act, struct sigaction *oldact) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigactionRaw(sig, act, oldact);
    }
    int rc = vprocSigaction(vprocPid(vp), sig, act, oldact);
#if defined(PSCAL_TARGET_IOS)
    /* Resize notifications from the runtime use pthread_kill(SIGWINCH) on the
     * host thread. Mirror SIGWINCH handlers into host sigaction so those
     * notifications can wake OpenSSH clientloop even when signal handling is
     * virtualized through vproc. */
    if (rc == 0 && act && sig == SIGWINCH) {
        (void)vprocHostSigactionRaw(sig, act, NULL);
    }
#endif
    return rc;
}

int vprocSigprocmaskShim(int how, const sigset_t *set, sigset_t *oldset) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigprocmaskRaw(how, set, oldset);
    }
    return vprocSigprocmask(vprocPid(vp), how, set, oldset);
}

int vprocSigpendingShim(sigset_t *set) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigpendingRaw(set);
    }
    return vprocSigpending(vprocPid(vp), set);
}

int vprocSigsuspendShim(const sigset_t *mask) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigsuspendRaw(mask);
    }
    return vprocSigsuspend(vprocPid(vp), mask);
}

int vprocPthreadSigmaskShim(int how, const sigset_t *set, sigset_t *oldset) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostPthreadSigmaskRaw(how, set, oldset);
    }
    if (vprocSigprocmask(vprocPid(vp), how, set, oldset) == 0) {
        return 0;
    }
    return errno ? errno : EINVAL;
}

int vprocGetitimerShim(int which, struct itimerval *curr_value) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return getitimer(which, curr_value);
    }
    if (!curr_value) {
        errno = EINVAL;
        return -1;
    }
    if (which != ITIMER_REAL) {
        errno = ENOTSUP;
        return -1;
    }
    int pid = vprocPid(vp);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    uint64_t now_ns = vprocNowMonoNs();
    if (vprocTaskExpireRealTimerLocked(entry, now_ns)) {
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    vprocTaskFillRealTimerLocked(entry, now_ns, curr_value);
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

int vprocSetitimerShim(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return setitimer(which, new_value, old_value);
    }
    if (which != ITIMER_REAL) {
        errno = ENOTSUP;
        return -1;
    }
    if (!new_value || !vprocTimevalValid(&new_value->it_value) || !vprocTimevalValid(&new_value->it_interval)) {
        errno = EINVAL;
        return -1;
    }
    int pid = vprocPid(vp);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    uint64_t now_ns = vprocNowMonoNs();
    if (vprocTaskExpireRealTimerLocked(entry, now_ns)) {
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    if (old_value) {
        vprocTaskFillRealTimerLocked(entry, now_ns, old_value);
    }
    vprocTaskSetRealTimerLocked(entry, new_value, now_ns);
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

unsigned int vprocAlarmShim(unsigned int seconds) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return alarm(seconds);
    }
    struct itimerval old_timer;
    struct itimerval new_timer;
    memset(&old_timer, 0, sizeof(old_timer));
    memset(&new_timer, 0, sizeof(new_timer));
    new_timer.it_value.tv_sec = (time_t)seconds;
    if (vprocSetitimerShim(ITIMER_REAL, &new_timer, &old_timer) != 0) {
        return 0;
    }
    uint64_t old_ns = vprocTimevalToNs(&old_timer.it_value);
    return vprocNsToSecondsCeil(old_ns);
}

useconds_t vprocUalarmShim(useconds_t useconds, useconds_t interval_useconds) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return ualarm(useconds, interval_useconds);
    }
    struct itimerval old_timer;
    struct itimerval new_timer;
    memset(&old_timer, 0, sizeof(old_timer));
    memset(&new_timer, 0, sizeof(new_timer));
    new_timer.it_value.tv_sec = (time_t)(useconds / 1000000u);
    new_timer.it_value.tv_usec = (suseconds_t)(useconds % 1000000u);
    new_timer.it_interval.tv_sec = (time_t)(interval_useconds / 1000000u);
    new_timer.it_interval.tv_usec = (suseconds_t)(interval_useconds % 1000000u);
    if (vprocSetitimerShim(ITIMER_REAL, &new_timer, &old_timer) != 0) {
        return (useconds_t)-1;
    }
    uint64_t old_us = (uint64_t)old_timer.it_value.tv_sec * 1000000ull + (uint64_t)old_timer.it_value.tv_usec;
    if (old_us > (uint64_t)UINT_MAX) {
        return (useconds_t)UINT_MAX;
    }
    return (useconds_t)old_us;
}

int vprocRaiseShim(int sig) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostRaiseRaw(sig);
    }
    return vprocKillShim(vprocPid(vp), sig);
}

VProcSigHandler vprocSignalShim(int sig, VProcSigHandler handler) {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    if (vprocSigactionShim(sig, &sa, &old) != 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}


#if defined(PSCAL_TARGET_IOS)
/* --- Credentials -----------------------------------------------------------------------------
 *
 * A task's uids and gids, the way a process has them. They live in its vproc beside its file mask,
 * and a new vproc copies its creator's, so what a command starts runs as the command does. A
 * session starts as the login account: the host's uid, which is the one real user underneath
 * (mobile, 501, on a device), with that account's group and groups from the app's /etc/passwd and
 * /etc/group rather than the host's. Only exec of a set-user-id program makes a task root. */

typedef struct {
    uid_t uid;
    gid_t gid;
    int ngroups;
    gid_t groups[VPROC_NGROUPS_MAX];
    bool found;  /* the account was in /etc/passwd */
} VProcLoginCred;

static VProcLoginCred gVprocLoginCred;
static pthread_mutex_t gVprocLoginCredMu = PTHREAD_MUTEX_INITIALIZER;
static bool gVprocLoginCredReady;

/* Every group in /etc/group that lists `user`, after `gid`. Read here rather than through
 * getgrent, whose loader keeps no member lists. */
static int vprocGroupsForUser(const char *user, gid_t gid, gid_t *out, int max) {
    int count = 0;
    if (max <= 0) {
        return 0;
    }
    out[count++] = gid;
    char path[PATH_MAX];
    const char *group_path = user ? pscalEtcPath("group", path, sizeof(path)) : NULL;
    FILE *fp = group_path ? fopen(group_path, "r") : NULL;
    if (!fp) {
        return count;
    }
    char line[2048];
    while (count < max && fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') {
            continue;
        }
        char *save = NULL;
        char *name = strtok_r(line, ":\n", &save);
        char *pass = name ? strtok_r(NULL, ":\n", &save) : NULL;
        char *gid_text = pass ? strtok_r(NULL, ":\n", &save) : NULL;
        char *members = gid_text ? strtok_r(NULL, "\n", &save) : NULL;
        if (!gid_text || !members) {
            continue;
        }
        gid_t group_gid = (gid_t)strtoul(gid_text, NULL, 10);
        char *msave = NULL;
        for (char *m = strtok_r(members, ",", &msave); m; m = strtok_r(NULL, ",", &msave)) {
            if (strcmp(m, user) != 0) {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < count; ++i) {
                if (out[i] == group_gid) {
                    seen = true;
                }
            }
            if (!seen) {
                out[count++] = group_gid;
            }
            break;
        }
    }
    fclose(fp);
    return count;
}

static void vprocLoginCredInit(void) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    char name[256] = { 0 };
    struct passwd *pw = pscalGetpwuid(uid);
    if (pw) {
        gid = pw->pw_gid;
        if (pw->pw_name) {
            strlcpy(name, pw->pw_name, sizeof(name));
        }
    }
    gVprocLoginCred.uid = uid;
    gVprocLoginCred.gid = gid;
    gVprocLoginCred.ngroups = vprocGroupsForUser(name[0] ? name : NULL, gid,
                                                 gVprocLoginCred.groups, VPROC_NGROUPS_MAX);
    gVprocLoginCred.found = pw != NULL;
}

/* The login account. At a first launch the installer may not have written /etc/passwd yet, so an
 * answer without it is asked again next time. */
static const VProcLoginCred *vprocLoginCred(void) {
    static time_t last_try;
    pthread_mutex_lock(&gVprocLoginCredMu);
    time_t now = time(NULL);
    if (!gVprocLoginCredReady || (!gVprocLoginCred.found && now != last_try)) {
        vprocLoginCredInit();
        gVprocLoginCredReady = true;
        last_try = now;
    }
    pthread_mutex_unlock(&gVprocLoginCredMu);
    return &gVprocLoginCred;
}

static void vprocCredSetLogin(VProc *vp, const VProcLoginCred *login) {
    vp->ruid = vp->euid = vp->suid = login->uid;
    vp->rgid = vp->egid = vp->sgid = login->gid;
    vp->ngroups = login->ngroups;
    memcpy(vp->groups, login->groups, sizeof(vp->groups));
    vp->cred_provisional = !login->found;
}

/* A task that started as the login account before its entry could be read takes it now. */
static VProc *vprocCredCurrent(void) {
    VProc *vp = vprocForThread();
    if (vp && vp->cred_provisional && vp->ruid == vp->euid) {
        const VProcLoginCred *login = vprocLoginCred();
        if (login->found && login->uid == vp->ruid) {
            vprocCredSetLogin(vp, login);
        }
    }
    return vp;
}
#endif

static void vprocCredInherit(VProc *vp, const VProc *parent) {
#if defined(PSCAL_TARGET_IOS)
    if (parent) {
        vp->ruid = parent->ruid;
        vp->euid = parent->euid;
        vp->suid = parent->suid;
        vp->rgid = parent->rgid;
        vp->egid = parent->egid;
        vp->sgid = parent->sgid;
        vp->ngroups = parent->ngroups;
        memcpy(vp->groups, parent->groups, sizeof(vp->groups));
        vp->cred_provisional = parent->cred_provisional;
        if (!vp->cred_provisional) {
            return;
        }
        const VProcLoginCred *login = vprocLoginCred();
        if (login->found && login->uid == vp->ruid && vp->ruid == vp->euid) {
            vprocCredSetLogin(vp, login);
        }
        return;
    }
    vprocCredSetLogin(vp, vprocLoginCred());
#else
    if (parent) {
        vp->ruid = parent->ruid;
        vp->euid = parent->euid;
        vp->suid = parent->suid;
        vp->rgid = parent->rgid;
        vp->egid = parent->egid;
        vp->sgid = parent->sgid;
        vp->ngroups = parent->ngroups;
        memcpy(vp->groups, parent->groups, sizeof(vp->groups));
        return;
    }
    vp->ruid = vp->euid = vp->suid = getuid();
    vp->rgid = vp->egid = vp->sgid = getgid();
    vp->ngroups = 1;
    vp->groups[0] = vp->rgid;
#endif
}

#if defined(PSCAL_TARGET_IOS)
/* kill's check reads the target's ids from the task table, which other threads hold. */
static void vprocCredPublish(VProc *vp) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vp->pid);
    if (entry) {
        entry->cred_ruid = vp->ruid;
        entry->cred_euid = vp->euid;
        entry->cred_suid = vp->suid;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

uid_t vprocGetuidShim(void) {
    VProc *vp = vprocCredCurrent();
    return vp ? vp->ruid : vprocLoginCred()->uid;
}

uid_t vprocGeteuidShim(void) {
    VProc *vp = vprocCredCurrent();
    return vp ? vp->euid : vprocLoginCred()->uid;
}

gid_t vprocGetgidShim(void) {
    VProc *vp = vprocCredCurrent();
    return vp ? vp->rgid : vprocLoginCred()->gid;
}

gid_t vprocGetegidShim(void) {
    VProc *vp = vprocCredCurrent();
    return vp ? vp->egid : vprocLoginCred()->gid;
}

int vprocGetgroupsShim(int size, gid_t list[]) {
    VProc *vp = vprocCredCurrent();
    const VProcLoginCred *login = vprocLoginCred();
    int count = vp ? vp->ngroups : login->ngroups;
    const gid_t *groups = vp ? vp->groups : login->groups;
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0) {
        return count;
    }
    if (size < count) {
        errno = EINVAL;
        return -1;
    }
    memcpy(list, groups, (size_t)count * sizeof(gid_t));
    return count;
}

/* The app's own threads have no task, and nothing changes who they are. */
static VProc *vprocCredTarget(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        errno = EPERM;
    }
    return vp;
}

int vprocSetuidShim(uid_t uid) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    if (vp->euid == 0) {
        vp->ruid = vp->euid = vp->suid = uid;
    } else if (uid == vp->ruid || uid == vp->suid) {
        vp->euid = uid;
    } else {
        errno = EPERM;
        return -1;
    }
    vprocCredPublish(vp);
    return 0;
}

int vprocSeteuidShim(uid_t uid) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    if (vp->euid != 0 && uid != vp->ruid && uid != vp->suid) {
        errno = EPERM;
        return -1;
    }
    vp->euid = uid;
    vprocCredPublish(vp);
    return 0;
}

/* Linux's rules: an unprivileged task may swap its real and effective ids, or set either to its
 * saved one; setting the real id, or an effective one other than the old real, saves the new one. */
int vprocSetreuidShim(uid_t ruid, uid_t euid) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    bool root = vp->euid == 0;
    uid_t new_ruid = (ruid == (uid_t)-1) ? vp->ruid : ruid;
    uid_t new_euid = (euid == (uid_t)-1) ? vp->euid : euid;
    if (!root) {
        if (ruid != (uid_t)-1 && ruid != vp->ruid && ruid != vp->euid) {
            errno = EPERM;
            return -1;
        }
        if (euid != (uid_t)-1 && euid != vp->ruid && euid != vp->euid && euid != vp->suid) {
            errno = EPERM;
            return -1;
        }
    }
    if (ruid != (uid_t)-1 || (euid != (uid_t)-1 && new_euid != vp->ruid)) {
        vp->suid = new_euid;
    }
    vp->ruid = new_ruid;
    vp->euid = new_euid;
    vprocCredPublish(vp);
    return 0;
}

int vprocSetgidShim(gid_t gid) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    if (vp->euid == 0) {
        vp->rgid = vp->egid = vp->sgid = gid;
    } else if (gid == vp->rgid || gid == vp->sgid) {
        vp->egid = gid;
    } else {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int vprocSetegidShim(gid_t gid) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    if (vp->euid != 0 && gid != vp->rgid && gid != vp->sgid) {
        errno = EPERM;
        return -1;
    }
    vp->egid = gid;
    return 0;
}

int vprocSetregidShim(gid_t rgid, gid_t egid) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    bool root = vp->euid == 0;
    gid_t new_rgid = (rgid == (gid_t)-1) ? vp->rgid : rgid;
    gid_t new_egid = (egid == (gid_t)-1) ? vp->egid : egid;
    if (!root) {
        if (rgid != (gid_t)-1 && rgid != vp->rgid && rgid != vp->egid) {
            errno = EPERM;
            return -1;
        }
        if (egid != (gid_t)-1 && egid != vp->rgid && egid != vp->egid && egid != vp->sgid) {
            errno = EPERM;
            return -1;
        }
    }
    if (rgid != (gid_t)-1 || (egid != (gid_t)-1 && new_egid != vp->rgid)) {
        vp->sgid = new_egid;
    }
    vp->rgid = new_rgid;
    vp->egid = new_egid;
    return 0;
}

int vprocSetgroupsShim(int size, const gid_t list[]) {
    VProc *vp = vprocCredTarget();
    if (!vp) {
        return -1;
    }
    if (vp->euid != 0) {
        errno = EPERM;
        return -1;
    }
    if (size < 0 || size > VPROC_NGROUPS_MAX || (size > 0 && !list)) {
        errno = EINVAL;
        return -1;
    }
    vp->ngroups = size;
    if (size > 0) {
        memcpy(vp->groups, list, (size_t)size * sizeof(gid_t));
    }
    return 0;
}

int vprocInitgroupsShim(const char *user, gid_t gid) {
    gid_t groups[VPROC_NGROUPS_MAX];
    int count = vprocGroupsForUser(user, gid, groups, VPROC_NGROUPS_MAX);
    return vprocSetgroupsShim(count, groups);
}

/* POSIX's rule: the sender's real or effective uid must be the target's real or saved one. Within
 * a session the terminal's owner keeps control of its jobs, as sudo's relay gives a Linux user over
 * the command sudo runs, so only another session's tasks are checked. Groups and broadcasts are
 * not checked yet. */
static bool vprocKillPermitted(pid_t pid) {
    VProc *caller = vprocForThread();
    if (!caller || caller->euid == 0 || pid <= 0) {
        return true;
    }
    int caller_sid = vprocGetSid(caller->pid);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked((int)pid);
    bool permitted = true;
    if (entry && !entry->exited && !(caller_sid > 0 && entry->sid == caller_sid)) {
        permitted = caller->ruid == entry->cred_ruid || caller->ruid == entry->cred_suid ||
                    caller->euid == entry->cred_ruid || caller->euid == entry->cred_suid;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return permitted;
}

/* --- What root gates ------------------------------------------------------------------------
 *
 * A normal user can't make, change or remove anything under /etc, or read /etc/shadow, or touch
 * sudo's records under /var/run/sudo. The check is on the host path after expansion, resolved
 * with realpath, so `cd /etc; rm x`, `..` and a symlink pointing in are all the same file. It adds
 * to the host's permissions and replaces none. */

typedef struct {
    char etc[PATH_MAX];
    char sudo_run[PATH_MAX];
    char source_etc[PATH_MAX];
} VProcGuardRoots;

typedef struct {
    dev_t dev;
    ino_t ino;
    bool present;
} VProcGuardSecret;

#define VPROC_GUARD_SECRETS 4
static const char *const kVprocGuardSecretNames[VPROC_GUARD_SECRETS] = { "shadow", "gshadow", "shadow-", "shadow.tmp" };

/* The roots and the secret files' identities, kept for a moment: every open and stat by a task
 * asks, and the answers change only when the app's root does or passwd replaces a file. */
static pthread_mutex_t gVprocGuardMu = PTHREAD_MUTEX_INITIALIZER;
static VProcGuardRoots gVprocGuardRootsCache;
static VProcGuardSecret gVprocGuardSecrets[VPROC_GUARD_SECRETS];
static uint64_t gVprocGuardStamp;

static uint64_t vprocGuardNowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void vprocGuardRootsCompute(VProcGuardRoots *roots) {
    char expanded[PATH_MAX];
    roots->etc[0] = roots->sudo_run[0] = '\0';
    if (!pathTruncateExpand("/etc", expanded, sizeof(expanded))) {
        strlcpy(expanded, "/etc", sizeof(expanded));
    }
    strlcpy(roots->source_etc, expanded, sizeof(roots->source_etc));
    if (!realpath(expanded, roots->etc)) {
        strlcpy(roots->etc, expanded, sizeof(roots->etc));
    }
    char run[PATH_MAX];
    if (!pathTruncateExpand("/var/run", run, sizeof(run))) {
        strlcpy(run, "/var/run", sizeof(run));
    }
    char run_real[PATH_MAX];
    if (!realpath(run, run_real)) {
        strlcpy(run_real, run, sizeof(run_real));
    }
    snprintf(roots->sudo_run, sizeof(roots->sudo_run), "%s/sudo", run_real);
}

/* The host directories behind /etc and /var/run/sudo, which follow the container root. With
 * secrets, the identities of /etc/shadow and its kind too. */
static bool vprocGuardRootsWith(VProcGuardRoots *roots, VProcGuardSecret secrets[VPROC_GUARD_SECRETS]) {
    char expanded[PATH_MAX];
    if (!pathTruncateExpand("/etc", expanded, sizeof(expanded))) {
        strlcpy(expanded, "/etc", sizeof(expanded));
    }
    uint64_t now = vprocGuardNowNs();
    pthread_mutex_lock(&gVprocGuardMu);
    if (gVprocGuardStamp == 0 || now - gVprocGuardStamp > 500000000ull ||
        strcmp(expanded, gVprocGuardRootsCache.source_etc) != 0) {
        vprocGuardRootsCompute(&gVprocGuardRootsCache);
        for (int i = 0; i < VPROC_GUARD_SECRETS; ++i) {
            char secret[PATH_MAX];
            struct stat st;
            gVprocGuardSecrets[i].present =
                snprintf(secret, sizeof(secret), "%s/%s", gVprocGuardRootsCache.source_etc,
                         kVprocGuardSecretNames[i]) < (int)sizeof(secret) &&
                vprocHostStatRaw(secret, &st) == 0;
            if (gVprocGuardSecrets[i].present) {
                gVprocGuardSecrets[i].dev = st.st_dev;
                gVprocGuardSecrets[i].ino = st.st_ino;
            }
        }
        gVprocGuardStamp = now;
    }
    *roots = gVprocGuardRootsCache;
    if (secrets) {
        memcpy(secrets, gVprocGuardSecrets, sizeof(gVprocGuardSecrets));
    }
    pthread_mutex_unlock(&gVprocGuardMu);
    return true;
}

static bool vprocGuardRoots(VProcGuardRoots *roots) {
    return vprocGuardRootsWith(roots, NULL);
}

/* passwd has just replaced a file the cache knows. */
static void vprocGuardForget(void) {
    pthread_mutex_lock(&gVprocGuardMu);
    gVprocGuardStamp = 0;
    pthread_mutex_unlock(&gVprocGuardMu);
}

static bool vprocPathWithin(const char *path, const char *root) {
    size_t len = strlen(root);
    if (len == 0 || strncmp(path, root, len) != 0) {
        return false;
    }
    return path[len] == '\0' || path[len] == '/';
}

/* The file a host path names, canonical. With follow, a final symlink is followed when its target
 * exists; a name that doesn't exist yet is its directory's real path plus the name. */
static bool vprocCanonicalHostPath(const char *path, bool follow, char out[PATH_MAX]) {
    if (follow && realpath(path, out)) {
        return true;
    }
    char copy[PATH_MAX];
    if (strlcpy(copy, path, sizeof(copy)) >= sizeof(copy)) {
        return false;
    }
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    char *slash = strrchr(copy, '/');
    const char *base = slash ? slash + 1 : copy;
    char dir[PATH_MAX];
    if (!slash) {
        strlcpy(dir, ".", sizeof(dir));
    } else if (slash == copy) {
        strlcpy(dir, "/", sizeof(dir));
    } else {
        *slash = '\0';
        strlcpy(dir, copy, sizeof(dir));
    }
    if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0 || base[0] == '\0') {
        return realpath(path, out) != NULL;
    }
    char dir_real[PATH_MAX];
    if (!realpath(dir, dir_real)) {
        return false;
    }
    int written = snprintf(out, PATH_MAX, "%s%s%s", dir_real,
                           (dir_real[strlen(dir_real) - 1] == '/') ? "" : "/", base);
    return written > 0 && written < PATH_MAX;
}

/* /etc/shadow and its kind, by the file itself: a symlink or a hard link to one is the same file.
 * A file replaced since the cache was filled is checked again by name. */
static bool vprocGuardIsSecretFile(const char *host_path, const VProcGuardRoots *roots,
                                   const VProcGuardSecret secrets[VPROC_GUARD_SECRETS]) {
    struct stat st;
    if (vprocHostStatRaw(host_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    for (int i = 0; i < VPROC_GUARD_SECRETS; ++i) {
        if (secrets[i].present && secrets[i].dev == st.st_dev && secrets[i].ino == st.st_ino) {
            return true;
        }
    }
    char canonical[PATH_MAX];
    if (!realpath(host_path, canonical)) {
        return false;
    }
    size_t len = strlen(roots->etc);
    if (strncmp(canonical, roots->etc, len) != 0 || canonical[len] != '/') {
        return false;
    }
    for (int i = 0; i < VPROC_GUARD_SECRETS; ++i) {
        if (strcmp(canonical + len + 1, kVprocGuardSecretNames[i]) == 0) {
            vprocGuardForget();
            return true;
        }
    }
    return false;
}

int vprocHostPathPermit(const char *host_path, int op) {
    VProc *vp = vprocForThread();
    if (!vp || vp->euid == 0 || !host_path || host_path[0] == '\0') {
        return 0;
    }
    VProcGuardRoots roots;
    VProcGuardSecret secrets[VPROC_GUARD_SECRETS];
    vprocGuardRootsWith(&roots, secrets);
    bool denied = false;
    if (op & (VPROC_PERM_WRITE | VPROC_PERM_ENTRY)) {
        char canonical[PATH_MAX];
        bool follow = (op & VPROC_PERM_ENTRY) == 0;
        /* Nothing to resolve: the call itself fails, as it would have. */
        if (vprocCanonicalHostPath(host_path, follow, canonical)) {
            denied = vprocPathWithin(canonical, roots.etc) || vprocPathWithin(canonical, roots.sudo_run);
        }
    }
    if (!denied && (op & VPROC_PERM_READ)) {
        denied = vprocGuardIsSecretFile(host_path, &roots, secrets);
    }
    if (denied) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int vprocHostPathPermitOpen(const char *host_path, int flags) {
    int op = 0;
    int access_mode = flags & O_ACCMODE;
    if (access_mode == O_RDONLY || access_mode == O_RDWR) {
        op |= VPROC_PERM_READ;
    }
    if (access_mode == O_WRONLY || access_mode == O_RDWR || (flags & (O_TRUNC | O_APPEND))) {
        op |= VPROC_PERM_WRITE;
    }
    if (flags & O_CREAT) {
        struct stat st;
        if (stat(host_path, &st) != 0) {
            op |= VPROC_PERM_WRITE;
        }
    }
    return vprocHostPathPermit(host_path, op);
}

#define VPROC_OWNER_XATTR "user.pscal.owner"

/* A recorded owner, which chown as root writes; otherwise what the installer put in /etc is root's,
 * and anything else stays the one real user's. */
void vprocHostPathOwner(const char *host_path, struct stat *st) {
    if (!host_path || !st) {
        return;
    }
    char value[64];
#if defined(__APPLE__)
    ssize_t len = getxattr(host_path, VPROC_OWNER_XATTR, value, sizeof(value) - 1, 0, 0);
#else
    ssize_t len = getxattr(host_path, VPROC_OWNER_XATTR, value, sizeof(value) - 1);
#endif
    if (len > 0) {
        value[len] = '\0';
        unsigned long uid = 0;
        unsigned long gid = 0;
        if (sscanf(value, "%lu:%lu", &uid, &gid) == 2) {
            st->st_uid = (uid_t)uid;
            st->st_gid = (gid_t)gid;
            return;
        }
    }
    if (st->st_uid != vprocLoginCred()->uid) {
        return;
    }
    VProcGuardRoots roots;
    vprocGuardRoots(&roots);
    /* Only a path that names /etc as written: a symlink elsewhere shows its own owner. */
    if (!vprocPathWithin(host_path, roots.source_etc) && !vprocPathWithin(host_path, roots.etc) &&
        strstr(host_path, "/var/run/sudo") == NULL) {
        return;
    }
    char canonical[PATH_MAX];
    if (!vprocCanonicalHostPath(host_path, false, canonical)) {
        return;
    }
    if (vprocPathWithin(canonical, roots.etc) || vprocPathWithin(canonical, roots.sudo_run)) {
        st->st_uid = 0;
        st->st_gid = 0;
    }
}

/* chown: the one real user underneath owns every file, so the owner is recorded, for stat to show.
 * Only root gives a file away; a normal user may only change its group to one of their own. */
static int vprocChownRecord(const char *host_path, uid_t uid, gid_t gid, bool follow) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostChownRaw(host_path, uid, gid);
    }
    struct stat st;
    int rc = follow ? stat(host_path, &st) : lstat(host_path, &st);
    if (rc != 0) {
        return -1;
    }
    vprocHostPathOwner(host_path, &st);
    uid_t new_uid = (uid == (uid_t)-1) ? st.st_uid : uid;
    gid_t new_gid = (gid == (gid_t)-1) ? st.st_gid : gid;
    if (vp->euid != 0) {
        bool own_group = new_gid == vp->egid;
        for (int i = 0; i < vp->ngroups && !own_group; ++i) {
            own_group = vp->groups[i] == new_gid;
        }
        if (st.st_uid != vp->euid || new_uid != st.st_uid || (new_gid != st.st_gid && !own_group)) {
            errno = EPERM;
            return -1;
        }
    }
    if (vprocHostPathPermit(host_path, VPROC_PERM_WRITE) != 0) {
        return -1;
    }
    char value[64];
    snprintf(value, sizeof(value), "%lu:%lu", (unsigned long)new_uid, (unsigned long)new_gid);
    int options = 0;
#if defined(__APPLE__)
    if (!follow) {
        options |= XATTR_NOFOLLOW;
    }
    rc = setxattr(host_path, VPROC_OWNER_XATTR, value, strlen(value), 0, options);
#else
    (void)options;
    rc = follow ? setxattr(host_path, VPROC_OWNER_XATTR, value, strlen(value), 0)
                : lsetxattr(host_path, VPROC_OWNER_XATTR, value, strlen(value), 0);
#endif
    /* A filesystem without attributes still lets root chown; there is nothing to show. */
    (void)rc;
    return 0;
}

/* --- exec --------------------------------------------------------------------------------------
 *
 * There is one process, so a program can't replace it: exec runs the new program as a call on
 * this task, then ends the task with its status, which is all a program can see of exec. The app
 * says what can run and how. A set-user-id program gets effective uid 0 here, from a short table,
 * because applets have no files to carry the bit. */

static VProcExecCanRunFn gVprocExecCanRun;
static VProcExecRunFn gVprocExecRun;
static pid_t gVprocHostPid;

void vprocSetExecHooks(VProcExecCanRunFn can_run, VProcExecRunFn run) {
    if (gVprocHostPid == 0) {
        gVprocHostPid = getpid();
    }
    gVprocExecCanRun = can_run;
    gVprocExecRun = run;
}

static bool vprocExecIsSetuidProgram(const char *path) {
    static const char *const kSetuidPrograms[] = { "su", "sudo", "passwd", "login" };
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (size_t i = 0; i < sizeof(kSetuidPrograms) / sizeof(kSetuidPrograms[0]); ++i) {
        if (strcmp(base, kSetuidPrograms[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Called by the app as each program starts, as exec would: the set-user-id ones get root. */
void vprocExecApplyCredentials(const char *program) {
    VProc *vp = vprocForThread();
    if (!vp || !program || !vprocExecIsSetuidProgram(program)) {
        return;
    }
    vp->euid = 0;
    vp->suid = 0;
    vprocCredPublish(vp);
}

/* What the program sees as its environment: envp, in place of the task's own. */
static void vprocExecReplaceEnvironment(char *const envp[]) {
    char **current = *vprocEnvironShim();
    size_t count = 0;
    while (current && current[count]) {
        count++;
    }
    char **names = calloc(count + 1, sizeof(char *));
    for (size_t i = 0; names && i < count; ++i) {
        const char *eq = strchr(current[i], '=');
        names[i] = eq ? strndup(current[i], (size_t)(eq - current[i])) : strdup(current[i]);
    }
    for (size_t i = 0; names && i < count; ++i) {
        if (names[i]) {
            vprocUnsetenvShim(names[i]);
            free(names[i]);
        }
    }
    free(names);
    for (size_t i = 0; envp && envp[i]; ++i) {
        const char *eq = strchr(envp[i], '=');
        if (!eq || eq == envp[i]) {
            continue;
        }
        char *name = strndup(envp[i], (size_t)(eq - envp[i]));
        if (name) {
            vprocSetenvShim(name, eq + 1, 1);
            free(name);
        }
    }
}

int vprocProgramFrameStatus(const VProcProgramFrame *frame, int jumped) {
    if (frame && frame->exec_ended) {
        return frame->exec_status;
    }
    return jumped;
}

/* The program has ended, so the task does: back to where the app started it. */
static void vprocExecEnd(VProc *vp, int status) {
    VProcProgramFrame *frame = gVprocProgramFrame;
    if (vp && frame && frame->pid == vp->pid) {
        gVprocProgramFrame = frame->prev;
        frame->pushed = false;
        frame->exec_ended = true;
        frame->exec_status = status & 0xff;
        gVprocHandlerTask = frame->handler_task;
#if defined(__APPLE__)
        pthread_self()->__cleanup_stack = frame->cleanup_stack;
#endif
        longjmp(frame->env, 1);
    }
    exit(status & 0xff);
}

int vprocExecveShim(const char *path, char *const argv[], char *const envp[]) {
    if (!path || path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    /* A real child of a real fork, on a host build, is a process of its own. */
    if (gVprocHostPid != 0 && getpid() != gVprocHostPid) {
        return execve(path, argv, envp ? envp : *vprocEnvironShim());
    }
    /* Nothing says how a program runs here, as in a host build with no applets: the host's exec. */
    if (!gVprocExecCanRun || !gVprocExecRun) {
        return execve(path, argv, envp ? envp : *vprocEnvironShim());
    }
    if (!gVprocExecCanRun(path)) {
        char expanded[PATH_MAX];
        const char *host = pscalPathVirtualized_hostPath(path, expanded);
        errno = (strchr(path, '/') && access(host, F_OK) == 0) ? EACCES : ENOENT;
        return -1;
    }
    int argc = 0;
    while (argv && argv[argc]) {
        argc++;
    }
    char **args = calloc((size_t)(argc > 0 ? argc : 1) + 1, sizeof(char *));
    if (!args) {
        errno = ENOMEM;
        return -1;
    }
    /* The app finds the program by its first word. */
    args[0] = strdup(path);
    for (int i = 1; i < argc; ++i) {
        args[i] = strdup(argv[i]);
    }
    int count = argc > 0 ? argc : 1;
    VProc *vp = vprocForThread();
    if (envp && envp != *vprocEnvironShim()) {
        vprocExecReplaceEnvironment(envp);
    }
    vprocExecApplyCredentials(path);
    if (vp) {
        vprocSimExecResetCaughtSignals(vp->pid);
    }
    int status = gVprocExecRun(count, args);
    for (int i = 0; i < count; ++i) {
        free(args[i]);
    }
    free(args);
    vprocExecEnd(vp, status);
    return -1;
}

int vprocExecvShim(const char *path, char *const argv[]) {
    return vprocExecveShim(path, argv, NULL);
}

int vprocExecvpShim(const char *file, char *const argv[]) {
    if ((gVprocHostPid != 0 && getpid() != gVprocHostPid) || !gVprocExecRun) {
        return execvp(file, argv);
    }
    return vprocExecveShim(file, argv, NULL);
}

static char **vprocExecCollectArgs(const char *arg0, va_list ap) {
    size_t cap = 8;
    size_t count = 0;
    char **args = calloc(cap, sizeof(char *));
    if (!args) {
        return NULL;
    }
    const char *arg = arg0;
    while (arg) {
        if (count + 1 >= cap) {
            cap *= 2;
            char **grown = realloc(args, cap * sizeof(char *));
            if (!grown) {
                free(args);
                return NULL;
            }
            args = grown;
        }
        args[count++] = (char *)arg;
        arg = va_arg(ap, const char *);
    }
    args[count] = NULL;
    return args;
}

int vprocExeclShim(const char *path, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **args = vprocExecCollectArgs(arg0, ap);
    va_end(ap);
    if (!args) {
        errno = ENOMEM;
        return -1;
    }
    int rc = vprocExecveShim(path, args, NULL);
    int saved = errno;
    free(args);
    errno = saved;
    return rc;
}

int vprocExeclpShim(const char *file, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **args = vprocExecCollectArgs(arg0, ap);
    va_end(ap);
    if (!args) {
        errno = ENOMEM;
        return -1;
    }
    int rc = vprocExecvpShim(file, args);
    int saved = errno;
    free(args);
    errno = saved;
    return rc;
}

/* --- The host name -----------------------------------------------------------------------------
 *
 * The kernel's, as on Linux: /etc/hostname when the app starts, otherwise iDevice, and a set lasts
 * until the app ends. Only root may set it. */

static pthread_mutex_t gVprocHostnameMu = PTHREAD_MUTEX_INITIALIZER;
static char gVprocHostname[256];
static bool gVprocHostnameLoaded;

static void vprocHostnameLoadLocked(void) {
    if (gVprocHostnameLoaded) {
        return;
    }
    gVprocHostnameLoaded = true;
    strlcpy(gVprocHostname, "iDevice", sizeof(gVprocHostname));
    char path[PATH_MAX];
    const char *file = pscalEtcPath("hostname", path, sizeof(path));
    FILE *fp = file ? fopen(file, "r") : NULL;
    if (!fp) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t len = strcspn(p, " \t\r\n#");
        if (len > 0 && p[0] != '#') {
            p[len] = '\0';
            strlcpy(gVprocHostname, p, sizeof(gVprocHostname));
            break;
        }
    }
    fclose(fp);
}

int vprocGethostnameShim(char *name, size_t len) {
    if (!name) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&gVprocHostnameMu);
    vprocHostnameLoadLocked();
    size_t need = strlen(gVprocHostname);
    if (len == 0) {
        pthread_mutex_unlock(&gVprocHostnameMu);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, gVprocHostname, need < len ? need + 1 : len);
    pthread_mutex_unlock(&gVprocHostnameMu);
    if (need >= len) {
        name[len - 1] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int vprocSethostnameShim(const char *name, size_t len) {
    if (vprocGeteuidShim() != 0) {
        errno = EPERM;
        return -1;
    }
    if (!name || len >= sizeof(gVprocHostname)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVprocHostnameMu);
    gVprocHostnameLoaded = true;
    memcpy(gVprocHostname, name, len);
    gVprocHostname[len] = '\0';
    pthread_mutex_unlock(&gVprocHostnameMu);
    return 0;
}

int vprocUnameShim(struct utsname *buf) {
    if (uname(buf) != 0) {
        return -1;
    }
    char host[sizeof(buf->nodename)];
    if (vprocGethostnameShim(host, sizeof(host)) == 0) {
        strlcpy(buf->nodename, host, sizeof(buf->nodename));
    }
    return 0;
}

static uint64_t gVprocBootIdValue;
static pthread_once_t gVprocBootIdOnce = PTHREAD_ONCE_INIT;

static void vprocBootIdInit(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t value = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) ^
                     ((uint64_t)getpid() << 40);
    gVprocBootIdValue = value ? value : 1;
}

uint64_t vprocBootId(void) {
    pthread_once(&gVprocBootIdOnce, vprocBootIdInit);
    return gVprocBootIdValue;
}
#else
static bool vprocKillPermitted(pid_t pid) {
    (void)pid;
    return true;
}

int vprocHostPathPermit(const char *host_path, int op) {
    (void)host_path;
    (void)op;
    return 0;
}

int vprocHostPathPermitOpen(const char *host_path, int flags) {
    (void)host_path;
    (void)flags;
    return 0;
}

void vprocHostPathOwner(const char *host_path, struct stat *st) {
    (void)host_path;
    (void)st;
}

static int vprocChownRecord(const char *host_path, uid_t uid, gid_t gid, bool follow) {
    (void)follow;
    return vprocHostChownRaw(host_path, uid, gid);
}

uid_t vprocGeteuidShim(void) {
    return geteuid();
}
#endif /* PSCAL_TARGET_IOS */

#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */
