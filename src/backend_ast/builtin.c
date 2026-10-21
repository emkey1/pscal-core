#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "core/version.h"
#include "symbol/symbol.h"
#include "core/globals.h"                  // Assuming globals.h is directly in src/
#include "common/frontend_kind.h"
#include "common/runtime_tty.h"
#include "backend_ast/builtin_network_api.h"
#include "vm/vm.h"
#include "vm/string_sentinels.h"
#include "vm/vm_fx_policy.h"

// Standard library includes remain the same
#include <math.h>
#include <termios.h> // For tcgetattr, tcsetattr, etc. (Terminal I/O)
#include <poll.h>
#include <signal.h>  // For signal handling (SIGINT)
#include <unistd.h>  // For read, write, STDIN_FILENO, STDOUT_FILENO, isatty
#include <ctype.h>   // For isdigit
#include <errno.h>   // For errno
#include <stdatomic.h>
#include <sys/ioctl.h> // For ioctl, FIONREAD (Terminal I/O)
#include <stdint.h>  // For fixed-width integer types like uint8_t
#include <stdbool.h> // For bool, true, false (IMPORTANT - GCC needs this for 'bool')
#include <string.h>  // For strlen, strdup
#include <strings.h> // For strcasecmp
#include <dirent.h>  // For directory traversal
#include <sys/stat.h> // For file attributes
#include <stdlib.h>  // For system(), getenv, malloc
#include <stdarg.h>  // For va_list operations in JSON helpers
#include <limits.h>
#include <time.h>    // For date/time functions
#include <sys/time.h> // For gettimeofday
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <wordexp.h>
#include <stdio.h>   // For printf, fprintf
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <fcntl.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
bool shellRuntimeIsSharedFileStream(FILE *stream) {
    (void)stream;
    return false;
}

#if defined(PSCAL_TARGET_IOS)
#include "runtime/vproc/vproc.h"
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#undef PATH_VIRTUALIZATION_NO_MACROS
#if defined(__APPLE__)
extern VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) __attribute__((weak_import));
#else
extern VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) __attribute__((weak));
#endif
typedef struct {
    FILE *fp;
    int host_fd;
    int std_fd;
    dev_t host_dev;
    ino_t host_ino;
    bool host_identity_valid;
} VmVprocStream;

typedef struct {
    int std_fd;
    bool can_read;
    bool can_write;
} VmVprocShimCookie;

typedef struct VmShimCookieNode {
    void *cookie;
    struct VmShimCookieNode *next;
} VmShimCookieNode;

static __thread VmVprocStream gVmVprocStdout = { NULL, -1, STDOUT_FILENO };
static __thread VmVprocStream gVmVprocStderr = { NULL, -1, STDERR_FILENO };
static __thread VmVprocStream gVmVprocStdin = { NULL, -1, STDIN_FILENO };
static VmVprocStream gVmStdoutSharedShim = { NULL, -1, STDOUT_FILENO };
static VmVprocStream gVmStderrSharedShim = { NULL, -1, STDERR_FILENO };
static VmVprocStream gVmStdinSharedShim = { NULL, -1, STDIN_FILENO };
static pthread_mutex_t gVmSharedShimLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gVmShimCookieLock = PTHREAD_MUTEX_INITIALIZER;
static VmShimCookieNode *gVmShimCookieHead = NULL;

static void vmShimCookieTrackAdd(void *cookie) {
    if (!cookie) {
        return;
    }
    VmShimCookieNode *node = (VmShimCookieNode *)calloc(1, sizeof(VmShimCookieNode));
    if (!node) {
        return;
    }
    node->cookie = cookie;
    pthread_mutex_lock(&gVmShimCookieLock);
    node->next = gVmShimCookieHead;
    gVmShimCookieHead = node;
    pthread_mutex_unlock(&gVmShimCookieLock);
}

static bool vmShimCookieTrackRemove(void *cookie) {
    if (!cookie) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&gVmShimCookieLock);
    VmShimCookieNode *prev = NULL;
    VmShimCookieNode *cur = gVmShimCookieHead;
    while (cur) {
        if (cur->cookie == cookie) {
            if (prev) {
                prev->next = cur->next;
            } else {
                gVmShimCookieHead = cur->next;
            }
            free(cur);
            found = true;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&gVmShimCookieLock);
    return found;
}

static FILE *vmVprocStreamFallback(int std_fd) {
    if (std_fd == STDIN_FILENO) {
        return stdin;
    }
    if (std_fd == STDOUT_FILENO) {
        return stdout;
    }
    return stderr;
}

static int vmVprocShimRead(void *cookie, char *buf, int len) {
    VmVprocShimCookie *ctx = (VmVprocShimCookie *)cookie;
    if (!ctx || !ctx->can_read || !buf || len <= 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t res = vprocReadShim(ctx->std_fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        res = INT_MAX;
    }
    return (int)res;
}

static int vmVprocShimWrite(void *cookie, const char *buf, int len) {
    VmVprocShimCookie *ctx = (VmVprocShimCookie *)cookie;
    if (!ctx || !ctx->can_write || !buf || len <= 0) {
        errno = EBADF;
        return -1;
    }
    ssize_t res = vprocWriteShim(ctx->std_fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        res = INT_MAX;
    }
    return (int)res;
}

static int vmVprocShimClose(void *cookie) {
    if (!vmShimCookieTrackRemove(cookie)) {
        return 0;
    }
    free(cookie);
    return 0;
}

static FILE *vmVprocStreamOpenShim(int std_fd,
                                   VmVprocStream *cache,
                                   const char *mode,
                                   int buf_mode) {
    if (!cache || !mode) {
        return vmVprocStreamFallback(std_fd);
    }

    if (cache->fp && cache->host_fd < 0) {
        return cache->fp;
    }

    if (cache->fp) {
        fflush(cache->fp);
        fclose(cache->fp);
        cache->fp = NULL;
    }
    cache->host_fd = -1;
    cache->host_identity_valid = false;

    VmVprocShimCookie *cookie = (VmVprocShimCookie *)calloc(1, sizeof(VmVprocShimCookie));
    if (!cookie) {
        return vmVprocStreamFallback(std_fd);
    }
    cookie->std_fd = std_fd;
    cookie->can_read = strchr(mode, 'r') != NULL;
    cookie->can_write = (strchr(mode, 'w') != NULL) || (strchr(mode, 'a') != NULL);
    vmShimCookieTrackAdd(cookie);

    FILE *fp = funopen(cookie,
                       cookie->can_read ? vmVprocShimRead : NULL,
                       cookie->can_write ? vmVprocShimWrite : NULL,
                       NULL,
                       vmVprocShimClose);
    if (!fp) {
        (void)vmShimCookieTrackRemove(cookie);
        free(cookie);
        return vmVprocStreamFallback(std_fd);
    }
    if (buf_mode >= 0) {
        setvbuf(fp, NULL, buf_mode, 0);
    }
    cache->fp = fp;
    cache->host_fd = -1;
    cache->host_identity_valid = false;
    return fp;
}

static FILE *vmVprocStreamOpen(int std_fd,
                               VmVprocStream *cache,
                               const char *mode,
                               int buf_mode) {
    if (!cache || !mode) {
        return vmVprocStreamFallback(std_fd);
    }

    int host_fd = std_fd;
    bool use_host_stream = true;
    VProc *vp = vprocCurrent();
    if (vp) {
        int translated = vprocTranslateFd(vp, std_fd);
        if (translated >= 0) {
            host_fd = translated;
        } else {
            use_host_stream = false;
        }
    } else {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && !vprocSessionStdioIsDefault(session)) {
            use_host_stream = false;
        }
    }

    if (!use_host_stream) {
        return vmVprocStreamOpenShim(std_fd, cache, mode, buf_mode);
    }

    if (host_fd < 0) {
        return vmVprocStreamFallback(std_fd);
    }

    if (cache->fp && cache->host_fd == host_fd) {
        if (!cache->host_identity_valid) {
            return cache->fp;
        }
        struct stat current_st;
        if (fstat(host_fd, &current_st) == 0 &&
            current_st.st_dev == cache->host_dev &&
            current_st.st_ino == cache->host_ino) {
            return cache->fp;
        }
    }

    if (cache->fp) {
        fflush(cache->fp);
        fclose(cache->fp);
        cache->fp = NULL;
    }
    cache->host_fd = -1;
    cache->host_identity_valid = false;

    int dup_fd = -1;
#ifdef F_DUPFD_CLOEXEC
    dup_fd = fcntl(host_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0 && errno == EINVAL) {
        dup_fd = -1;
    }
#endif
    if (dup_fd < 0) {
        dup_fd = dup(host_fd);
    }
    if (dup_fd < 0) {
        return vmVprocStreamFallback(std_fd);
    }

    FILE *fp = fdopen(dup_fd, mode);
    if (!fp) {
        close(dup_fd);
        return vmVprocStreamFallback(std_fd);
    }
    if (buf_mode >= 0) {
        setvbuf(fp, NULL, buf_mode, 0);
    }
    cache->fp = fp;
    cache->host_fd = host_fd;
    struct stat host_st;
    if (fstat(host_fd, &host_st) == 0) {
        cache->host_dev = host_st.st_dev;
        cache->host_ino = host_st.st_ino;
        cache->host_identity_valid = true;
    }
    return fp;
}

static FILE *vmVprocStdout(void) {
    int buf_mode = pscalRuntimeStdoutIsInteractive() ? _IOLBF : _IOFBF;
    return vmVprocStreamOpen(STDOUT_FILENO, &gVmVprocStdout, "w", buf_mode);
}

static FILE *vmVprocStderr(void) {
    return vmVprocStreamOpen(STDERR_FILENO, &gVmVprocStderr, "w", _IONBF);
}

static FILE *vmVprocStdin(void) {
    return vmVprocStreamOpen(STDIN_FILENO, &gVmVprocStdin, "r", -1);
}

static bool vmFdMatchesFileIdentity(int fd, int other_fd) {
    if (fd < 0 || other_fd < 0) {
        return false;
    }
    struct stat fd_st;
    struct stat other_st;
    if (fstat(fd, &fd_st) != 0 || fstat(other_fd, &other_st) != 0) {
        return false;
    }
    return fd_st.st_dev == other_st.st_dev && fd_st.st_ino == other_st.st_ino;
}

static bool vmStreamTargetsSharedStdin(FILE *stream) {
    if (!stream) {
        return false;
    }
    if (stream == stdin) {
        return true;
    }
    if (pscalRuntimeVmIsSharedFileStream(stream)) {
        return true;
    }
    int fd = fileno(stream);
    if (fd < 0) {
        return false;
    }
    if (vmFdMatchesFileIdentity(fd, STDIN_FILENO)) {
        return true;
    }
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocCurrent();
    if (vp) {
        int translated = vprocTranslateFd(vp, STDIN_FILENO);
        if (translated >= 0 && vmFdMatchesFileIdentity(fd, translated)) {
            return true;
        }
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && !vprocSessionStdioIsDefault(session) &&
        session->stdin_host_fd >= 0 &&
        vmFdMatchesFileIdentity(fd, session->stdin_host_fd)) {
        return true;
    }
#endif
    return false;
}

static FILE *vmVprocSharedShimStream(int std_fd,
                                     VmVprocStream *cache,
                                     const char *mode,
                                     int buf_mode) {
    FILE *fp = NULL;
    pthread_mutex_lock(&gVmSharedShimLock);
    fp = vmVprocStreamOpenShim(std_fd, cache, mode, buf_mode);
    pthread_mutex_unlock(&gVmSharedShimLock);
    return fp;
}

FILE* pscalRuntimeVmStdin(void) {
    return vmVprocSharedShimStream(STDIN_FILENO, &gVmStdinSharedShim, "r", -1);
}

FILE* pscalRuntimeVmStdout(void) {
    return vmVprocSharedShimStream(STDOUT_FILENO, &gVmStdoutSharedShim, "w", _IONBF);
}

FILE* pscalRuntimeVmStderr(void) {
    return vmVprocSharedShimStream(STDERR_FILENO, &gVmStderrSharedShim, "w", _IONBF);
}

bool pscalRuntimeVmIsSharedFileStream(FILE* stream) {
    if (!stream) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (stream == stdin || stream == stdout || stream == stderr) {
        return true;
    }
    if (stream == gVmVprocStdin.fp || stream == gVmVprocStdout.fp || stream == gVmVprocStderr.fp) {
        return true;
    }
    if (stream == gVmStdinSharedShim.fp || stream == gVmStdoutSharedShim.fp || stream == gVmStderrSharedShim.fp) {
        return true;
    }
    if (shellRuntimeIsSharedFileStream && shellRuntimeIsSharedFileStream(stream)) {
        return true;
    }
    return false;
#else
    if (shellRuntimeIsSharedFileStream && shellRuntimeIsSharedFileStream(stream)) {
        return true;
    }
    return stream == stdin || stream == stdout || stream == stderr;
#endif
}

#undef stdin
#undef stdout
#undef stderr
#define stdin vmVprocStdin()
#define stdout vmVprocStdout()
#define stderr vmVprocStderr()
#endif

#if !defined(PSCAL_TARGET_IOS)
static bool vmFdMatchesFileIdentity(int fd, int other_fd) {
    if (fd < 0 || other_fd < 0) {
        return false;
    }
    struct stat fd_st;
    struct stat other_st;
    if (fstat(fd, &fd_st) != 0 || fstat(other_fd, &other_st) != 0) {
        return false;
    }
    return fd_st.st_dev == other_st.st_dev && fd_st.st_ino == other_st.st_ino;
}

static bool vmStreamTargetsSharedStdin(FILE *stream) {
    if (!stream) {
        return false;
    }
    if (stream == stdin) {
        return true;
    }
    if (pscalRuntimeVmIsSharedFileStream(stream)) {
        return true;
    }
    int fd = fileno(stream);
    if (fd < 0) {
        return false;
    }
    if (vmFdMatchesFileIdentity(fd, STDIN_FILENO)) {
        return true;
    }
    return false;
}

FILE* pscalRuntimeVmStdin(void) {
    return stdin;
}

FILE* pscalRuntimeVmStdout(void) {
    return stdout;
}

FILE* pscalRuntimeVmStderr(void) {
    return stderr;
}

bool pscalRuntimeVmIsSharedFileStream(FILE* stream) {
    if (!stream) {
        return false;
    }
    if (shellRuntimeIsSharedFileStream && shellRuntimeIsSharedFileStream(stream)) {
        return true;
    }
    return stream == stdin || stream == stdout || stream == stderr;
}
#endif

static void vmMaybeCloseFileValue(Value *file_value) {
    if (!file_value || VALUE_TYPE(*file_value) != TYPE_FILE || !AS_FILE(*file_value)) {
        return;
    }
    FILE *fp = AS_FILE(*file_value);
    if (pscalRuntimeVmIsSharedFileStream(fp)) {
        fflush(fp);
    } else {
        fclose(fp);
    }
    AS_FILE(*file_value) = NULL;
}

#if defined(__APPLE__)
extern void shellRuntimeSetLastStatus(int status) __attribute__((weak_import));
extern void shellRuntimeSetLastStatusSticky(int status) __attribute__((weak_import));
#elif defined(__GNUC__)
extern void shellRuntimeSetLastStatus(int status) __attribute__((weak));
extern void shellRuntimeSetLastStatusSticky(int status) __attribute__((weak));
#else
void shellRuntimeSetLastStatus(int status);
void shellRuntimeSetLastStatusSticky(int status);
#endif

// Maximum number of arguments allowed for write/writeln
#define MAX_WRITE_ARGS_VM 32

/* SDL-backed builtins are registered dynamically when available. */
#ifdef SDL
#include "backend_ast/pscal_sdl_runtime.h"
#define SDL_READKEY_BUFFER_CAPACITY 8
static int gSdlReadKeyBuffer[SDL_READKEY_BUFFER_CAPACITY];
static int gSdlReadKeyBufferStart = 0;
static int gSdlReadKeyBufferCount = 0;

#endif

static bool vmShouldBypassTerminalControl(void) {
    return false;
}

static const Value* resolveStringPointerBuiltin(const Value* value) {
    const Value* current = value;
    int depth = 0;
    while (current && VALUE_TYPE(*current) == TYPE_POINTER &&
           PTR_BASE_TYPE_NODE(*current) != STRING_CHAR_PTR_SENTINEL &&
           PTR_BASE_TYPE_NODE(*current) != SERIALIZED_CHAR_PTR_SENTINEL &&
           PTR_BASE_TYPE_NODE(*current) != STRING_LENGTH_SENTINEL &&
           PTR_BASE_TYPE_NODE(*current) != BYTE_ARRAY_PTR_SENTINEL &&
           PTR_BASE_TYPE_NODE(*current) != SHELL_FUNCTION_PTR_SENTINEL &&
           PTR_BASE_TYPE_NODE(*current) != OPAQUE_POINTER_SENTINEL) {
        if (!AS_POINTER(*current)) {
            return NULL;
        }
        current = (const Value*)AS_POINTER(*current);
        if (++depth > 16) {
            return NULL;
        }
    }
    return current;
}

static int builtinValueIsStringLike(const Value* value) {
    if (!value) return 0;
    if (isPascalStringType(VALUE_TYPE(*value))) return 1;
    if (VALUE_TYPE(*value) == TYPE_POINTER) {
        if (PTR_BASE_TYPE_NODE(*value) == STRING_CHAR_PTR_SENTINEL ||
            PTR_BASE_TYPE_NODE(*value) == SERIALIZED_CHAR_PTR_SENTINEL) return 1;
        const Value* resolved = resolveStringPointerBuiltin(value);
        if (!resolved) return 0;
        if (isPascalStringType(VALUE_TYPE(*resolved))) return 1;
        if (VALUE_TYPE(*resolved) == TYPE_POINTER &&
            (PTR_BASE_TYPE_NODE(*resolved) == STRING_CHAR_PTR_SENTINEL ||
             PTR_BASE_TYPE_NODE(*resolved) == SERIALIZED_CHAR_PTR_SENTINEL)) {
            return 1;
        }
    }
    return 0;
}

static const char* builtinValueToCString(const Value* value) {
    if (!value) return NULL;
    if (isPascalStringType(VALUE_TYPE(*value))) return AS_STRING(*value) ? AS_STRING(*value) : "";
    if (VALUE_TYPE(*value) == TYPE_POINTER) {
        if (PTR_BASE_TYPE_NODE(*value) == STRING_CHAR_PTR_SENTINEL ||
            PTR_BASE_TYPE_NODE(*value) == SERIALIZED_CHAR_PTR_SENTINEL) {
            return (const char*)AS_POINTER(*value);
        }
        const Value* resolved = resolveStringPointerBuiltin(value);
        if (!resolved) return NULL;
        if (isPascalStringType(VALUE_TYPE(*resolved))) {
            return AS_STRING(*resolved) ? AS_STRING(*resolved) : "";
        }
        if (VALUE_TYPE(*resolved) == TYPE_POINTER &&
            (PTR_BASE_TYPE_NODE(*resolved) == STRING_CHAR_PTR_SENTINEL ||
             PTR_BASE_TYPE_NODE(*resolved) == SERIALIZED_CHAR_PTR_SENTINEL)) {
            return (const char*)AS_POINTER(*resolved);
        }
    }
    return NULL;
}

static bool valueIsByteCompatible(const Value* value) {
    if (!value) {
        return false;
    }
    switch (VALUE_TYPE(*value)) {
        case TYPE_BYTE:
        case TYPE_UINT8:
        case TYPE_INT8:
        case TYPE_CHAR:
        case TYPE_BOOLEAN:
            return true;
        default:
            return false;
    }
}

static unsigned char valueToByte(const Value* value) {
    if (!value) {
        return 0;
    }
    switch (VALUE_TYPE(*value)) {
        case TYPE_CHAR:
            return (unsigned char)AS_CHAR(*value);
        case TYPE_BOOLEAN:
            return VAL_INT(*value) ? 1u : 0u;
        default:
            if (IS_INTLIKE(*value)) {
                return (unsigned char)AS_INTEGER(*value);
            }
            return 0u;
    }
}

static bool writeBinaryElement(FILE* stream, const Value* rawValue, VarType elementType,
                               size_t elementSize, int* outErrno) {
    if (!stream || !rawValue) {
        return false;
    }

    const Value* value = rawValue;
    if (VALUE_TYPE(*value) == TYPE_POINTER && AS_POINTER(*value)) {
        value = (const Value*)AS_POINTER(*value);
    }
    // An enum's AS_INTEGER is 0; what goes in the file is its ordinal
    // (`file of Color` used to come out all zero bytes).
    Value ordinal_value;
    if (VALUE_TYPE(*value) == TYPE_ENUM) {
        ordinal_value = makeInt(AS_ENUM(*value).ordinal);
        value = &ordinal_value;
    }

    unsigned char buffer[sizeof(long double)] = {0};
    size_t bytes = 0;

    switch (elementType) {
        case TYPE_CHAR:
        case TYPE_BOOLEAN:
        case TYPE_BYTE:
        case TYPE_UINT8:
        case TYPE_INT8:
            buffer[0] = valueToByte(value);
            bytes = 1;
            break;
        case TYPE_INT16: {
            int16_t v = (int16_t)(IS_INTLIKE(*value) ? AS_INTEGER(*value)
                                 : (isRealType(VALUE_TYPE(*value)) ? (long long)AS_REAL(*value) : 0));
            memcpy(buffer, &v, sizeof(v));
            bytes = sizeof(v);
            break;
        }
        case TYPE_UINT16:
        case TYPE_WORD: {
            uint16_t v = (uint16_t)(IS_INTLIKE(*value) ? AS_INTEGER(*value)
                                   : (isRealType(VALUE_TYPE(*value)) ? (long long)AS_REAL(*value) : 0));
            memcpy(buffer, &v, sizeof(v));
            bytes = sizeof(v);
            break;
        }
        case TYPE_INT32: {
            int32_t v = (int32_t)(IS_INTLIKE(*value) ? AS_INTEGER(*value)
                                 : (isRealType(VALUE_TYPE(*value)) ? (long long)AS_REAL(*value) : 0));
            memcpy(buffer, &v, sizeof(v));
            bytes = sizeof(v);
            break;
        }
        case TYPE_UINT32:
        case TYPE_ENUM: {
            uint32_t v = (uint32_t)(IS_INTLIKE(*value) ? AS_INTEGER(*value)
                                     : (isRealType(VALUE_TYPE(*value)) ? (long long)AS_REAL(*value) : 0));
            memcpy(buffer, &v, sizeof(v));
            bytes = sizeof(v);
            break;
        }
        case TYPE_INT64: {
            int64_t v = (int64_t)(IS_INTLIKE(*value) ? AS_INTEGER(*value)
                                 : (isRealType(VALUE_TYPE(*value)) ? (long long)AS_REAL(*value) : 0));
            memcpy(buffer, &v, sizeof(v));
            bytes = sizeof(v);
            break;
        }
        case TYPE_UINT64: {
            uint64_t v = (uint64_t)(IS_INTLIKE(*value) ? AS_INTEGER(*value)
                                     : (isRealType(VALUE_TYPE(*value)) ? (unsigned long long)AS_REAL(*value) : 0u));
            memcpy(buffer, &v, sizeof(v));
            bytes = sizeof(v);
            break;
        }
        case TYPE_FLOAT: {
            float f = isRealType(VALUE_TYPE(*value)) ? (float)AS_REAL(*value)
                      : (IS_INTLIKE(*value) ? (float)AS_INTEGER(*value) : 0.0f);
            memcpy(buffer, &f, sizeof(f));
            bytes = sizeof(f);
            break;
        }
        case TYPE_DOUBLE: {
            double d = isRealType(VALUE_TYPE(*value)) ? (double)AS_REAL(*value)
                        : (IS_INTLIKE(*value) ? (double)AS_INTEGER(*value) : 0.0);
            memcpy(buffer, &d, sizeof(d));
            bytes = sizeof(d);
            break;
        }
        case TYPE_LONG_DOUBLE: {
            long double ld = isRealType(VALUE_TYPE(*value)) ? AS_REAL(*value)
                                 : (IS_INTLIKE(*value) ? (long double)AS_INTEGER(*value) : 0.0L);
            memcpy(buffer, &ld, sizeof(ld));
            bytes = sizeof(ld);
            break;
        }
        default:
            return false;
    }

    if (elementSize > 0 && elementSize != bytes) {
        if (elementSize > sizeof(buffer)) {
            return false;
        }
        if (elementSize > bytes) {
            memset(buffer + bytes, 0, elementSize - bytes);
        }
        bytes = elementSize;
    }

    errno = 0;
    size_t written = fwrite(buffer, 1, bytes, stream);
    if (written != bytes) {
        if (outErrno) {
            *outErrno = errno ? errno : 1;
        }
        return false;
    }
    if (outErrno) {
        *outErrno = 0;
    }
    return true;
}

/* The reading half of writeBinaryElement: one element of a typed file
 * (`file of Integer`, `file of Color`, ...) in the same fixed-size native
 * layout. Read used to parse these files as text and came back with 0 for
 * every element. A short read (end of file) reports IOResult 100, as in
 * Turbo/Free Pascal. */
static bool builtinSetEnumOrdinal(Value* dst, long long ordinal);

static bool readBinaryElement(FILE* stream, Value* dst, VarType elementType,
                              size_t elementSize, int* outErrno) {
    if (outErrno) *outErrno = 0;
    if (!stream || !dst || elementSize == 0 || elementSize > sizeof(long double)) {
        if (outErrno) *outErrno = 1;
        return false;
    }
    unsigned char buffer[sizeof(long double)] = {0};
    errno = 0;
    size_t got = fread(buffer, 1, elementSize, stream);
    if (got != elementSize) {
        if (outErrno) *outErrno = ferror(stream) ? (errno ? errno : 1) : 100;
        return false;
    }

    bool is_real = false;
    long long ival = 0;
    long double rval = 0.0L;
    switch (elementType) {
        case TYPE_INT8:   { int8_t v;   memcpy(&v, buffer, sizeof(v)); ival = v; break; }
        case TYPE_INT16:  { int16_t v;  memcpy(&v, buffer, sizeof(v)); ival = v; break; }
        case TYPE_INT32:  { int32_t v;  memcpy(&v, buffer, sizeof(v)); ival = v; break; }
        case TYPE_INT64:  { int64_t v;  memcpy(&v, buffer, sizeof(v)); ival = v; break; }
        case TYPE_CHAR:
        case TYPE_BOOLEAN:
        case TYPE_BYTE:
        case TYPE_UINT8:  ival = buffer[0]; break;
        case TYPE_UINT16:
        case TYPE_WORD:   { uint16_t v; memcpy(&v, buffer, sizeof(v)); ival = v; break; }
        case TYPE_UINT32:
        case TYPE_ENUM:   { uint32_t v; memcpy(&v, buffer, sizeof(v)); ival = v; break; }
        case TYPE_UINT64: { uint64_t v; memcpy(&v, buffer, sizeof(v)); ival = (long long)v; break; }
        case TYPE_FLOAT:  { float f;    memcpy(&f, buffer, sizeof(f)); rval = f; is_real = true; break; }
        case TYPE_DOUBLE: { double d;   memcpy(&d, buffer, sizeof(d)); rval = d; is_real = true; break; }
        case TYPE_LONG_DOUBLE: { long double ld; memcpy(&ld, buffer, sizeof(ld)); rval = ld; is_real = true; break; }
        default:
            if (outErrno) *outErrno = 1;
            return false;
    }

    VarType dst_type = VALUE_TYPE(*dst);
    if (dst_type == TYPE_ENUM) {
        if (is_real || !builtinSetEnumOrdinal(dst, ival)) {
            if (outErrno) *outErrno = 1;
            return false;
        }
        return true;
    }
    if (isRealType(dst_type)) {
        SET_REAL_VALUE(dst, is_real ? rval : (long double)ival);
        return true;
    }
    if (is_real) {
        if (outErrno) *outErrno = 1;
        return false;
    }
    if (dst_type == TYPE_CHAR) {
        SET_CHAR_VALUE(dst, (unsigned char)ival);
        SET_INT_VALUE(dst, AS_CHAR(*dst));
        return true;
    }
    if (dst_type == TYPE_BOOLEAN) {
        SET_INT_VALUE(dst, ival != 0 ? 1 : 0);
        return true;
    }
    if (IS_INTLIKE(*dst)) {
        SET_INT_VALUE(dst, ival);
        return true;
    }
    if (outErrno) *outErrno = 1;
    return false;
}

static bool readToken(FILE* stream, char* buffer, size_t bufferSize) {
    if (!stream || !buffer || bufferSize == 0) {
        return false;
    }
    return fscanf(stream, "%1023s", buffer) == 1;
}

/* Store freshly read text in a string variable by rebinding it to a new
 * StringObj, never by rewriting the buffer of the one it holds: string
 * Values share their StringObj (copy-on-write), so a rewrite in place
 * changed every other variable holding the same text. This Value's
 * reference to the previous object is released -- not leaked, and not
 * freed out from under any other holder. A string[N] target keeps its N:
 * the text is cut to N bytes and the buffer allocated at N + 1, the size
 * fixed-length assignment later copies into. Takes `len` bytes of `text`. */
static bool builtinRebindStringValue(Value* dst, const char* text, size_t len) {
    if (!dst) {
        return false;
    }
    VarType type = (VALUE_TYPE(*dst) == TYPE_UNICODE_STRING) ? TYPE_UNICODE_STRING : TYPE_STRING;
    StringObj* old = isPascalStringType(VALUE_TYPE(*dst)) ? PSCAL_VALUE_PTR(*dst, StringObj) : NULL;
    int max_length = old ? old->max_length : -1;
    if (max_length > 0 && len > (size_t)max_length) {
        len = (size_t)max_length;
    }
    size_t alloc_len = (max_length > 0) ? (size_t)max_length + 1 : len + 1;
    char* buffer = (char*)malloc(alloc_len);
    if (!buffer) {
        return false;
    }
    if (len > 0) {
        memcpy(buffer, text, len);
    }
    buffer[len] = '\0';
    StringObj* fresh = pscalStringObjCreate(max_length, type);
    fresh->buffer = buffer;
    pthread_mutex_lock(&value_cell_mutex);
    PSCAL_VALUE_FIELD(*dst, type) = type;
    pscalValueSetHeapPtrBits(dst, fresh);
    pthread_mutex_unlock(&value_cell_mutex);
    if (old) {
        pscalObjRelease(&old->header);
    }
    return true;
}

/* Set an enum variable's ordinal, range-checked against its type. The
 * variable gets a fresh EnumObj rather than having the shared one edited. */
static bool builtinSetEnumOrdinal(Value* dst, long long ordinal) {
    if (!dst || VALUE_TYPE(*dst) != TYPE_ENUM) {
        return false;
    }
    if (ordinal < 0 || ordinal > INT_MAX ||
        (ENUM_META(*dst) && ordinal >= ENUM_META(*dst)->member_count)) {
        return false;
    }
    if (AS_ENUM(*dst).ordinal == (int)ordinal) {
        return true;
    }
    Value fresh = makeEnum(AS_ENUM(*dst).enum_name, (int)ordinal);
    ENUM_META(fresh) = ENUM_META(*dst);
    ENUM_TYPE_DEF(fresh) = ENUM_TYPE_DEF(*dst);
    Value old = *dst;
    pthread_mutex_lock(&value_cell_mutex);
    *dst = fresh;
    pthread_mutex_unlock(&value_cell_mutex);
    freeValue(&old);
    return true;
}

/* Records and arrays written to (and read back from) a non-binary file go
 * through the two functions below as a sequence of space-separated tokens.
 *
 * Strings are written quoted, Pascal-literal style: 'Ada Lovelace', with an
 * embedded quote doubled ('it''s') and the empty string as ''. Written raw,
 * a string with a space came back as its first word and every later field
 * shifted (an empty string shifted them all). The quoted form reads back
 * whatever was written, including spaces and line breaks, and still reads
 * the old unquoted form, so files written before this change load as they
 * did.
 *
 * Pointer fields are refused: the old code followed them, so writing one
 * node of a cyclic list recursed until the stack overflowed, and a nil
 * pointer field failed anyway. Only the outermost value (a VAR argument)
 * is dereferenced. */
static bool writeStructuredValueInner(FILE* stream, const Value* value);
static bool readStructuredValueInner(FILE* stream, Value* value);

static bool writeQuotedString(FILE* stream, const char* text) {
    if (fputc('\'', stream) == EOF) return false;
    for (const char* c = text; *c; ++c) {
        if (*c == '\'' && fputc('\'', stream) == EOF) return false;
        if (fputc((unsigned char)*c, stream) == EOF) return false;
    }
    return fputs("' ", stream) != EOF;
}

static bool readStructuredString(FILE* stream, Value* value) {
    int ch;
    do {
        ch = fgetc(stream);
    } while (ch != EOF && isspace(ch));
    if (ch == EOF) {
        return false;
    }
    size_t len = 0, cap = 64;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        return false;
    }
    bool quoted = (ch == '\'');
    if (quoted) {
        for (;;) {
            ch = fgetc(stream);
            if (ch == EOF) {
                free(buf);
                return false; // unterminated
            }
            if (ch == '\'') {
                int next = fgetc(stream);
                if (next != '\'') {
                    if (next != EOF) ungetc(next, stream);
                    break;
                }
            }
            if (len + 1 >= cap) {
                char* grown = (char*)realloc(buf, cap * 2);
                if (!grown) { free(buf); return false; }
                buf = grown;
                cap *= 2;
            }
            buf[len++] = (char)ch;
        }
    } else {
        // Legacy unquoted token: up to the next whitespace.
        while (ch != EOF && !isspace(ch)) {
            if (len + 1 >= cap) {
                char* grown = (char*)realloc(buf, cap * 2);
                if (!grown) { free(buf); return false; }
                buf = grown;
                cap *= 2;
            }
            buf[len++] = (char)ch;
            ch = fgetc(stream);
        }
        if (ch != EOF) ungetc(ch, stream);
    }
    bool ok = builtinRebindStringValue(value, buf, len);
    free(buf);
    return ok;
}

static bool writeStructuredValue(FILE* stream, const Value* rawValue) {
    if (!stream || !rawValue) {
        return false;
    }

    const Value* value = rawValue;
    if (VALUE_TYPE(*value) == TYPE_POINTER && AS_POINTER(*value)) {
        value = (const Value*)AS_POINTER(*value);
    }
    return writeStructuredValueInner(stream, value);
}

static bool writeStructuredValueInner(FILE* stream, const Value* value) {
    switch (VALUE_TYPE(*value)) {
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            return fprintf(stream, "%lld ", (long long)AS_INTEGER(*value)) > 0;
        case TYPE_ENUM:
            // AS_INTEGER is 0 for an enum; its value is the ordinal.
            return fprintf(stream, "%d ", AS_ENUM(*value).ordinal) > 0;
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return fprintf(stream, "%.17Lg ", (long double)AS_REAL(*value)) > 0;
        case TYPE_CHAR:
            return fprintf(stream, "%u ", (unsigned int)(unsigned char)AS_CHAR(*value)) > 0;
        case TYPE_STRING:
            return writeQuotedString(stream, AS_STRING(*value) ? AS_STRING(*value) : "");
        case TYPE_ARRAY: {
            int total = calculateArrayTotalSize(value);
            // arrayUsesPackedBytes must be checked before AS_ARRAY(*value):
            // raw/elements are a union inside ArrayObj (VM 2.0 Phase 4e/4f),
            // so AS_ARRAY(*value) is non-NULL for a packed array too (same
            // bits as .raw) -- a plain truthiness check can no longer tell
            // packed and non-packed storage apart.
            if (total < 0 || arrayUsesPackedBytes(value) || !AS_ARRAY(*value)) {
                return false;
            }
            for (int i = 0; i < total; ++i) {
                if (!writeStructuredValueInner(stream, &AS_ARRAY(*value)[i])) {
                    return false;
                }
            }
            return true;
        }
        case TYPE_RECORD: {
            const Value* seen_storage[128];
            int seen_count = 0;
            for (FieldValue* field = AS_RECORD(*value); field; field = field->next) {
                const Value* fieldValue = fieldValueStorageConst(field);
                bool already_seen = false;
                for (int i = 0; i < seen_count; ++i) {
                    if (seen_storage[i] == fieldValue) {
                        already_seen = true;
                        break;
                    }
                }
                if (already_seen) {
                    continue;
                }
                if (seen_count < (int)(sizeof(seen_storage) / sizeof(seen_storage[0]))) {
                    seen_storage[seen_count++] = fieldValue;
                }
                if (!writeStructuredValueInner(stream, fieldValue)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

static bool readStructuredValue(FILE* stream, Value* rawValue) {
    if (!stream || !rawValue) {
        return false;
    }

    Value* value = rawValue;
    if (VALUE_TYPE(*value) == TYPE_POINTER && AS_POINTER(*value)) {
        value = (Value*)AS_POINTER(*value);
    }
    return readStructuredValueInner(stream, value);
}

static bool readStructuredValueInner(FILE* stream, Value* value) {
    char token[1024];
    // Strings, records and static arrays are filled in place (recursively,
    // field by field), so unshare each level before writing into it: copies
    // made by assignment or a by-value parameter share these payloads by
    // reference count and would otherwise change too.
    valueEnsureUnique(value);
    switch (VALUE_TYPE(*value)) {
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_ENUM: {
            if (!readToken(stream, token, sizeof(token))) return false;
            errno = 0;
            char* endp = NULL;
            long long parsed = strtoll(token, &endp, 10);
            if (endp == token || errno == ERANGE) return false;
            if (VALUE_TYPE(*value) == TYPE_ENUM) {
                // SET_INT_VALUE is a no-op on an enum.
                return builtinSetEnumOrdinal(value, parsed);
            }
            SET_INT_VALUE(value, parsed);
            return true;
        }
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE: {
            if (!readToken(stream, token, sizeof(token))) return false;
            errno = 0;
            char* endp = NULL;
            long double parsed = strtold(token, &endp);
            if (endp == token || errno == ERANGE) return false;
            SET_REAL_VALUE(value, parsed);
            return true;
        }
        case TYPE_CHAR: {
            if (!readToken(stream, token, sizeof(token))) return false;
            errno = 0;
            char* endp = NULL;
            long long parsed = strtoll(token, &endp, 10);
            if (endp != token && errno != ERANGE) {
                SET_CHAR_VALUE(value, (unsigned char)parsed);
                SET_INT_VALUE(value, AS_CHAR(*value));
                return true;
            }
            SET_CHAR_VALUE(value, (unsigned char)token[0]);
            SET_INT_VALUE(value, AS_CHAR(*value));
            return true;
        }
        case TYPE_STRING:
            return readStructuredString(stream, value);
        case TYPE_ARRAY: {
            int total = calculateArrayTotalSize(value);
            // See writeStructuredValue's matching comment: arrayUsesPackedBytes
            // must be checked before AS_ARRAY(*value) now that raw/elements
            // are a union inside ArrayObj.
            if (total < 0 || arrayUsesPackedBytes(value) || !AS_ARRAY(*value)) {
                return false;
            }
            for (int i = 0; i < total; ++i) {
                if (!readStructuredValueInner(stream, &AS_ARRAY(*value)[i])) {
                    return false;
                }
            }
            return true;
        }
        case TYPE_RECORD: {
            Value* seen_storage[128];
            int seen_count = 0;
            for (FieldValue* field = AS_RECORD(*value); field; field = field->next) {
                Value* fieldValue = fieldValueStorage(field);
                bool already_seen = false;
                for (int i = 0; i < seen_count; ++i) {
                    if (seen_storage[i] == fieldValue) {
                        already_seen = true;
                        break;
                    }
                }
                if (already_seen) {
                    continue;
                }
                if (seen_count < (int)(sizeof(seen_storage) / sizeof(seen_storage[0]))) {
                    seen_storage[seen_count++] = fieldValue;
                }
                if (!readStructuredValueInner(stream, fieldValue)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

static void assignByteToValue(Value* target, unsigned char byte) {
    if (!target) {
        return;
    }
    switch (VALUE_TYPE(*target)) {
        case TYPE_CHAR:
            SET_CHAR_VALUE(target, (unsigned char)byte);
            SET_INT_VALUE(target, AS_CHAR(*target));
            break;
        case TYPE_BOOLEAN:
            SET_INT_VALUE(target, byte ? 1 : 0);
            break;
        default:
            SET_INT_VALUE(target, byte);
            break;
    }
}

static void assignCountToResult(Value* slot, long long count) {
    if (!slot) {
        return;
    }
    if (VALUE_TYPE(*slot) == TYPE_POINTER && AS_POINTER(*slot)) {
        assignCountToResult((Value*)AS_POINTER(*slot), count);
        return;
    }
    if (isRealType(VALUE_TYPE(*slot))) {
        SET_REAL_VALUE(slot, (long double)count);
        return;
    }
    if (VALUE_TYPE(*slot) == TYPE_CHAR) {
        SET_CHAR_VALUE(slot, (unsigned char)count);
        SET_INT_VALUE(slot, AS_CHAR(*slot));
        return;
    }
    if (VALUE_TYPE(*slot) == TYPE_BOOLEAN) {
        SET_INT_VALUE(slot, count != 0 ? 1 : 0);
        return;
    }
    SET_INT_VALUE(slot, count);
    if (VALUE_TYPE(*slot) == TYPE_VOID || VALUE_TYPE(*slot) == TYPE_UNKNOWN || VALUE_TYPE(*slot) == TYPE_NIL) {
        SET_VALUE_TYPE(slot, TYPE_INT32);
    }
}

static bool builtinSizeForVarType(VarType type, long long *out_bytes) {
    if (!out_bytes) return false;
    switch (type) {
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
            *out_bytes = 1;
            return true;
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_WORD:
            *out_bytes = 2;
            return true;
        case TYPE_INT32:
        case TYPE_UINT32:
            *out_bytes = 4;
            return true;
        case TYPE_INT64:
        case TYPE_UINT64:
            *out_bytes = 8;
            return true;
        case TYPE_FLOAT:
            *out_bytes = (long long)sizeof(float);
            return true;
        case TYPE_DOUBLE:
            *out_bytes = (long long)sizeof(double);
            return true;
        case TYPE_LONG_DOUBLE:
            *out_bytes = (long long)sizeof(long double);
            return true;
        case TYPE_POINTER:
        case TYPE_FILE:
        case TYPE_MEMORYSTREAM:
        case TYPE_INTERFACE:
        case TYPE_CLOSURE:
        case TYPE_THREAD:
        case TYPE_TASK:
        case TYPE_CHANNEL:
            *out_bytes = (long long)sizeof(void*);
            return true;
        case TYPE_ENUM:
            *out_bytes = (long long)sizeof(int);
            return true;
        case TYPE_SET:
        case TYPE_ARRAY:
        case TYPE_RECORD:
        case TYPE_STRING:
        case TYPE_UNKNOWN:
        case TYPE_VOID:
        case TYPE_NIL:
        default:
            return false;
    }
}

static bool computeValueSizeBytesInternal(const Value *value, long long *out_bytes, int depth);

static bool computeValueSizeBytes(const Value *value, long long *out_bytes) {
    if (!value || !out_bytes) {
        return false;
    }
    return computeValueSizeBytesInternal(value, out_bytes, 0);
}

static bool computeSizeFromTypeName(const char *type_name, long long *out_bytes) {
    if (!type_name || !*type_name || !out_bytes) {
        return false;
    }

    if (strcasecmp(type_name, "integer") == 0 || strcasecmp(type_name, "longint") == 0) {
        return builtinSizeForVarType(TYPE_INT32, out_bytes);
    }
    if (strcasecmp(type_name, "real") == 0) {
        return builtinSizeForVarType(TYPE_DOUBLE, out_bytes);
    }
    if (strcasecmp(type_name, "float") == 0) {
        return builtinSizeForVarType(TYPE_FLOAT, out_bytes);
    }
    if (strcasecmp(type_name, "char") == 0) {
        return builtinSizeForVarType(TYPE_CHAR, out_bytes);
    }
    if (strcasecmp(type_name, "boolean") == 0) {
        return builtinSizeForVarType(TYPE_BOOLEAN, out_bytes);
    }
    if (strcasecmp(type_name, "byte") == 0) {
        return builtinSizeForVarType(TYPE_BYTE, out_bytes);
    }
    if (strcasecmp(type_name, "word") == 0) {
        return builtinSizeForVarType(TYPE_WORD, out_bytes);
    }

    AST *type_def = lookupType(type_name);
    if (!type_def) {
        return false;
    }

    AST *resolved = type_def;
    if (resolved->type == AST_TYPE_REFERENCE && resolved->right) {
        resolved = resolved->right;
    }

    VarType vt = resolved->var_type;
    if (vt == TYPE_VOID || vt == TYPE_UNKNOWN) {
        if (resolved->right) {
            vt = resolved->right->var_type;
        }
    }

    Value temp = makeValueForType(vt, resolved, NULL);
    bool ok = computeValueSizeBytes(&temp, out_bytes);
    freeValue(&temp);
    return ok;
}

static bool computeValueSizeBytesInternal(const Value *value, long long *out_bytes, int depth) {
    if (!value || !out_bytes || depth > 16) {
        return false;
    }

    switch (VALUE_TYPE(*value)) {
        case TYPE_POINTER:
            /* Pascal SizeOf should treat any pointer value as pointer-sized. */
            *out_bytes = (long long)sizeof(void*);
            return true;
        case TYPE_STRING:
            if (STRING_MAX_LENGTH(*value) > 0) {
                *out_bytes = (long long)STRING_MAX_LENGTH(*value) + 1;
            } else {
                *out_bytes = (long long)sizeof(char*);
            }
            return true;
        case TYPE_ARRAY: {
            int total = calculateArrayTotalSize(value);
            if (total < 0) {
                total = 0;
            }
            long long elem_size = 0;
            bool have_elem = false;
            // arrayUsesPackedBytes must be checked before AS_ARRAY(*value):
            // raw/elements are a union inside ArrayObj (VM 2.0 Phase 4e/4f).
            if (!arrayUsesPackedBytes(value) && AS_ARRAY(*value) && total > 0) {
                for (int i = 0; i < total; ++i) {
                    if (computeValueSizeBytesInternal(&AS_ARRAY(*value)[i], &elem_size, depth + 1)) {
                        have_elem = true;
                        break;
                    }
                }
            }
            if (!have_elem && ARRAY_ELEMENT_TYPE(*value) != TYPE_VOID) {
                have_elem = builtinSizeForVarType(ARRAY_ELEMENT_TYPE(*value), &elem_size);
                if (!have_elem) {
                    Value temp = makeValueForType(ARRAY_ELEMENT_TYPE(*value), ARRAY_ELEMENT_TYPE_DEF(*value), NULL);
                    have_elem = computeValueSizeBytesInternal(&temp, &elem_size, depth + 1);
                    freeValue(&temp);
                }
            }
            if (!have_elem) {
                return false;
            }
            long long count = (long long)total;
            if (elem_size > 0 && count > 0 && elem_size > LLONG_MAX / count) {
                return false;
            }
            *out_bytes = elem_size * count;
            return true;
        }
        case TYPE_RECORD: {
            long long total = 0;
            const Value* seen_storage[128];
            int seen_count = 0;
            for (FieldValue *field = AS_RECORD(*value); field; field = field->next) {
                const Value *fieldValue = fieldValueStorageConst(field);
                bool already_counted = false;
                for (int i = 0; i < seen_count; i++) {
                    if (seen_storage[i] == fieldValue) {
                        already_counted = true;
                        break;
                    }
                }
                if (already_counted) {
                    continue;
                }
                long long field_size = 0;
                if (!computeValueSizeBytesInternal(fieldValue, &field_size, depth + 1)) {
                    return false;
                }
                if (seen_count < (int)(sizeof(seen_storage) / sizeof(seen_storage[0]))) {
                    seen_storage[seen_count++] = fieldValue;
                }
                total += field_size;
            }
            *out_bytes = total;
            return true;
        }
        case TYPE_SET:
            if (AS_SET(*value).set_size > 0) {
                *out_bytes = (long long)AS_SET(*value).set_size * (long long)sizeof(long long);
            } else {
                *out_bytes = 0;
            }
            return true;
        case TYPE_NIL:
            *out_bytes = (long long)sizeof(void*);
            return true;
        default:
            if (builtinSizeForVarType(VALUE_TYPE(*value), out_bytes)) {
                return true;
            }
            return false;
    }
}

#ifdef SDL
static bool sdlReadKeyBufferHasData(void) {
    return gSdlReadKeyBufferCount > 0;
}

static int sdlReadKeyBufferPop(void) {
    if (!sdlReadKeyBufferHasData()) {
        return 0;
    }

    int value = gSdlReadKeyBuffer[gSdlReadKeyBufferStart];
    gSdlReadKeyBufferStart = (gSdlReadKeyBufferStart + 1) % SDL_READKEY_BUFFER_CAPACITY;
    gSdlReadKeyBufferCount--;
    return value & 0xFF;
}

static void sdlReadKeyBufferPushBytes(const int* bytes, int length) {
    if (!bytes || length <= 0) {
        return;
    }

    for (int i = 0; i < length; ++i) {
        if (gSdlReadKeyBufferCount >= SDL_READKEY_BUFFER_CAPACITY) {
            break;
        }
        int tail = (gSdlReadKeyBufferStart + gSdlReadKeyBufferCount) % SDL_READKEY_BUFFER_CAPACITY;
        gSdlReadKeyBuffer[tail] = bytes[i] & 0xFF;
        gSdlReadKeyBufferCount++;
    }
}

static int sdlTranslateKeycode(SDL_Keycode code, int* extraBytes, int* extraCount) {
    if (extraBytes) {
        extraBytes[0] = 0;
        extraBytes[1] = 0;
        extraBytes[2] = 0;
        extraBytes[3] = 0;
    }
    if (extraCount) {
        *extraCount = 0;
    }

    switch (code) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return '\r';
        case SDLK_BACKSPACE:
            return '\b';
        case SDLK_TAB:
            return '\t';
        case SDLK_ESCAPE:
            return 27;
        case SDLK_DELETE:
            return 127;
        case SDLK_LEFT:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'D';
                *extraCount = 2;
            }
            return 27;
        case SDLK_RIGHT:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'C';
                *extraCount = 2;
            }
            return 27;
        case SDLK_UP:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'A';
                *extraCount = 2;
            }
            return 27;
        case SDLK_DOWN:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'B';
                *extraCount = 2;
            }
            return 27;
        case SDLK_HOME:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'H';
                *extraCount = 2;
            }
            return 27;
        case SDLK_END:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'F';
                *extraCount = 2;
            }
            return 27;
        case SDLK_KP_0:
            return '0';
        case SDLK_KP_1:
            return '1';
        case SDLK_KP_2:
            return '2';
        case SDLK_KP_3:
            return '3';
        case SDLK_KP_4:
            return '4';
        case SDLK_KP_5:
            return '5';
        case SDLK_KP_6:
            return '6';
        case SDLK_KP_7:
            return '7';
        case SDLK_KP_8:
            return '8';
        case SDLK_KP_9:
            return '9';
        case SDLK_KP_PERIOD:
            return '.';
        case SDLK_KP_DIVIDE:
            return '/';
        case SDLK_KP_MULTIPLY:
            return '*';
        case SDLK_KP_MINUS:
            return '-';
        case SDLK_KP_PLUS:
            return '+';
        case SDLK_KP_EQUALS:
            return '=';
        default:
            break;
    }

    if (code >= 32 && code <= 126) {
        return (int)code;
    }

    if (code >= 0 && code <= 255) {
        return (int)(code & 0xFF);
    }

    return 0;
}

static int sdlFetchReadKeyChar(void) {
    if (!sdlIsGraphicsActive()) {
        return -1;
    }

    if (sdlReadKeyBufferHasData()) {
        return sdlReadKeyBufferPop();
    }

    int extraBytes[4];
    SDL_Keycode keycode;

    for (;;) {
        keycode = sdlWaitNextKeycode();
        if (keycode == SDLK_UNKNOWN) {
            return 0;
        }

        int extraCount = 0;
        int translated = sdlTranslateKeycode(keycode, extraBytes, &extraCount);
        if (extraCount > 0) {
            sdlReadKeyBufferPushBytes(extraBytes, extraCount);
        }

        if (translated != 0) {
            return translated & 0xFF;
        }

        if (sdlReadKeyBufferHasData()) {
            return sdlReadKeyBufferPop();
        }
    }
}
#endif

#ifndef SDL
#if defined(__GNUC__) || defined(__clang__)
#define SDL_UNUSED_FUNC __attribute__((unused))
#else
#define SDL_UNUSED_FUNC
#endif
static Value SDL_UNUSED_FUNC vmBuiltinSDLUnavailable(VM* vm, int arg_count, Value* args) {
    (void)arg_count;
    (void)args;
    const char* name = (vm && vm->current_builtin_name) ? vm->current_builtin_name : "This built-in";
    runtimeError(vm, "Built-in '%s' requires SDL support. Rebuild with -DSDL=ON to enable it.", name);
    if (vm) {
        vm->abort_requested = true;
    }
    return makeNil();
}
#define SDL_HANDLER(fn) vmBuiltinSDLUnavailable
#undef SDL_UNUSED_FUNC
#else
#define SDL_HANDLER(fn) fn
#endif

// Per-thread state to keep core builtins thread-safe
static _Thread_local DIR* dos_dir = NULL; // Used by dosFindfirst/findnext

/* Random number seeding.
 *
 * rand_seed is per-thread, which is what keeps the draws thread-safe -- but a
 * worker thread that never seeds itself starts from the same constant as every
 * other worker and draws an IDENTICAL stream. That made every `par` branch
 * produce the same "random" numbers, so a parallel sampler silently computed
 * the same draws in each branch and reported a confidently wrong aggregate.
 * Randomize() only ever touched the calling thread, so it could not fix this,
 * and calling it inside each branch did not help either: it seeded from
 * time(NULL), whose whole-second resolution handed both branches the same value.
 *
 * Each thread now derives its own seed on first use from a shared base mixed
 * with a unique per-thread index. That gives all three properties at once:
 *   - branches diverge, because their indices differ;
 *   - with no Randomize() the base stays at its default, so a run is still
 *     bit-for-bit reproducible (each thread deterministically from its index);
 *   - Randomize() moves the base using sub-second entropy, so back-to-back runs
 *     differ -- time(NULL) alone repeats for anything launched twice in a second.
 *
 * Thread indices are handed out in the order threads first draw a number, so
 * *which* branch gets which stream is not deterministic across runs. The
 * streams themselves are independent, which is the property that matters here;
 * pinning a stream to a particular branch would need a branch id the VM does
 * not expose to builtins.
 */
static _Thread_local unsigned int rand_seed = 1;
static _Thread_local int rand_seed_ready = 0;
static _Thread_local unsigned int rand_thread_index = 0;
static _Thread_local int rand_thread_index_ready = 0;
static atomic_uint gRandBaseSeed = 1u;
static atomic_uint gRandThreadCounter = 0u;

/* splitmix32-style avalanche: adjacent indices must not yield adjacent seeds,
 * or the generator's early outputs stay visibly correlated between branches. */
static unsigned int randMixSeed(unsigned int base, unsigned int index) {
    unsigned int x = base + index * 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static unsigned int randThreadIndex(void) {
    if (!rand_thread_index_ready) {
        rand_thread_index = atomic_fetch_add(&gRandThreadCounter, 1u);
        rand_thread_index_ready = 1;
    }
    return rand_thread_index;
}

static void randEnsureSeeded(void) {
    if (!rand_seed_ready) {
        rand_seed = randMixSeed(atomic_load(&gRandBaseSeed), randThreadIndex());
        rand_seed_ready = 1;
    }
}

/* time(NULL) has whole-second resolution; fold in nanoseconds so two runs
 * started inside the same second do not replay the same sequence. */
static unsigned int randEntropy(void) {
    unsigned int entropy = (unsigned int)time(NULL);
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        entropy ^= (unsigned int)tv.tv_usec * 0x9E3779B9u;
    }
    return entropy;
}

// Terminal cursor helper
static int getCursorPosition(int *row, int *col);

// Small buffer to requeue bytes we peek during ReadKey (e.g., when skipping
// DSR responses). Keeps console input lossless when we consume more than one
// byte while deciding what to return.
static _Thread_local unsigned char gReadKeyBuf[64];
static _Thread_local int gReadKeyBufStart = 0;
static _Thread_local int gReadKeyBufCount = 0;
#if defined(PSCAL_TARGET_IOS)
static _Thread_local bool gReadKeyPendingCaretControl = false;
#endif

static bool readKeyBufHasData(void) {
    return gReadKeyBufCount > 0;
}

static int readKeyBufPop(void) {
    if (!readKeyBufHasData()) {
        return -1;
    }
    int value = gReadKeyBuf[gReadKeyBufStart];
    gReadKeyBufStart = (gReadKeyBufStart + 1) % (int)sizeof(gReadKeyBuf);
    gReadKeyBufCount--;
    return value;
}

static void readKeyBufPush(unsigned char byte) {
    if (gReadKeyBufCount >= (int)sizeof(gReadKeyBuf)) {
        return;
    }
    int tail = (gReadKeyBufStart + gReadKeyBufCount) % (int)sizeof(gReadKeyBuf);
    gReadKeyBuf[tail] = byte;
    gReadKeyBufCount++;
}

/* Is the escape sequence captured so far complete? CSI (ESC [ ... final
 * byte 0x40-0x7E, with the Linux console's ESC [ [ x function keys), SS3
 * (ESC O x) and Alt+key (ESC x) all end at a known byte, so ReadKey can stop
 * there instead of waiting out its timeout after every arrow key. */
static bool readKeySeqComplete(const unsigned char *seq, size_t len) {
    if (len < 2) {
        return false;
    }
    if (seq[1] == '[') {
        if (len < 3) {
            return false;
        }
        if (len == 3 && seq[2] == '[') {
            return false;
        }
        unsigned char last = seq[len - 1];
        return last >= 0x40 && last <= 0x7E;
    }
    if (seq[1] == 'O') {
        return len >= 3;
    }
    return true;
}

// Fetch a console byte, skipping any stray DSR responses (ESC [ row ; col R)
// that may have been sent by the terminal for cursor queries. Ensures those
// sequences do not satisfy ReadKey.
static int readKeyFetchConsoleByte(void) {
    for (;;) {
        if (readKeyBufHasData()) {
            return readKeyBufPop();
        }
        unsigned char ch = 0;
#if defined(PSCAL_TARGET_IOS)
        ssize_t n = vprocReadShim(STDIN_FILENO, &ch, 1);
#else
        ssize_t n = read(STDIN_FILENO, &ch, 1);
#endif
        if (n != 1) {
            return 0;
        }
        if (ch != 0x1B) {
            return (int)ch;
        }

        /* Capture the full sequence after ESC to decide if it's a DSR reply. */
        unsigned char seq[64];
        size_t seq_len = 0;
        seq[seq_len++] = 0x1B;

        int orig_flags = fcntl(STDIN_FILENO, F_GETFL);
        bool toggled_nonblock = false;
        if (orig_flags != -1 && (orig_flags & O_NONBLOCK) == 0) {
            if (fcntl(STDIN_FILENO, F_SETFL, orig_flags | O_NONBLOCK) == 0) {
                toggled_nonblock = true;
            }
        }

        /* Wait at most this long for the rest of a sequence; a lone ESC
         * key press costs this much. A complete sequence ends the wait at
         * once (it used to wait 200 ms after every arrow key too). */
        const int max_wait_ms = 100;
        int waited_ms = 0;
        while (seq_len < sizeof(seq) && !readKeySeqComplete(seq, seq_len)) {
            unsigned char b = 0;
#if defined(PSCAL_TARGET_IOS)
            ssize_t m = vprocReadShim(STDIN_FILENO, &b, 1);
#else
            ssize_t m = read(STDIN_FILENO, &b, 1);
#endif
            if (m == 1) {
                seq[seq_len++] = b;
                continue;
            }
            if (m < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waited_ms < max_wait_ms) {
                struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
                (void)poll(&pfd, 1, 10);
                waited_ms += 10;
                continue;
            }
            break;
        }

        if (toggled_nonblock && orig_flags != -1) {
            fcntl(STDIN_FILENO, F_SETFL, orig_flags);
        }

        /* Check if the captured sequence is ESC [ digits ; digits R */
        bool is_dsr = false;
        size_t r_pos = 0;
        for (size_t i = 0; i < seq_len; ++i) {
            if (seq[i] == 'R') {
                r_pos = i;
                break;
            }
        }
        if (seq_len >= 4 && seq[0] == 0x1B && seq[1] == '[' && r_pos > 2) {
            bool ok = true;
            bool saw_digit = false;
            bool saw_sep = false;
            for (size_t i = 2; i < r_pos; ++i) {
                unsigned char b = seq[i];
                if (b >= '0' && b <= '9') {
                    saw_digit = true;
                    continue;
                }
                if (b == ';') {
                    if (!saw_digit) { ok = false; break; }
                    saw_sep = true;
                    saw_digit = false;
                    continue;
                }
                ok = false;
                break;
            }
            if (ok && saw_digit && saw_sep) {
                is_dsr = true;
            }
        }

        /* The FIFO is empty here (this function only reads the console once
         * it is), so the bytes go back in in their arrival order. They used to
         * be pushed last-first, which made Up-arrow (ESC [ A) read back as
         * 'A', '[', ESC. */
        if (is_dsr) {
            /* Discard the DSR sequence and continue. Preserve any trailing bytes after R. */
            for (size_t i = r_pos + 1; i < seq_len; ++i) {
                readKeyBufPush(seq[i]);
            }
            continue;
        }

        /* Not a DSR: hand back the first byte, queue the rest. */
        for (size_t i = 1; i < seq_len; ++i) {
            readKeyBufPush(seq[i]);
        }
        return seq[0];
    }
}

// Real -> integer for int/trunc/round/floor/ceil. `x` is already integral (the
// caller applied truncl/roundl/floorl/ceill). The result is as wide as it needs
// to be: makeInt() alone tags TYPE_INT32, which turned int(5000000000.0) into
// 705032704 and round(-3000000000.0) into 1294967296. A Real with no 64-bit
// integer value (NaN, an infinity, |x| >= 2^63) is a runtime error, because
// casting it is undefined behaviour in C (int(1e20) came back as -1).
static Value vmRealToIntResult(VM* vm, const char* who, long double x) {
    if (x >= -9223372036854775808.0L && x < 9223372036854775808.0L) {
        return pscalIntResultNarrow((long long)x);
    }
    runtimeError(vm, "Runtime Error: %s(%.16Lg) has no 64-bit integer value.", who, x);
    return makeInt(0);
}

// ---- CLike-style conversion helpers (Phase 1) ----
static Value vmBuiltinToInt(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "int(x) expects 1 argument.");
        return makeInt(0);
    }
    Value v = args[0];
    long long i = 0;
    if (isRealType(VALUE_TYPE(v))) {
        return vmRealToIntResult(vm, "int", truncl(AS_REAL(v))); // toward zero, like a C cast
    } else if (IS_INTLIKE(v)) {
        return pscalIntResultLike1(v, AS_INTEGER(v));
    } else if (VALUE_TYPE(v) == TYPE_BOOLEAN) {
        i = VAL_INT(v) ? 1 : 0;
    } else if (VALUE_TYPE(v) == TYPE_CHAR) {
        i = AS_CHAR(v);
    } else {
        i = 0;
    }
    return makeInt(i);
}

static Value vmBuiltinToDouble(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "double(x) expects 1 argument.");
        return makeReal(0.0);
    }
    Value v = args[0];
    long double d = 0.0L;
    if (isRealType(VALUE_TYPE(v))) {
        d = AS_REAL(v);
    } else if (IS_INTLIKE(v)) {
        d = (long double)AS_INTEGER(v);
    } else if (VALUE_TYPE(v) == TYPE_BOOLEAN) {
        d = VAL_INT(v) ? 1.0L : 0.0L;
    } else if (VALUE_TYPE(v) == TYPE_CHAR) {
        d = (long double)AS_CHAR(v);
    } else {
        d = 0.0L;
    }
    return makeReal(d);
}

static Value vmBuiltinToFloat(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "float(x) expects 1 argument.");
        return makeFloat(0.0f);
    }
    Value v = args[0];
    float f = 0.0f;
    if (isRealType(VALUE_TYPE(v))) {
        f = (float)AS_REAL(v);
    } else if (IS_INTLIKE(v)) {
        f = (float)AS_INTEGER(v);
    } else if (VALUE_TYPE(v) == TYPE_BOOLEAN) {
        f = VAL_INT(v) ? 1.0f : 0.0f;
    } else if (VALUE_TYPE(v) == TYPE_CHAR) {
        f = (float)AS_CHAR(v);
    } else {
        f = 0.0f;
    }
    return makeFloat(f);
}

static Value vmBuiltinToChar(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "char(x) expects 1 argument.");
        return makeChar('\0');
    }
    Value v = args[0];
    unsigned char c = 0;
    if (isRealType(VALUE_TYPE(v))) {
        long double d = AS_REAL(v);
        // Casting a NaN, an infinity or |x| >= 2^63 to an integer is
        // undefined behaviour in C; like int(x), that is an error.
        if (!(d > -9223372036854775809.0L && d < 9223372036854775808.0L)) {
            runtimeError(vm, "Runtime Error: char(%.16Lg) has no integer value.", d);
            return makeChar('\0');
        }
        c = (unsigned char)((long long)d); // truncate then narrow
    } else if (IS_INTLIKE(v)) {
        c = (unsigned char)AS_INTEGER(v);
    } else if (VALUE_TYPE(v) == TYPE_BOOLEAN) {
        c = VAL_INT(v) ? 1 : 0;
    } else if (VALUE_TYPE(v) == TYPE_CHAR) {
        c = (unsigned char)AS_CHAR(v);
    } else {
        c = 0;
    }
    return makeChar((int)c);
}

static Value vmBuiltinToByte(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "byte(x) expects 1 argument.");
        return makeByte(0);
    }
    Value v = args[0];
    unsigned char b = 0;
    if (isRealType(VALUE_TYPE(v))) {
        long double d = AS_REAL(v);
        // See char(x): an out-of-range cast is undefined behaviour.
        if (!(d > -9223372036854775809.0L && d < 9223372036854775808.0L)) {
            runtimeError(vm, "Runtime Error: byte(%.16Lg) has no integer value.", d);
            return makeByte(0);
        }
        b = (unsigned char)((long long)d);
    } else if (IS_INTLIKE(v)) {
        b = (unsigned char)AS_INTEGER(v);
    } else if (VALUE_TYPE(v) == TYPE_BOOLEAN) {
        b = VAL_INT(v) ? 1 : 0;
    } else if (VALUE_TYPE(v) == TYPE_CHAR) {
        b = (unsigned char)AS_CHAR(v);
    } else if (isPascalStringType(VALUE_TYPE(v)) && AS_STRING(v) && AS_STRING(v)[0] != '\0') {
        /* byte(Text) had no case at all, so any Text argument silently fell
         * through to the `else { b = 0; }` default below -- no error, just
         * wrong data (byte("a") returned 0, not 97). A Byte is inherently an
         * 8-bit value, so this takes the raw first byte rather than
         * decoding a full UTF-8 codepoint (unlike ord(), which returns the
         * full codepoint) -- correct for the common single-char-Text case
         * this builtin exists for, and a defined (if lossy) truncation for
         * a non-ASCII leading byte, consistent with Byte's 0-255 range. */
        b = (unsigned char)AS_STRING(v)[0];
    } else {
        b = 0;
    }
    return makeByte(b);
}

static Value vmBuiltinToBool(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "bool(x) expects 1 argument.");
        return makeBoolean(0);
    }
    Value v = args[0];
    int truth = 0;
    if (isRealType(VALUE_TYPE(v))) {
        truth = (AS_REAL(v) != 0.0L);
    } else if (IS_INTLIKE(v)) {
        truth = (AS_INTEGER(v) != 0);
    } else if (VALUE_TYPE(v) == TYPE_BOOLEAN) {
        truth = VAL_INT(v) ? 1 : 0;
    } else if (VALUE_TYPE(v) == TYPE_CHAR) {
        truth = (AS_CHAR(v) != 0);
    } else {
        truth = 0;
    }
    return makeBoolean(truth);
}

// The new dispatch table for the VM - MUST be defined before the function that uses it
// Legacy entries remain sorted alphabetically by name (lowercase). Append new builtins at the
// end of the array to avoid shifting established builtin IDs.
// SDL/graphics builtins use NULL placeholders; registerGraphicsBuiltins() overrides them when
// SDL support is enabled so legacy builtin IDs remain stable.
static VmBuiltinMapping vmBuiltinDispatchTable[] = {
    {"abs", vmBuiltinAbs},
    {"apiReceive", vmBuiltinApiReceive},
    {"apiSend", vmBuiltinApiSend},
    {"httpsession", vmBuiltinHttpSession},
    {"httpclose", vmBuiltinHttpClose},
    {"httperrorcode", vmBuiltinHttpErrorCode},
    {"httpgetlastheaders", vmBuiltinHttpGetLastHeaders},
    {"httpgetheader", vmBuiltinHttpGetHeader},
    {"httpsetheader", vmBuiltinHttpSetHeader},
    {"httpclearheaders", vmBuiltinHttpClearHeaders},
    {"httpsetoption", vmBuiltinHttpSetOption},
    {"httprequest", vmBuiltinHttpRequest},
    {"httprequesttofile", vmBuiltinHttpRequestToFile},
    {"httprequestasync", vmBuiltinHttpRequestAsync},
    {"httprequestasynctofile", vmBuiltinHttpRequestAsyncToFile},
    {"httpisdone", vmBuiltinHttpIsDone},
    {"httptryawait", vmBuiltinHttpTryAwait},
    {"httpcancel", vmBuiltinHttpCancel},
    {"httpgetasyncprogress", vmBuiltinHttpGetAsyncProgress},
    {"httpgetasynctotal", vmBuiltinHttpGetAsyncTotal},
    {"httpawait", vmBuiltinHttpAwait},
    {"httplasterror", vmBuiltinHttpLastError},
    {"jsonget", vmBuiltinJsonGet},
    {"append", vmBuiltinAppend},
    {"arccos", vmBuiltinArccos},
    {"arcsin", vmBuiltinArcsin},
    {"arctan", vmBuiltinArctan},
    {"assign", vmBuiltinAssign},
    {"assignfile", vmBuiltinAssign},
    {"beep", vmBuiltinBeep},
    {"biblinktext", vmBuiltinBlinktext},
    {"biboldtext", vmBuiltinBoldtext},
    {"biclrscr", vmBuiltinClrscr},
    {"bilowvideo", vmBuiltinLowvideo},
    {"binormvideo", vmBuiltinNormvideo},
    {"biunderlinetext", vmBuiltinUnderlinetext},
    {"biwherex", vmBuiltinWherex},
    {"biwherey", vmBuiltinWherey},
    {"blinktext", vmBuiltinBlinktext},
    {"boldtext", vmBuiltinBoldtext},
    {"bool", vmBuiltinToBool},
    {"byte", vmBuiltinToByte},
    {"bytecodeversion", vmBuiltinBytecodeVersion},
    {"ceil", vmBuiltinCeil},
    {"char", vmBuiltinToChar},
    {"chr", vmBuiltinChr},
    {"cleardevice", NULL},
    {"clreol", vmBuiltinClreol},
    {"clrscr", vmBuiltinClrscr},
    {"close", vmBuiltinClose},
    {"closefile", vmBuiltinClose},
    {"closegraph", NULL},
    {"closegraph3d", NULL},
    {"copy", vmBuiltinCopy},
    {"cos", vmBuiltinCos},
    {"cosh", vmBuiltinCosh},
    {"cotan", vmBuiltinCotan},
    {"cursoroff", vmBuiltinCursoroff},
    {"cursoron", vmBuiltinCursoron},
    {"createtargettexture", NULL}, // Moved
    {"createtexture", NULL}, // Moved
    {"dec", vmBuiltinDec},
    {"delay", vmBuiltinDelay},
    {"deline", vmBuiltinDeline},
    {"destroytexture", NULL},
    {"dispose", vmBuiltinDispose},
    {"dnslookup", vmBuiltinDnsLookup},
    {"dosExec", vmBuiltinDosExec},
    {"dosFindfirst", vmBuiltinDosFindfirst},
    {"dosFindnext", vmBuiltinDosFindnext},
    {"dosGetdate", vmBuiltinDosGetdate},
    {"dosGetenv", vmBuiltinDosGetenv},
    {"dosGetfattr", vmBuiltinDosGetfattr},
    {"dosGettime", vmBuiltinDosGettime},
    {"dosMkdir", vmBuiltinDosMkdir},
    {"dosRmdir", vmBuiltinDosRmdir},
    {"double", vmBuiltinToDouble},
    {"drawcircle", NULL}, // Moved
    {"drawline", NULL}, // Moved
    {"drawpolygon", NULL}, // Moved
    {"drawrect", NULL}, // Moved
    {"eof", vmBuiltinEof},
    {"erase", vmBuiltinErase},
    {"exec", vmBuiltinDosExec},
    {"exit", vmBuiltinExit},
    {"exp", vmBuiltinExp},
    {"fillcircle", NULL},
    {"fillrect", NULL},
    {"findfirst", vmBuiltinDosFindfirst},
    {"findnext", vmBuiltinDosFindnext},
    {"float", vmBuiltinToFloat},
    {"floor", vmBuiltinFloor},
    {"format", vmBuiltinFormat},
    {"formatfloat", vmBuiltinFormatfloat},
    {"freesound", NULL},
    {"getdate", vmBuiltinDosGetdate},
    {"getenv", vmBuiltinGetenv},
    {"getenvint", vmBuiltinGetenvint},
    {"getfattr", vmBuiltinDosGetfattr},
    {"getmaxx", NULL},
    {"getmaxy", NULL},
    {"getmousestate", NULL},
    {"getpixelcolor", NULL}, // Moved
    {"gettextsize", NULL},
    {"getticks", NULL},
    {"glbegin", NULL},
    {"glclear", NULL},
    {"glclearcolor", NULL},
    {"glcleardepth", NULL},
    {"glcolor3f", NULL},
    {"gldepthtest", NULL},
    {"glend", NULL},
    {"glfrustum", NULL},
    {"glloadidentity", NULL},
    {"glmatrixmode", NULL},
    {"glpopmatrix", NULL},
    {"glpushmatrix", NULL},
    {"glrotatef", NULL},
    {"glscalef", NULL},
    {"glperspective", NULL},
    {"glsetswapinterval", NULL},
    {"glswapwindow", NULL},
    {"gltranslatef", NULL},
    {"glvertex3f", NULL},
    {"glviewport", NULL},
    {"gettime", vmBuiltinDosGettime},
    {"graphloop", NULL},
    {"gotoxy", vmBuiltinGotoxy},
    {"halt", vmBuiltinHalt},
    {"hidecursor", vmBuiltinHidecursor},
    {"high", vmBuiltinHigh},
    {"highvideo", vmBuiltinHighvideo},
    {"inc", vmBuiltinInc},
    {"initgraph", NULL},
    {"initgraph3d", NULL},
    {"initsoundsystem", NULL},
    {"inittextsystem", NULL},
    {"insline", vmBuiltinInsline},
    {"int", vmBuiltinToInt},
    {"inttostr", vmBuiltinInttostr},
    {"invertcolors", vmBuiltinInvertcolors},
    {"ioresult", vmBuiltinIoresult},
    {"issoundplaying", NULL}, // Moved
    {"keypressed", vmBuiltinKeypressed},
    {"length", vmBuiltinLength},
    {"ln", vmBuiltinLn},
    {"log10", vmBuiltinLog10},
    {"loadimagetotexture", NULL}, // Moved
    {"loadsound", NULL},
    {"low", vmBuiltinLow},
    {"lowvideo", vmBuiltinLowvideo},
    {"max", vmBuiltinMax},
    {"min", vmBuiltinMin},
    {"clamp", vmBuiltinClamp},
    {"mkdir", vmBuiltinDosMkdir},
    {"mstreamcreate", vmBuiltinMstreamcreate},
    {"mstreamfree", vmBuiltinMstreamfree},
    {"mstreamloadfromfile", vmBuiltinMstreamloadfromfile},
    {"mstreamsavetofile", vmBuiltinMstreamsavetofile},
    {"mstreambuffer", vmBuiltinMstreambuffer},
    {"mstreamappendbyte", vmBuiltinMstreamAppendByte},
    {"newobj", vmBuiltinNewObj},
    {"new", vmBuiltinNew},
    {"normalcolors", vmBuiltinNormalcolors},
    {"normvideo", vmBuiltinNormvideo},
    {"ord", vmBuiltinOrd},
    {"outtextxy", NULL}, // Moved
    {"paramcount", vmBuiltinParamcount},
    {"paramstr", vmBuiltinParamstr},
    {"playsound", NULL},
    {"stopallsounds", NULL},
    {"pollkey", NULL},
    {"iskeydown", NULL},
    {"popscreen", vmBuiltinPopscreen},
    {"pos", vmBuiltinPos},
    {"power", vmBuiltinPower},
    {"printf", vmBuiltinPrintf},
    {"fopen", vmBuiltinFopen},
    {"fclose", vmBuiltinFclose},
    {"pushscreen", vmBuiltinPushscreen},
    {"putpixel", NULL},
    {"write", vmBuiltinWrite}, // Preserve legacy builtin id for write
    {"fprintf", vmBuiltinFprintf}, // Registered after write to avoid shifting legacy id 176
    {"quitsoundsystem", NULL},
    {"quittextsystem", NULL},
    {"random", vmBuiltinRandom},
    {"randomize", vmBuiltinRandomize},
    {"read", vmBuiltinRead},
    {"readkey", vmBuiltinReadkey},
    {"readln", vmBuiltinReadln},
    {"real", vmBuiltinReal},
    {"realtostr", vmBuiltinRealtostr},
    {"rename", vmBuiltinRename},
    {"rendercopy", NULL}, // Moved
    {"rendercopyex", NULL}, // Moved
    {"rendercopyrect", NULL},
    {"rendertexttotexture", NULL},
    {"reset", vmBuiltinReset},
    {"restorecursor", vmBuiltinRestorecursor},
    {"rewrite", vmBuiltinRewrite},
    {"rmdir", vmBuiltinDosRmdir},
    {"round", vmBuiltinRound},
    {"savecursor", vmBuiltinSavecursor},
    {"screencols", vmBuiltinScreencols},
    {"screenrows", vmBuiltinScreenrows},
    {"setlength", vmBuiltinSetlength},
    {"setalphablend", NULL},
    {"setcolor", NULL}, // Moved
    {"setrendertarget", NULL}, // Moved
    {"setrgbcolor", NULL},
    {"showcursor", vmBuiltinShowcursor},
    {"sin", vmBuiltinSin},
    {"sinh", vmBuiltinSinh},
    {"socketaccept", vmBuiltinSocketAccept},
    {"socketbind", vmBuiltinSocketBind},
    {"socketbindaddr", vmBuiltinSocketBindAddr},
    {"socketclose", vmBuiltinSocketClose},
    {"socketconnect", vmBuiltinSocketConnect},
    {"socketcreate", vmBuiltinSocketCreate},
    {"socketlasterror", vmBuiltinSocketLastError},
    {"socketlisten", vmBuiltinSocketListen},
    {"socketpoll", vmBuiltinSocketPoll},
    {"socketreceive", vmBuiltinSocketReceive},
    {"socketsend", vmBuiltinSocketSend},
    {"socketsetblocking", vmBuiltinSocketSetBlocking},
    {"sqr", vmBuiltinSqr},
    {"sqrt", vmBuiltinSqrt},
    {"str", vmBuiltinStr},
    {"stringofchar", vmBuiltinStringOfChar},
    {"succ", vmBuiltinSucc},
    {"tan", vmBuiltinTan},
    {"tanh", vmBuiltinTanh},
    {"textbackground", vmBuiltinTextbackground},
    {"textbackgrounde", vmBuiltinTextbackgrounde},
    {"textcolor", vmBuiltinTextcolor},
    {"textcolore", vmBuiltinTextcolore},
    {"trim", vmBuiltinTrim},
    {"trunc", vmBuiltinTrunc},
    {"underlinetext", vmBuiltinUnderlinetext},
    {"upcase", vmBuiltinUpcase},
    {"toupper", vmBuiltinUpcase},
    {"updatescreen", NULL},
    {"updatetexture", NULL},
    {"val", vmBuiltinVal},
    {"valreal", vmBuiltinValreal},
    {"vmversion", vmBuiltinVMVersion},
    {"waitkeyevent", NULL}, // Moved
    {"wherex", vmBuiltinWherex},
    {"wherey", vmBuiltinWherey},
    {"window", vmBuiltinWindow},
    {"quitrequested", vmBuiltinQuitrequested},
    {"getscreensize", NULL},
    {"pollkeyany", vmBuiltinPollkeyany},
    {"threadgetresult", vmBuiltinThreadGetResult},
    {"threadgetstatus", vmBuiltinThreadGetStatus},
    {"threadspawnbuiltin", vmBuiltinThreadSpawnBuiltin},
    {"waitforthread", vmBuiltinWaitForThread},
    {"threadcancel", vmBuiltinThreadCancel},
    {"threadlookup", vmBuiltinThreadLookup},
    {"threadpause", vmBuiltinThreadPause},
    {"threadpoolsubmit", vmBuiltinThreadPoolSubmit},
    {"threadresume", vmBuiltinThreadResume},
    {"threadsetname", vmBuiltinThreadSetName},
    {"threadstats", vmBuiltinThreadStats},
    {"threadstatsjson", vmBuiltinThreadStatsJson},
    {"atan2", vmBuiltinAtan2},
    {"blockread", vmBuiltinBlockread},
    {"blockwrite", vmBuiltinBlockwrite},
    {"sizeof", vmBuiltinSizeof},
    {"filesize", vmBuiltinFilesize},
    {"glcullface", NULL}, // Append new builtins at the end of the array to avoid shifting legacy IDs.
    {"gllinewidth", NULL},
    {"gldepthmask", NULL},
    {"gldepthfunc", NULL},
    {"fflush", vmBuiltinFflush},
    {"socketpeeraddr", vmBuiltinSocketPeerAddr},
    {"odd", vmBuiltinOdd},
    /* pow was briefly inserted next to power (mid-table), which shifted every
     * id after it and broke bytecode compatibility; appended here instead. */
    {"pow", vmBuiltinPower},   /* C-style alias of power(base, exp) */
    /* VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1):
     * appended here per this table's own append-only convention above. */
    {"taskspawn", vmBuiltinTaskSpawn},
    {"taskawait", vmBuiltinTaskAwait},
    {"taskdone", vmBuiltinTaskDone},
    {"taskcancel", vmBuiltinTaskCancel},
    /* VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2):
     * appended here per this table's own append-only convention above. */
    {"channelcreate", vmBuiltinChannelCreate},
    {"channelsend", vmBuiltinChannelSend},
    {"channelreceive", vmBuiltinChannelReceive},
    {"channeltrysend", vmBuiltinChannelTrySend},
    {"channeltryreceive", vmBuiltinChannelTryReceive},
    {"channelclose", vmBuiltinChannelClose},
    {"channelisclosed", vmBuiltinChannelIsClosed},
};

static const size_t num_vm_builtins = sizeof(vmBuiltinDispatchTable) / sizeof(vmBuiltinDispatchTable[0]);

static bool threadBuiltinIsAllowlisted(int id);

/* Dynamic registry for user-supplied VM built-ins. Written under
 * builtin_registry_mutex.
 *
 * CALL_BUILTIN looks a handler up by id on every call, and it used to take
 * builtin_registry_mutex twice per call (handler and name) -- a recursive
 * mutex every worker thread contended on. The by-id readers are lock-free
 * instead: the core table's names never change and its handlers are written
 * with atomic stores, and the extra table is published as an atomic
 * pointer plus count. The table is only ever replaced by a larger copy, and
 * a replaced table is never freed, because a reader may still be using it
 * (they only double, so the retired tables together are smaller than the
 * live one). The count is stored after the entry it covers is complete. */
static VmBuiltinMapping *extra_vm_builtins = NULL;
static size_t num_extra_vm_builtins = 0;
static size_t extra_vm_builtins_capacity = 0;
static _Atomic(VmBuiltinMapping *) g_extra_builtins_published = NULL;
static atomic_size_t g_extra_builtins_published_count = 0;
static pthread_mutex_t builtin_registry_mutex;
static pthread_once_t builtin_registry_once = PTHREAD_ONCE_INIT;

/* VM 2.0 Phase 6 (Docs/pscal_vm2_plan.md §6.3): per-id effect-mask overrides.
 * Sparse, indexed by builtin id (core ids included, since registerVmBuiltin
 * can replace a core dispatch-table placeholder, e.g. SDL graphics). Grown
 * lazily; g_effect_override_set[id] tells whether g_effect_overrides[id] is
 * meaningful, since FX_PURE (0) is itself a valid mask. Ids without an
 * override fall back to name-based classification (core builtins) or FX_IO
 * (extra builtins registered without ever calling the new registerVmBuiltin
 * mask parameter -- shouldn't happen, but conservative if it does). */
static EffectMask *g_effect_overrides = NULL;
static bool *g_effect_override_set = NULL;
static size_t g_effect_overrides_cap = 0;

static bool ensureEffectOverrideCapacityUnlocked(size_t id) {
    if (id < g_effect_overrides_cap) {
        return true;
    }
    size_t new_cap = g_effect_overrides_cap ? g_effect_overrides_cap * 2 : 64;
    while (new_cap <= id) {
        new_cap *= 2;
    }
    EffectMask *new_masks = realloc(g_effect_overrides, sizeof(EffectMask) * new_cap);
    if (!new_masks) return false;
    g_effect_overrides = new_masks;
    bool *new_set = realloc(g_effect_override_set, sizeof(bool) * new_cap);
    if (!new_set) return false;
    g_effect_override_set = new_set;
    for (size_t i = g_effect_overrides_cap; i < new_cap; ++i) {
        g_effect_overrides[i] = FX_PURE;
        g_effect_override_set[i] = false;
    }
    g_effect_overrides_cap = new_cap;
    return true;
}

static void setEffectOverrideUnlocked(size_t id, EffectMask mask) {
    if (!ensureEffectOverrideCapacityUnlocked(id)) return;
    g_effect_overrides[id] = mask;
    g_effect_override_set[id] = true;
}

/* Lazily-computed per-core-builtin masks, classified by name (see
 * pscalBuiltinNameEffectMask below). Populated once so getVmBuiltinEffectMaskById
 * is O(1) even under an active --deny policy. */
static EffectMask *g_core_builtin_masks = NULL;

typedef struct {
    Symbol symbol;
    size_t id;
} BuiltinRegistryEntry;

static HashTable *builtinRegistryHash = NULL;

static VmBuiltinMapping *builtinMappingFromId(size_t id) {
    if (id < num_vm_builtins) {
        return &vmBuiltinDispatchTable[id];
    }
    size_t extra_index = id - num_vm_builtins;
    if (extra_index < num_extra_vm_builtins) {
        return &extra_vm_builtins[extra_index];
    }
    return NULL;
}

static bool canonicalizeBuiltinName(const char *name, char *out, size_t out_size) {
    if (!name || !out || out_size == 0) {
        return false;
    }

    size_t i = 0;
    for (; i + 1 < out_size && name[i]; ++i) {
        out[i] = (char)tolower((unsigned char)name[i]);
    }
    out[i] = '\0';
    return i > 0;
}

static BuiltinRegistryEntry *lookupBuiltinEntryUnlocked(const char *canonical_name) {
    if (!canonical_name || !builtinRegistryHash) {
        return NULL;
    }
    Symbol *sym = hashTableLookup(builtinRegistryHash, canonical_name);
    if (!sym) {
        return NULL;
    }
    return (BuiltinRegistryEntry *)sym;
}

static void builtinRegistryInsertUnlocked(const char *canonical_name, size_t id) {
    if (!builtinRegistryHash || !canonical_name || !*canonical_name) {
        return;
    }

    BuiltinRegistryEntry *existing = lookupBuiltinEntryUnlocked(canonical_name);
    if (existing) {
        existing->id = id;
        if (existing->symbol.name && strcmp(existing->symbol.name, canonical_name) != 0) {
            char *dup = strdup(canonical_name);
            if (dup) {
                free(existing->symbol.name);
                existing->symbol.name = dup;
            }
        }
        return;
    }

    BuiltinRegistryEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return;
    }
    entry->symbol.name = strdup(canonical_name);
    if (!entry->symbol.name) {
        free(entry);
        return;
    }
    entry->symbol.is_alias = true;
    entry->id = id;
    hashTableInsert(builtinRegistryHash, &entry->symbol);
}

static VmBuiltinMapping *builtinRegistryLookupMappingUnlocked(const char *canonical_name, size_t *out_id) {
    if (out_id) {
        *out_id = SIZE_MAX;
    }
    if (!canonical_name || !*canonical_name) {
        return NULL;
    }

    if (builtinRegistryHash) {
        BuiltinRegistryEntry *entry = lookupBuiltinEntryUnlocked(canonical_name);
        if (!entry) {
            return NULL;
        }
        if (out_id) {
            *out_id = entry->id;
        }
        return builtinMappingFromId(entry->id);
    }

    for (size_t i = 0; i < num_vm_builtins; ++i) {
        if (strcasecmp(canonical_name, vmBuiltinDispatchTable[i].name) == 0) {
            if (out_id) {
                *out_id = i;
            }
            return &vmBuiltinDispatchTable[i];
        }
    }
    for (size_t i = 0; i < num_extra_vm_builtins; ++i) {
        if (strcasecmp(canonical_name, extra_vm_builtins[i].name) == 0) {
            if (out_id) {
                *out_id = num_vm_builtins + i;
            }
            return &extra_vm_builtins[i];
        }
    }
    return NULL;
}

/* Builtin ids ARE the dispatch-table indices, and CALL_BUILTIN_PROC bakes
 * them into compiled bytecode, so the table layout is an ABI: never insert,
 * remove, or reorder entries mid-table. Append new builtins at the end and
 * keep {"name", NULL} placeholders for retired ones. These sentinels pin a
 * spread of well-known ids and fail fast at startup when the invariant is
 * broken (a mid-table insertion shifts everything after it and makes stale
 * bytecode dispatch to the WRONG builtin). Extend the list when appending. */
static const struct { size_t id; const char *name; } kBuiltinIdSentinels[] = {
    {0,   "abs"},
    {175, "power"},
    {181, "write"},
    {182, "fprintf"},
    {276, "odd"},
    {277, "pow"},
    {278, "taskspawn"},
    {281, "taskcancel"},
};

static void verifyBuiltinIdSentinels(void) {
    bool ok = true;
    for (size_t i = 0; i < sizeof(kBuiltinIdSentinels) / sizeof(kBuiltinIdSentinels[0]); ++i) {
        size_t id = kBuiltinIdSentinels[i].id;
        const char *want = kBuiltinIdSentinels[i].name;
        const char *have = (id < num_vm_builtins) ? vmBuiltinDispatchTable[id].name : NULL;
        if (!have || strcasecmp(have, want) != 0) {
            fprintf(stderr,
                    "FATAL: builtin id %zu is '%s' but must be '%s'. Builtin ids are baked "
                    "into compiled bytecode; vmBuiltinDispatchTable is append-only (use a "
                    "{\"name\", NULL} placeholder to retire an entry, never remove/reorder).\n",
                    id, have ? have : "<out of range>", want);
            ok = false;
        }
    }
    if (!ok) {
        EXIT_FAILURE_HANDLER();
    }
}

static void initBuiltinRegistryMutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&builtin_registry_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    verifyBuiltinIdSentinels();

    builtinRegistryHash = createHashTable();
    if (!builtinRegistryHash) {
        return;
    }

    for (size_t i = 0; i < num_vm_builtins; ++i) {
        char canonical[MAX_SYMBOL_LENGTH];
        if (canonicalizeBuiltinName(vmBuiltinDispatchTable[i].name, canonical, sizeof(canonical))) {
            builtinRegistryInsertUnlocked(canonical, i);
        }
    }
}

/* Single source of truth for builtin effectfulness/effect-class, shared by
 * the Aether FX-001 gate, the builtin-introspection metadata (builtins_json /
 * builtin_info), and the VM 2.0 Phase 6 --deny sandbox + record/replay
 * machinery (Docs/pscal_vm2_plan.md §6.3). Anything that touches the outside
 * world, is nondeterministic, or observes concurrent state gets a nonzero
 * mask; pure builtins (math, string, conversion, in-memory streams, TOON
 * reads, type predicates) are deliberately absent (FX_PURE). Includes both
 * raw VM names and the Aether-spelling aliases (ai_chat, sleep, task_*) so a
 * single check serves every front end. This is the seed classification for
 * core dispatch-table builtins (see getVmBuiltinEffectMaskById); ext_builtins
 * categories additionally pass an explicit mask to registerVmBuiltin(). */
typedef struct {
    const char *name;
    EffectMask mask;
} EffectClassification;

static const EffectClassification kEffectClassifiedNames[] = {
    /* output / stdio */
    {"print", FX_IO}, {"printf", FX_IO}, {"println", FX_IO}, {"fprintf", FX_IO},
    {"write", FX_IO}, {"writeln", FX_IO}, {"read", FX_IO}, {"readln", FX_IO},
    {"flush", FX_IO},
    /* process control */
    {"exit", FX_PROC}, {"halt", FX_PROC}, {"store", FX_PROC}, {"swap", FX_PROC},
    {"fetch", FX_PROC},
    /* timing (raw + Aether alias) */
    {"delay", FX_CLOCK}, {"sleep", FX_CLOCK},
    /* AI (raw + Aether alias) -- goes over the network */
    {"openaichatcompletions", FX_NET}, {"ai_chat", FX_NET},
    /* tasks / threads (Aether aliases + raw) */
    {"task_spawn", FX_PROC}, {"task_queue", FX_PROC}, {"task_wait", FX_PROC},
    {"task_lookup", FX_PROC}, {"task_status", FX_PROC}, {"task_result", FX_PROC},
    {"task_stats", FX_PROC}, {"task_cancel", FX_PROC}, {"task_pause", FX_PROC},
    {"task_resume", FX_PROC}, {"task_set_name", FX_PROC},
    {"thread_spawn_named", FX_PROC}, {"thread_pool_submit", FX_PROC},
    {"thread_set_name", FX_PROC}, {"thread_cancel", FX_PROC},
    {"thread_pause", FX_PROC}, {"thread_resume", FX_PROC},
    {"thread_get_result", FX_PROC}, {"thread_get_status", FX_PROC},
    {"thread_lookup", FX_PROC}, {"thread_stats", FX_PROC},
    {"threadspawnbuiltin", FX_PROC}, {"waitforthread", FX_PROC},
    /* the raw spellings of the thread_* rows above; only the aliases were
     * classified, so threadpoolsubmit passed --deny proc as FX_PURE */
    {"threadpoolsubmit", FX_PROC}, {"threadgetresult", FX_PROC},
    {"threadgetstatus", FX_PROC}, {"threadcancel", FX_PROC},
    {"threadlookup", FX_PROC}, {"threadpause", FX_PROC},
    {"threadresume", FX_PROC}, {"threadsetname", FX_PROC},
    {"threadstats", FX_PROC}, {"threadstatsjson", FX_PROC},
    /* VM 2.0 Phase 5a checkpoint 5a-i: the new TYPE_TASK builtins -- distinct
     * from the task_* Aether-alias names above, which are Aether's existing
     * frontend sugar over ThreadSpawnBuiltin/ThreadPoolSubmit, not this. */
    {"taskspawn", FX_PROC}, {"taskawait", FX_PROC}, {"taskdone", FX_PROC},
    {"taskcancel", FX_PROC},
    /* filesystem + file-handle I/O */
    {"fileexists", FX_IO}, {"getcurrentdir", FX_IO}, {"mkdir", FX_IO},
    {"rmdir", FX_IO}, {"rename", FX_IO}, {"erase", FX_IO}, {"findfirst", FX_IO},
    {"findnext", FX_IO}, {"getfattr", FX_IO}, {"fopen", FX_IO}, {"fclose", FX_IO},
    {"fflush", FX_IO}, {"filesize", FX_IO}, {"blockread", FX_IO},
    {"blockwrite", FX_IO}, {"ioresult", FX_IO}, {"eof", FX_IO},
    {"assignfile", FX_IO}, {"closefile", FX_IO}, {"mstreamloadfromfile", FX_IO},
    {"mstreamsavetofile", FX_IO}, {"toon_parse_file", FX_IO},
    {"yyjsonreadfile", FX_IO}, /* canonical lowering of toon_parse_file */
    /* environment / process / CLI */
    {"getenv", FX_IO}, {"getenvint", FX_IO}, {"paramstr", FX_IO},
    {"paramcount", FX_IO}, {"getpid", FX_PROC}, {"exec", FX_PROC}, {"beep", FX_IO},
    {"dosexec", FX_PROC}, {"dosgetenv", FX_IO}, {"dosfindfirst", FX_IO},
    {"dosfindnext", FX_IO}, {"dosgetdate", FX_CLOCK}, {"dosgettime", FX_CLOCK},
    {"dosgetfattr", FX_IO}, {"dosmkdir", FX_IO}, {"dosrmdir", FX_IO},
    /* nondeterminism */
    {"random", FX_RANDOM}, {"randomize", FX_RANDOM},
    /* external clock */
    {"gettime", FX_CLOCK}, {"getdate", FX_CLOCK}, {"realtimeclock", FX_CLOCK},
    /* console input */
    {"readkey", FX_IO}, {"keypressed", FX_IO},
    /* network */
    {"dnslookup", FX_NET}, {"apisend", FX_NET}, {"apireceive", FX_NET},
    {"httpsession", FX_NET}, {"httpclose", FX_NET}, {"httperrorcode", FX_NET},
    {"httpgetlastheaders", FX_NET}, {"httpgetheader", FX_NET},
    {"httpsetheader", FX_NET}, {"httpclearheaders", FX_NET},
    {"httpsetoption", FX_NET}, {"httprequest", FX_NET},
    {"httprequesttofile", FX_NET}, {"httprequestasync", FX_NET},
    {"httprequestasynctofile", FX_NET}, {"httpisdone", FX_NET},
    {"httptryawait", FX_NET}, {"httpcancel", FX_NET},
    {"httpgetasyncprogress", FX_NET}, {"httpgetasynctotal", FX_NET},
    {"httpawait", FX_NET}, {"httplasterror", FX_NET},
    /* raw sockets: core dispatch-table builtins, so this row is their only
     * classification. Missing, they were FX_PURE: callable from Aether
     * @pure and outside fx, and not stopped by --deny net. */
    {"socketcreate", FX_NET}, {"socketbind", FX_NET},
    {"socketbindaddr", FX_NET}, {"socketlisten", FX_NET},
    {"socketaccept", FX_NET}, {"socketconnect", FX_NET},
    {"socketsend", FX_NET}, {"socketreceive", FX_NET},
    {"socketpoll", FX_NET}, {"socketclose", FX_NET},
    {"socketsetblocking", FX_NET}, {"socketpeeraddr", FX_NET},
    {"socketlasterror", FX_NET},
    /* database (sqlite) */
    {"sqliteopen", FX_IO}, {"sqliteclose", FX_IO}, {"sqliteexec", FX_IO},
    {"sqliteprepare", FX_IO}, {"sqlitefinalize", FX_IO}, {"sqlitestep", FX_IO},
    {"sqlitereset", FX_IO}, {"sqlitecolumncount", FX_IO},
    {"sqlitecolumntype", FX_IO}, {"sqlitecolumnname", FX_IO},
    {"sqlitecolumnint", FX_IO}, {"sqlitecolumndouble", FX_IO},
    {"sqlitecolumntext", FX_IO}, {"sqlitebindtext", FX_IO},
    {"sqlitebindint", FX_IO}, {"sqlitebinddouble", FX_IO},
    {"sqlitebindnull", FX_IO}, {"sqliteclearbindings", FX_IO},
    {"sqliteerrmsg", FX_IO}, {"sqlitelastinsertrowid", FX_IO},
    {"sqlitechanges", FX_IO},
};

EffectMask pscalBuiltinNameEffectMask(const char *name) {
    if (!name) return FX_PURE;
    for (size_t i = 0; i < sizeof(kEffectClassifiedNames) / sizeof(kEffectClassifiedNames[0]); ++i) {
        if (strcasecmp(kEffectClassifiedNames[i].name, name) == 0) {
            return kEffectClassifiedNames[i].mask;
        }
    }
    return FX_PURE;
}

bool pscalBuiltinNameIsEffectful(const char *name) {
    return pscalBuiltinNameEffectMask(name) != FX_PURE;
}

EffectMask pscalBuiltinNameEffectMaskLive(const char *name) {
    int id = getVmBuiltinID(name);
    if (id >= 0) {
        return getVmBuiltinEffectMaskById(id);
    }
    return pscalBuiltinNameEffectMask(name);
}

EffectMask getVmBuiltinEffectMaskById(int id) {
    if (id < 0) return FX_PURE;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    EffectMask result = FX_PURE;
    size_t uid = (size_t)id;
    if (uid < g_effect_overrides_cap && g_effect_override_set[uid]) {
        result = g_effect_overrides[uid];
    } else if (uid < num_vm_builtins) {
        if (!g_core_builtin_masks) {
            g_core_builtin_masks = calloc(num_vm_builtins, sizeof(EffectMask));
            if (g_core_builtin_masks) {
                for (size_t i = 0; i < num_vm_builtins; ++i) {
                    g_core_builtin_masks[i] = pscalBuiltinNameEffectMask(vmBuiltinDispatchTable[i].name);
                }
            }
        }
        result = g_core_builtin_masks ? g_core_builtin_masks[uid] : pscalBuiltinNameEffectMask(vmBuiltinDispatchTable[uid].name);
    } else {
        /* Extra builtin id with no recorded mask: registerVmBuiltin always
         * records one now, so this is defensive only. Conservative default
         * per Docs/pscal_vm2_plan.md §6.3. */
        result = FX_IO;
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return result;
}

void registerVmBuiltin(const char *name, VmBuiltinFn handler,
        BuiltinRoutineType type, const char *display_name, EffectMask effect_mask) {
    if (!name || !handler) return;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);

    if (type == BUILTIN_TYPE_FUNCTION || type == BUILTIN_TYPE_PROCEDURE) {
        const char *reg_name = display_name ? display_name : name;
        ASTNodeType declType = (type == BUILTIN_TYPE_FUNCTION)
                                   ? AST_FUNCTION_DECL
                                   : AST_PROCEDURE_DECL;
        registerBuiltinFunction(reg_name, declType, NULL);
    }

    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    size_t existing_id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, &existing_id);
    if (mapping) {
        __atomic_store_n(&mapping->handler, handler, __ATOMIC_RELEASE);
        if (existing_id != SIZE_MAX) {
            setEffectOverrideUnlocked(existing_id, effect_mask);
        }
        pthread_mutex_unlock(&builtin_registry_mutex);
        return;
    }

    if (num_extra_vm_builtins == extra_vm_builtins_capacity) {
        // Grow by copying into a new table, never realloc in place: see the
        // comment on extra_vm_builtins for why the old one stays allocated.
        size_t new_capacity = extra_vm_builtins_capacity ? extra_vm_builtins_capacity * 2 : 32;
        VmBuiltinMapping *new_table = malloc(sizeof(VmBuiltinMapping) * new_capacity);
        if (!new_table) {
            pthread_mutex_unlock(&builtin_registry_mutex);
            return;
        }
        if (num_extra_vm_builtins > 0) {
            memcpy(new_table, extra_vm_builtins, sizeof(VmBuiltinMapping) * num_extra_vm_builtins);
        }
        extra_vm_builtins = new_table;
        extra_vm_builtins_capacity = new_capacity;
        atomic_store_explicit(&g_extra_builtins_published, new_table, memory_order_release);
    }
    extra_vm_builtins[num_extra_vm_builtins].name = strdup(canonical);
    extra_vm_builtins[num_extra_vm_builtins].handler = handler;
    size_t new_index = num_extra_vm_builtins;
    num_extra_vm_builtins++;
    atomic_store_explicit(&g_extra_builtins_published_count, num_extra_vm_builtins, memory_order_release);
    size_t new_id = num_vm_builtins + new_index;
    builtinRegistryInsertUnlocked(canonical, new_id);
    setEffectOverrideUnlocked(new_id, effect_mask);
    pthread_mutex_unlock(&builtin_registry_mutex);
}

// This function now comes AFTER the table and comparison function it uses.
VmBuiltinFn getVmBuiltinHandler(const char *name) {
    if (!name) return NULL;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return NULL;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    VmBuiltinFn handler = NULL;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, NULL);
    if (mapping) {
        handler = mapping->handler;
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return handler;
}

/* The extra-table entry for `extra_index`, or NULL; lock-free (see the
 * comment on extra_vm_builtins). */
static const VmBuiltinMapping *publishedExtraBuiltin(size_t extra_index) {
    size_t count = atomic_load_explicit(&g_extra_builtins_published_count, memory_order_acquire);
    if (extra_index >= count) {
        return NULL;
    }
    VmBuiltinMapping *table = atomic_load_explicit(&g_extra_builtins_published, memory_order_acquire);
    return table ? &table[extra_index] : NULL;
}

VmBuiltinFn getVmBuiltinHandlerById(int id) {
    if (id < 0) return NULL;
    if ((size_t)id < num_vm_builtins) {
        return __atomic_load_n(&vmBuiltinDispatchTable[id].handler, __ATOMIC_ACQUIRE);
    }
    const VmBuiltinMapping *extra = publishedExtraBuiltin((size_t)id - num_vm_builtins);
    return extra ? __atomic_load_n(&extra->handler, __ATOMIC_ACQUIRE) : NULL;
}

const char* getVmBuiltinNameById(int id) {
    if (id < 0) return NULL;
    if ((size_t)id < num_vm_builtins) {
        return vmBuiltinDispatchTable[id].name;
    }
    const VmBuiltinMapping *extra = publishedExtraBuiltin((size_t)id - num_vm_builtins);
    return extra ? extra->name : NULL;
}

size_t getVmBuiltinCount(void) {
    return num_vm_builtins + atomic_load_explicit(&g_extra_builtins_published_count, memory_order_acquire);
}

BuiltinRoutineType getVmBuiltinTypeById(int id) {
    if (id < 0) {
        return BUILTIN_TYPE_NONE;
    }
    const char *name = getVmBuiltinNameById(id);
    if (!name) {
        return BUILTIN_TYPE_NONE;
    }
    return getBuiltinType(name);
}

bool getVmBuiltinMapping(const char *name, VmBuiltinMapping *out_mapping, int *out_id) {
    if (out_id) {
        *out_id = -1;
    }
    if (!name) {
        return false;
    }
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return false;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    size_t id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, &id);
    bool found = mapping != NULL;
    if (found) {
        if (out_mapping) {
            *out_mapping = *mapping;
        }
        if (out_id && id <= (size_t)INT_MAX) {
            *out_id = (int)id;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return found;
}

bool getVmBuiltinMappingCanonical(const char *canonical_name, VmBuiltinMapping *out_mapping, int *out_id) {
    if (out_id) {
        *out_id = -1;
    }
    if (!canonical_name || !*canonical_name) {
        return false;
    }

    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    size_t id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical_name, &id);
    bool found = mapping != NULL;
    if (found) {
        if (out_mapping) {
            *out_mapping = *mapping;
        }
        if (out_id && id <= (size_t)INT_MAX) {
            *out_id = (int)id;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return found;
}

#define MAX_BUILTIN_NAME_ALIASES 4
static struct {
    int frontendKind;
    BuiltinNameAliasFn resolver;
} gBuiltinNameAliases[MAX_BUILTIN_NAME_ALIASES];
static size_t gBuiltinNameAliasCount = 0;

void registerBuiltinNameAlias(int frontendKind, BuiltinNameAliasFn resolver) {
    if (!resolver) {
        return;
    }
    for (size_t i = 0; i < gBuiltinNameAliasCount; ++i) {
        if (gBuiltinNameAliases[i].frontendKind == frontendKind &&
            gBuiltinNameAliases[i].resolver == resolver) {
            return;
        }
    }
    if (gBuiltinNameAliasCount < MAX_BUILTIN_NAME_ALIASES) {
        gBuiltinNameAliases[gBuiltinNameAliasCount].frontendKind = frontendKind;
        gBuiltinNameAliases[gBuiltinNameAliasCount].resolver = resolver;
        gBuiltinNameAliasCount++;
    }
}

const char* resolveBuiltinNameAlias(const char* name) {
    if (!name) {
        return NULL;
    }
    int kind = (int)frontendGetKind();
    for (size_t i = 0; i < gBuiltinNameAliasCount; ++i) {
        if (gBuiltinNameAliases[i].frontendKind != kind) {
            continue;
        }
        const char* engine = gBuiltinNameAliases[i].resolver(name);
        if (engine && *engine) {
            return engine;
        }
    }
    return NULL;
}

int getVmBuiltinID(const char *name) {
    if (!name) {
        return -1;
    }
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return -1;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    size_t id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, &id);
    int result = -1;
    if (mapping && id != SIZE_MAX && id <= (size_t)INT_MAX) {
        if (id < num_vm_builtins) {
            if (mapping->handler) {
                result = (int)id;
            }
        } else if (id - num_vm_builtins < num_extra_vm_builtins) {
            result = (int)id;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);

    return result;
}

Value vmBuiltinSqr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Sqr expects 1 argument.");
        return makeInt(0);
    }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) {
        long long v = AS_INTEGER(arg);
        long long r;
        if (__builtin_mul_overflow(v, v, &r)) {
            runtimeError(vm, "Runtime Error: Integer overflow.");
            return makeInt(0);
        }
        return pscalIntResultLike1(arg, r);
    } else if (isRealType(VALUE_TYPE(arg))) {
        long double v = AS_REAL(arg);
        return makeReal(v * v);
    }
    runtimeError(vm, "Sqr expects an Integer or Real argument. Got %s.", varTypeToString(VALUE_TYPE(arg)));
    return makeInt(0);
}

Value vmBuiltinChr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "Chr expects 1 integer argument.");
        return makeChar('\0');
    }
    long long code = AS_INTEGER(args[0]);
    if (code < 0 || code > UNICODE_MAX) {
        runtimeError(vm, "Chr argument out of range (0-0x10FFFF).");
        return makeChar('\0');
    }
    if (code <= PASCAL_CHAR_MAX) {
        return makeChar((int)code);
    }
    return makeWideChar((int)code);
}

Value vmBuiltinSucc(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Succ expects 1 argument.");
        return makeVoid();
    }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) {
        // The successor of the type's largest value does not exist: it was a
        // signed overflow (undefined behaviour, wrapping to the minimum).
        long long cur = AS_INTEGER(arg);
        if (VALUE_TYPE(arg) == TYPE_UINT64) {
            if ((unsigned long long)cur == ULLONG_MAX) {
                runtimeError(vm, "Succ integer overflow.");
                return makeVoid();
            }
            return pscalIntResultLike1(arg, (long long)((unsigned long long)cur + 1ULL));
        }
        long long next;
        if (__builtin_add_overflow(cur, 1LL, &next)) {
            runtimeError(vm, "Succ integer overflow.");
            return makeVoid();
        }
        return pscalIntResultLike1(arg, next);
    }
    switch(VALUE_TYPE(arg)) {
        case TYPE_CHAR:
            if (AS_CHAR(arg) >= PASCAL_CHAR_MAX) {
                runtimeError(vm, "Succ char overflow.");
                return makeVoid();
            }
            return makeChar(AS_CHAR(arg) + 1);
        case TYPE_BOOLEAN: {
            long long next_val = VAL_INT(arg) + 1;
            int bool_result = next_val > 1 ? 1 : (next_val != 0);
            return makeBoolean(bool_result);
        }
        case TYPE_ENUM: {
            int ordinal = AS_ENUM(arg).ordinal;
            if (ENUM_META(arg) && ordinal + 1 >= ENUM_META(arg)->member_count) {
                runtimeError(vm, "Succ enum overflow.");
                return makeVoid();
            }
            Value result = makeEnum(AS_ENUM(arg).enum_name, ordinal + 1);
            ENUM_META(result) = ENUM_META(arg);
            ENUM_TYPE_DEF(result) = ENUM_TYPE_DEF(arg);
            return result;
        }
        default:
            runtimeError(vm, "Succ requires an ordinal type argument. Got %s.",
                         varTypeToString(VALUE_TYPE(arg)));
            return makeVoid();
    }
}

Value vmBuiltinUpcase(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Upcase expects 1 argument, got %d.", arg_count);
        return makeChar('\0');
    }

    Value arg = args[0];
    int c;
    if (VALUE_TYPE(arg) == TYPE_CHAR || VALUE_TYPE(arg) == TYPE_WIDECHAR) {
        c = AS_CHAR(arg);
    } else if (IS_INTLIKE(arg)) {
        c = (int)AS_INTEGER(arg);
    } else if (IS_REAL(arg)) {
        /*
         * Some frontends currently promote integer literals or variables to a
         * floating‑point type when used as arguments.  Accept real numbers and
         * coerce them back to an integer so `toupper` behaves correctly even if
         * the value was widened to a real earlier in the pipeline.
         */
        c = (int)AS_REAL(arg);
    } else if (isPascalStringType(VALUE_TYPE(arg))) {
        const char* s = AS_STRING(arg);
        if (s && s[0] != '\0') {
            uint32_t codepoint = 0;
            size_t advance = 0;
            if (decodeUtf8Codepoint(s, strlen(s), &codepoint, &advance)) {
                c = (int)codepoint;
            } else {
                c = (unsigned char)s[0];
            }
        } else {
            runtimeError(vm,
                         "Upcase expects a non-empty string or char argument. Got an empty string.");
            return makeChar('\0');
        }
    } else {
        runtimeError(vm,
                     "Upcase expects a char, int, or non-empty string argument. Got %s.",
                     varTypeToString(VALUE_TYPE(arg)));
        return makeChar('\0');
    }
    if (c >= 0 && c <= 255) {
        return makeChar(toupper((unsigned char)c));
    }
    return makeWideChar(c);
}

Value vmBuiltinPos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { // Changed this to a general error
        runtimeError(vm, "Pos expects 2 arguments.");
        return makeInt(0);
    }
    // Allow the first argument to be a char
    if (!isPascalStringType(VALUE_TYPE(args[0])) && !isPascalCharType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "Pos first argument must be a string or char.");
        return makeInt(0);
    }
    if (!isPascalStringType(VALUE_TYPE(args[1]))) {
        runtimeError(vm, "Pos second argument must be a string.");
        return makeInt(0);
    }

    const char* needle = NULL;
    char needle_buf[5] = {0};
    if (VALUE_TYPE(args[0]) == TYPE_CHAR) {
        needle_buf[0] = AS_CHAR(args[0]);
        needle = needle_buf;
    } else if (VALUE_TYPE(args[0]) == TYPE_WIDECHAR) {
        encodeUtf8Codepoint((uint32_t)AS_CHAR(args[0]), needle_buf);
        needle = needle_buf;
    } else {
        needle = AS_STRING(args[0]);
    }
    const char* haystack = AS_STRING(args[1]);

    /* The result follows the frontend's string base, and so does the "absent"
     * sentinel: 1-based frontends return 0 (which cannot be a valid 1-based
     * index), but 0 IS a valid index for a 0-based frontend -- a match at the
     * very start -- so those return -1 instead. Keeping 0 there would make a
     * prefix match indistinguishable from "not found". */
    const long long pos_base = frontendIsZeroBasedStrings() ? 0 : 1;
    const long long pos_absent = frontendIsZeroBasedStrings() ? -1 : 0;

    if (!needle || !haystack) return makeInt(pos_absent);
    // An empty substring is not found (Free Pascal: Pos('', s) = 0); strstr
    // matched it at the start. 0-based frontends keep their match at 0.
    if (needle[0] == '\0' && !frontendIsZeroBasedStrings()) return makeInt(pos_absent);

    const char* found = strstr(haystack, needle);
    if (!found) {
        return makeInt(pos_absent);
    }
    if (VALUE_TYPE(args[1]) == TYPE_UNICODE_STRING) {
        size_t prefix_len = (size_t)(found - haystack);
        return makeInt((long long)utf8CodepointCount(haystack, prefix_len) + pos_base);
    }
    return makeInt((long long)(found - haystack) + pos_base);
}

/* printf/fprintf/Format conversion helpers.
 *
 * Every integer argument these builtins hand to the C library is a long long
 * (or unsigned long long) and every real a long double, whatever length
 * modifier the program wrote -- the modifier only says how the program thinks
 * of the value. So the spec passed to the C library is rebuilt with the
 * modifier that matches what is actually passed: "ll" for the integer
 * conversions, "L" for the real ones, none for %c/%s/%p. Passing the
 * program's own modifier through made "%d" receive a long long and "%Lf" a
 * double (undefined behaviour; garbage on x86-64 Linux). */
static const char *builtinPrintfCLength(char spec) {
    switch (spec) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
            return "ll";
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            return "L";
        default:
            return "";
    }
}

/* Accumulate one decimal digit of a width/precision field, saturating instead
 * of overflowing int on an absurdly long digit run. */
static int builtinPrintfAccumDigit(int value, char digit) {
    if (value > (INT_MAX - 9) / 10) {
        return value;
    }
    return value * 10 + (digit - '0');
}

/* snprintf into a heap buffer sized for the whole result: no fixed-size
 * scratch buffer to silently cut a piece short. Returns NULL on failure. */
static char *builtinAsprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap2);
        return NULL;
    }
    char *out = (char *)malloc((size_t)needed + 1);
    if (out) {
        vsnprintf(out, (size_t)needed + 1, fmt, ap2);
    }
    va_end(ap2);
    return out;
}

Value vmBuiltinPrintf(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "printf expects a format string as the first argument.");
        return makeInt(0);
    }
    const char* fmt = AS_STRING(args[0]);
    int arg_index = 1;
    for (size_t i = 0; fmt && fmt[i] != '\0'; i++) {
        char c = fmt[i];
        if (c == '\\' && fmt[i + 1] != '\0') {
            char esc = fmt[++i];
            switch (esc) {
                case 'n': fputc('\n', stdout); break;
                case 'r': fputc('\r', stdout); break;
                case 't': fputc('\t', stdout); break;
                case '\\': fputc('\\', stdout); break;
                case '"': fputc('"', stdout); break;
                default: fputc(esc, stdout); break;
            }
            continue;
        }
        if (c == '%' && fmt[i + 1] != '\0') {
            if (fmt[i + 1] == '%') {
                fputc('%', stdout);
                i++;
                continue;
            }
            size_t j = i + 1;
            char flags[8];
            size_t flag_len = 0;
            const char* flag_chars = "-+ #0'";
            while (fmt[j] && strchr(flag_chars, fmt[j]) != NULL) {
                if (flag_len + 1 < sizeof(flags)) {
                    flags[flag_len++] = fmt[j];
                }
                j++;
            }
            flags[flag_len] = '\0';
            bool width_specified = false;
            int width = 0;
            while (isdigit((unsigned char)fmt[j])) {
                width_specified = true;
                width = builtinPrintfAccumDigit(width, fmt[j]);
                j++;
            }
            int precision = -1;
            if (fmt[j] == '.') {
                j++;
                precision = 0;
                while (isdigit((unsigned char)fmt[j])) {
                    precision = builtinPrintfAccumDigit(precision, fmt[j]);
                    j++;
                }
            }
            // Parse length modifiers and record small-width flags; we normalize by truncating manually
            bool mod_h = false, mod_hh = false;
            char length_mod[3] = {0};
            size_t length_len = 0;
            if (fmt[j] == 'h') {
                mod_h = true;
                length_mod[length_len++] = 'h';
                j++;
                if (fmt[j] == 'h') {
                    mod_hh = true;
                    mod_h = false;
                    length_mod[length_len++] = 'h';
                    j++;
                }
            } else if (fmt[j] == 'l') {
                length_mod[length_len++] = 'l';
                j++;
                if (fmt[j] == 'l') {
                    length_mod[length_len++] = 'l';
                    j++;
                }
            } else {
                const char* length_mods = "Ljzt";
                while (fmt[j] && strchr(length_mods, fmt[j]) != NULL) {
                    if (length_len + 1 < sizeof(length_mod)) {
                        length_mod[length_len++] = fmt[j];
                    }
                    j++;
                }
            }
            length_mod[length_len < sizeof(length_mod) ? length_len : (sizeof(length_mod) - 1)] = '\0';
            char spec = fmt[j];
            if (spec == '\0') {
                runtimeError(vm, "printf: incomplete format specifier.");
                return makeInt(0);
            }
            char fmtbuf[48];
            size_t pos = 0;
            fmtbuf[pos++] = '%';
            if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], flags, flag_len);
                pos += flag_len;
            }
            if (width_specified) {
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            if (precision >= 0 && pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = '.';
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            // The program's length modifier is not passed on (see
            // builtinPrintfCLength): the spec names what is actually passed.
            const char *c_length = builtinPrintfCLength(spec);
            size_t c_length_len = strlen(c_length);
            if (c_length_len > 0 && pos + c_length_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], c_length, c_length_len);
                pos += c_length_len;
            }
            if (pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = spec;
            }
            fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';
            if (arg_index < arg_count) {
                Value v = args[arg_index++];
                switch (spec) {
                    case 'd':
                    case 'i': {
                        long long iv = asI64(v);
                        if (mod_hh) iv = (signed char)iv; else if (mod_h) iv = (short)iv; // wider mods use default
                        fprintf(stdout, fmtbuf, iv);
                        break; }
                    case 'u':
                    case 'o':
                    case 'x':
                    case 'X': {
                        unsigned long long uv = (unsigned long long)asI64(v);
                        if (mod_hh) uv = (unsigned char)uv; else if (mod_h) uv = (unsigned short)uv;
                        fprintf(stdout, fmtbuf, uv);
                        break; }
                    case 'f':
                    case 'F':
                    case 'e':
                    case 'E':
                    case 'g':
                    case 'G':
                    case 'a':
                    case 'A':
                        fprintf(stdout, fmtbuf, (long double)AS_REAL(v));
                        break;
                    case 'c': {
                        char ch = (VALUE_TYPE(v) == TYPE_CHAR) ? AS_CHAR(v) : (char)asI64(v);
                        fprintf(stdout, fmtbuf, (int)ch);
                        break;
                    }
                    case 's': {
                        char char_text[5] = {0};
                        const char* sv = "";
                        if (isPascalStringType(VALUE_TYPE(v)) && AS_STRING(v)) {
                            sv = AS_STRING(v);
                        } else if (VALUE_TYPE(v) == TYPE_CHAR) {
                            char_text[0] = (char)AS_CHAR(v);
                            sv = char_text;
                        } else if (VALUE_TYPE(v) == TYPE_WIDECHAR) {
                            encodeUtf8Codepoint((uint32_t)AS_CHAR(v), char_text);
                            sv = char_text;
                        }
                        fprintf(stdout, fmtbuf, sv);
                        break;
                    }
                    case 'p':
                        fprintf(stdout, fmtbuf, (void*)(uintptr_t)asI64(v));
                        break;
                    default:
                        printValueToStream(v, stdout);
                        break;
                }
            } else {
                fputc('%', stdout);
                fputc(spec, stdout);
            }
            i = j;
            continue;
        }
        fputc(c, stdout);
    }
    fflush(stdout);
    return makeInt(0);
}

// fprintf(file, fmt, ...)
Value vmBuiltinFprintf(VM* vm, int arg_count, Value* args) {
    if (arg_count < 2) {
        runtimeError(vm, "fprintf expects at least (file, format).");
        return makeInt(0);
    }
    // Determine output FILE*
    FILE* output_stream = NULL;
    const Value* farg = &args[0];
    if (VALUE_TYPE(*farg) == TYPE_POINTER && AS_POINTER(*farg)) farg = (const Value*)AS_POINTER(*farg);
    if (VALUE_TYPE(*farg) == TYPE_FILE && AS_FILE(*farg)) {
        output_stream = AS_FILE(*farg);
    } else {
        runtimeError(vm, "fprintf first argument must be an open file.");
        return makeInt(0);
    }
    if (!isPascalStringType(VALUE_TYPE(args[1])) || !AS_STRING(args[1])) {
        runtimeError(vm, "fprintf expects a format string as the second argument.");
        return makeInt(0);
    }
    const char* fmt = AS_STRING(args[1]);
    int arg_index = 2;
    for (size_t i = 0; fmt && fmt[i] != '\0'; i++) {
        char c = fmt[i];
        if (c == '\\' && fmt[i + 1] != '\0') {
            char esc = fmt[++i];
            switch (esc) {
                case 'n': fputc('\n', output_stream); break;
                case 'r': fputc('\r', output_stream); break;
                case 't': fputc('\t', output_stream); break;
                case '\\': fputc('\\', output_stream); break;
                case '"': fputc('"', output_stream); break;
                default: fputc(esc, output_stream); break;
            }
            continue;
        }
        if (c == '%' && fmt[i + 1] != '\0') {
            if (fmt[i + 1] == '%') {
                fputc('%', output_stream);
                i++;
                continue;
            }
            size_t j = i + 1;
            char flags[8];
            size_t flag_len = 0;
            const char* flag_chars = "-+ #0'";
            while (fmt[j] && strchr(flag_chars, fmt[j]) != NULL) {
                if (flag_len + 1 < sizeof(flags)) {
                    flags[flag_len++] = fmt[j];
                }
                j++;
            }
            flags[flag_len] = '\0';
            bool width_specified = false;
            int width = 0;
            while (isdigit((unsigned char)fmt[j])) { width_specified = true; width = builtinPrintfAccumDigit(width, fmt[j]); j++; }
            int precision = -1;
            if (fmt[j] == '.') {
                j++; precision = 0;
                while (isdigit((unsigned char)fmt[j])) { precision = builtinPrintfAccumDigit(precision, fmt[j]); j++; }
            }
            bool mod_h = false, mod_hh = false;
            char length_mod[3] = {0};
            size_t length_len = 0;
            if (fmt[j] == 'h') {
                mod_h = true;
                length_mod[length_len++] = 'h';
                j++;
                if (fmt[j] == 'h') {
                    mod_hh = true;
                    mod_h = false;
                    length_mod[length_len++] = 'h';
                    j++;
                }
            } else if (fmt[j] == 'l') {
                length_mod[length_len++] = 'l';
                j++;
                if (fmt[j] == 'l') {
                    length_mod[length_len++] = 'l';
                    j++;
                }
            } else {
                const char* length_mods = "Ljzt";
                while (fmt[j] && strchr(length_mods, fmt[j]) != NULL) {
                    if (length_len + 1 < sizeof(length_mod)) {
                        length_mod[length_len++] = fmt[j];
                    }
                    j++;
                }
            }
            length_mod[length_len < sizeof(length_mod) ? length_len : (sizeof(length_mod) - 1)] = '\0';
            char spec = fmt[j];
            if (!spec) { runtimeError(vm, "fprintf: incomplete format specifier."); return makeInt(0); }
            char fmtbuf[48];
            size_t pos = 0;
            fmtbuf[pos++] = '%';
            if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], flags, flag_len);
                pos += flag_len;
            }
            if (width_specified) {
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            if (precision >= 0 && pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = '.';
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            // The program's length modifier is not passed on (see
            // builtinPrintfCLength): the spec names what is actually passed.
            const char *c_length = builtinPrintfCLength(spec);
            size_t c_length_len = strlen(c_length);
            if (c_length_len > 0 && pos + c_length_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], c_length, c_length_len);
                pos += c_length_len;
            }
            if (pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = spec;
            }
            fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';
            if (arg_index < arg_count) {
                Value v = args[arg_index++];
                switch (spec) {
                    case 'd': case 'i': {
                        long long iv = asI64(v);
                        if (mod_hh) iv = (signed char)iv; else if (mod_h) iv = (short)iv;
                        fprintf(output_stream, fmtbuf, iv);
                        break; }
                    case 'u': case 'o': case 'x': case 'X': {
                        unsigned long long uv = (unsigned long long)asI64(v);
                        if (mod_hh) uv = (unsigned char)uv; else if (mod_h) uv = (unsigned short)uv;
                        fprintf(output_stream, fmtbuf, uv);
                        break; }
                    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                        fprintf(output_stream, fmtbuf, (long double)AS_REAL(v));
                        break; }
                    case 'c': {
                        char ch = (VALUE_TYPE(v) == TYPE_CHAR) ? AS_CHAR(v) : (char)asI64(v);
                        fprintf(output_stream, fmtbuf, (int)ch);
                        break; }
                    case 's': {
                        char char_text[5] = {0};
                        const char* sv = "";
                        if (isPascalStringType(VALUE_TYPE(v)) && AS_STRING(v)) {
                            sv = AS_STRING(v);
                        } else if (VALUE_TYPE(v) == TYPE_CHAR) {
                            char_text[0] = (char)AS_CHAR(v);
                            sv = char_text;
                        } else if (VALUE_TYPE(v) == TYPE_WIDECHAR) {
                            encodeUtf8Codepoint((uint32_t)AS_CHAR(v), char_text);
                            sv = char_text;
                        }
                        fprintf(output_stream, fmtbuf, sv);
                        break; }
                    case 'p': {
                        fprintf(output_stream, fmtbuf, (void*)(uintptr_t)asI64(v));
                        break; }
                    default:
                        printValueToStream(v, output_stream);
                        break;
                }
            } else {
                fputc('%', output_stream);
                fputc(spec, output_stream);
            }
            i = j;
            continue;
        }
        fputc(c, output_stream);
    }
    fflush(output_stream);
    return makeInt(0);
}

// fflush([file])
Value vmBuiltinFflush(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) {
        fflush(NULL);
        return makeInt(0);
    }
    if (arg_count != 1) {
        runtimeError(vm, "fflush expects (file).");
        return makeInt(0);
    }
    const Value* farg = &args[0];
    if (VALUE_TYPE(*farg) == TYPE_POINTER && AS_POINTER(*farg)) {
        farg = (const Value*)AS_POINTER(*farg);
    }
    if (VALUE_TYPE(*farg) != TYPE_FILE || !AS_FILE(*farg)) {
        runtimeError(vm, "fflush requires a valid file argument.");
        return makeInt(0);
    }
    fflush(AS_FILE(*farg));
    return makeInt(0);
}

// fopen(path, mode) -> FILE
Value vmBuiltinFopen(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !isPascalStringType(VALUE_TYPE(args[0])) || !isPascalStringType(VALUE_TYPE(args[1]))) {
        runtimeError(vm, "fopen expects (path:string, mode:string).");
        return makeVoid();
    }
    const char* path = AS_STRING(args[0]);
    const char* mode = AS_STRING(args[1]);
    FILE* f = fopen(path, mode);
    if (!f) {
        runtimeError(vm, "fopen failed for '%s'", path);
        return makeVoid();
    }
    Value v = makeVoid();
    SET_VALUE_TYPE(&v, TYPE_FILE);
    pscalFileEnsureObj(&v);
    AS_FILE(v) = f;
    FILE_FILENAME(v) = strdup(path);
    return v;
}

// fclose(file)
Value vmBuiltinFclose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "fclose expects (file).");
        return makeVoid();
    }
    Value* farg = (Value*)&args[0];
    if (VALUE_TYPE(*farg) == TYPE_POINTER && AS_POINTER(*farg)) farg = (Value*)AS_POINTER(*farg);
    if (VALUE_TYPE(*farg) != TYPE_FILE || !AS_FILE(*farg)) {
        runtimeError(vm, "fclose requires a valid file argument.");
        return makeVoid();
    }
    vmMaybeCloseFileValue(farg);
    return makeVoid();
}

Value vmBuiltinCopy(VM* vm, int arg_count, Value* args) {
    // Allow the first argument to be a char
    if (arg_count != 3 || (!isPascalStringType(VALUE_TYPE(args[0])) && !isPascalCharType(VALUE_TYPE(args[0]))) || !IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2])) {
        runtimeError(vm, "Copy expects (String/Char, Integer, Integer).");
        return makeString("");
    }
    const char* source = NULL;
    char source_buf[5] = {0};
    if (VALUE_TYPE(args[0]) == TYPE_CHAR) {
        source_buf[0] = AS_CHAR(args[0]);
        source = source_buf;
    } else if (VALUE_TYPE(args[0]) == TYPE_WIDECHAR) {
        encodeUtf8Codepoint((uint32_t)AS_CHAR(args[0]), source_buf);
        source = source_buf;
    } else {
        source = AS_STRING(args[0]);
    }
    long long start_idx = AS_INTEGER(args[1]);
    long long count = AS_INTEGER(args[2]);

    /* `start` follows the frontend's string base: 0-based for Aether/shell,
     * 1-based for Pascal/rea/clike. Everything below works in 0-based byte and
     * codepoint offsets, so normalise once here. */
    const long long start_base = frontendIsZeroBasedStrings() ? 0 : 1;

    if (!source || start_idx < start_base || count < 0) {
        return (VALUE_TYPE(args[0]) == TYPE_UNICODE_STRING) ? makeUnicodeString("") : makeString("");
    }

    size_t source_len = strlen(source);
    if (VALUE_TYPE(args[0]) == TYPE_UNICODE_STRING) {
        size_t source_cp_len = utf8CodepointCount(source, source_len);
        size_t start_0based = (size_t)(start_idx - start_base);
        if (start_0based >= source_cp_len) return makeUnicodeString("");

        size_t start_byte = utf8ByteOffsetForCodepointIndex(source, source_len, start_0based);
        size_t end_index = start_0based + (size_t)count;
        if (end_index > source_cp_len) {
            end_index = source_cp_len;
        }
        size_t end_byte = utf8ByteOffsetForCodepointIndex(source, source_len, end_index);
        return makeUnicodeStringLen(source + start_byte, end_byte - start_byte);
    }

    size_t start_0based = (size_t)(start_idx - start_base);
    if (start_0based >= source_len) return makeString("");

    size_t len_to_copy = count;
    if (start_0based + len_to_copy > source_len) {
        len_to_copy = source_len - start_0based;
    }

    return makeStringLen(source + start_0based, len_to_copy);
}

Value vmBuiltinTrim(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || (!builtinValueIsStringLike(&args[0]) && !isPascalCharType(VALUE_TYPE(args[0])))) {
        runtimeError(vm, "Trim expects 1 string argument.");
        return makeString("");
    }

    char char_buf[5];
    const char *source;
    if (VALUE_TYPE(args[0]) == TYPE_CHAR) {
        char_buf[0] = AS_CHAR(args[0]);
        char_buf[1] = '\0';
        source = char_buf;
    } else if (VALUE_TYPE(args[0]) == TYPE_WIDECHAR) {
        size_t n = encodeUtf8Codepoint((uint32_t)AS_CHAR(args[0]), char_buf);
        char_buf[n] = '\0';
        source = char_buf;
    } else {
        source = builtinValueToCString(&args[0]);
    }
    if (!source) {
        runtimeError(vm, "Trim expects a valid string argument.");
        return makeString("");
    }

    const unsigned char *start = (const unsigned char *)source;
    while (*start != '\0' && isspace(*start)) {
        start++;
    }

    const unsigned char *end = (const unsigned char *)source + strlen(source);
    while (end > start && isspace(end[-1])) {
        end--;
    }

    return makeStringLen((const char *)start, (size_t)(end - start));
}

Value vmBuiltinStringOfChar(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "StringOfChar expects (Char/String, Integer).");
        return makeString("");
    }

    int fill = 0;
    char encoded_fill[5] = {0};
    size_t encoded_fill_len = 0;
    bool unicode_fill = false;
    if (VALUE_TYPE(args[0]) == TYPE_CHAR) {
        fill = (unsigned char)AS_CHAR(args[0]);
        encoded_fill[0] = (char)fill;
        encoded_fill[1] = '\0';
        encoded_fill_len = 1;
    } else if (VALUE_TYPE(args[0]) == TYPE_WIDECHAR) {
        fill = AS_CHAR(args[0]);
        encoded_fill_len = encodeUtf8Codepoint((uint32_t)fill, encoded_fill);
        unicode_fill = true;
    } else if (VALUE_TYPE(args[0]) == TYPE_STRING) {
        const char *source = AS_STRING(args[0]);
        fill = (source && source[0]) ? (unsigned char)source[0] : 0;
        encoded_fill[0] = (char)fill;
        encoded_fill[1] = '\0';
        encoded_fill_len = 1;
    } else if (VALUE_TYPE(args[0]) == TYPE_UNICODE_STRING) {
        const char *source = AS_STRING(args[0]);
        uint32_t codepoint = 0;
        size_t advance = 0;
        if (source && decodeUtf8Codepoint(source, strlen(source), &codepoint, &advance)) {
            fill = (int)codepoint;
            encoded_fill_len = encodeUtf8Codepoint(codepoint, encoded_fill);
            unicode_fill = true;
        }
    } else if (IS_INTLIKE(args[0])) {
        fill = (unsigned char)AS_INTEGER(args[0]);
        encoded_fill[0] = (char)fill;
        encoded_fill[1] = '\0';
        encoded_fill_len = 1;
    } else {
        runtimeError(vm, "StringOfChar expects a character-like first argument.");
        return makeString("");
    }

    long long count = AS_INTEGER(args[1]);
    if (count <= 0) {
        return unicode_fill ? makeUnicodeString("") : makeString("");
    }

    // A string's length is an int everywhere downstream (Length, indexing),
    // and len * encoded_fill_len must not wrap: StringOfChar(chr(128512),
    // 2^62 + 1) used to malloc 5 bytes and then fill 4 * (2^62 + 1).
    if (count > INT_MAX || (unicode_fill && (size_t)count > (size_t)INT_MAX / encoded_fill_len)) {
        runtimeError(vm, "StringOfChar: count %lld is too large.", count);
        return unicode_fill ? makeUnicodeString("") : makeString("");
    }
    size_t len = (size_t)count;
    size_t total_bytes = unicode_fill ? len * encoded_fill_len : len;

    // Fill the result's own buffer directly rather than building a temporary
    // and copying it again.
    Value result = unicode_fill ? makeUnicodeStringLen(NULL, 0) : makeStringLen(NULL, 0);
    char *buffer = (char*)malloc(total_bytes + 1);
    if (!buffer) {
        freeValue(&result);
        runtimeError(vm, "StringOfChar: memory allocation failed.");
        return makeString("");
    }

    if (unicode_fill) {
        char *cursor = buffer;
        for (size_t i = 0; i < len; i++) {
            memcpy(cursor, encoded_fill, encoded_fill_len);
            cursor += encoded_fill_len;
        }
    } else {
        memset(buffer, fill, len);
    }
    buffer[total_bytes] = '\0';

    free(AS_STRING(result));
    AS_STRING(result) = buffer;
    return result;
}

// Pure flat-offset arithmetic over raw bound arrays, used only by
// resizeDynamicArrayValue's old-storage-to-new-storage copy loop below --
// deliberately not routed through computeFlatOffset (which takes a Value*
// and expects a fully-formed ArrayObj) since this needs to compute offsets
// against two different bound-array pairs (old and new) that don't have
// their own Value/ArrayObj wrappers.
static int flatOffsetFromBounds(int dims, const int* lower, const int* upper, const int* indices) {
    int offset = 0;
    int multiplier = 1;
    for (int i = dims - 1; i >= 0; i--) {
        offset += (indices[i] - lower[i]) * multiplier;
        multiplier *= (upper[i] - lower[i] + 1);
    }
    return offset;
}

static bool resizeDynamicArrayValue(VM* vm,
                                    Value* array_value,
                                    int dimension_count,
                                    const long long* lengths) {
    if (!array_value || VALUE_TYPE(*array_value) != TYPE_ARRAY) {
        runtimeError(vm, "SetLength target is not an array.");
        return false;
    }
    if (dimension_count <= 0) {
        runtimeError(vm, "SetLength requires at least one dimension for arrays.");
        return false;
    }

    // VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core
    // f65432e): f65432e serialized this function's own release-old/
    // publish-new step (further down) against a concurrent READER's
    // copyDynamicArraySnapshotValue(), but left this function's own
    // initial read of *array_value's current ArrayObj -- used throughout
    // the rest of this function to read the array's existing
    // element_type/bounds/data for the resize -- unprotected against a
    // SECOND thread concurrently calling SetLength on the SAME array. If
    // that other thread's own publish step (the locked release+install
    // further down) runs while this thread is still reading `old`'s
    // fields, `old` can be freed out from under this thread: a genuine
    // use-after-free under concurrent-writer stress, not just the
    // reader/writer case f65432e covered.
    //
    // Retaining `old` here (matching copyDynamicArraySnapshotValue's own
    // retain-under-lock convention) gives this thread its OWN
    // independent reference, so `old` stays alive for the rest of this
    // function regardless of what a concurrent SetLength does to the
    // live cell. This retain is deliberately separate from -- and
    // released separately from -- the cell's own reference: the publish
    // step below no longer assumes the cell still holds `old` (another
    // thread may already have replaced it there), it re-reads the
    // cell's CURRENT content under lock and releases THAT, and only
    // then does this function release its own defensive retain of
    // `old`. Not a whole-function lock: the dimension_count > 1
    // recursive call further down only ever touches its own freshly
    // allocated, not-yet-published elements, never this outer
    // array_value cell, and by the time that recursion happens this
    // retain's own lock has long since been released -- so it cannot
    // deadlock against this (pthread_mutex_lock here is not reentrant).
    pthread_mutex_lock(&dynamic_array_refcount_mutex);
    ArrayObj* old = PSCAL_VALUE_PTR(*array_value, ArrayObj);
    if (old) pscalObjRetain(&old->header);
    pthread_mutex_unlock(&dynamic_array_refcount_mutex);
    if (!old) {
        runtimeError(vm, "SetLength target is not an array.");
        return false;
    }

    VarType element_type = old->element_type;
    AST* element_type_def = old->element_type_def;
    bool use_packed = isPackedByteElementType(element_type);
    bool is_dynamic = old->is_dynamic;

    // An array whose elements are arrays (`array of array of T`) is itself
    // one-dimensional: SetLength(a, n, m) gives `a` n elements and then sizes
    // each of them to m (the recursion further down). Shaping `a` as an n*m
    // 2-D array as well allocated n*m inner arrays (SetLength(a, 300, 300)
    // built 90,000 rows) and rejected re-dimensioning an array first sized
    // with SetLength(a, n).
    int own_dims = (element_type == TYPE_ARRAY && dimension_count > 1) ? 1 : dimension_count;

    if (old->dimensions > 0 && old->dimensions != own_dims) {
        runtimeError(vm, "SetLength dimension count (%d) does not match existing array (%d).",
                     own_dims,
                     old->dimensions);
        pscalObjRelease(&old->header);
        return false;
    }

    int* new_lower = (int*)malloc(sizeof(int) * own_dims);
    int* new_upper = (int*)malloc(sizeof(int) * own_dims);
    if (!new_lower || !new_upper) {
        if (new_lower) free(new_lower);
        if (new_upper) free(new_upper);
        runtimeError(vm, "SetLength: memory allocation failed for array bounds.");
        pscalObjRelease(&old->header);
        return false;
    }

    size_t new_total = 1;
    bool saw_zero = false;
    for (int i = 0; i < own_dims; ++i) {
        long long len = lengths[i];
        if (len < 0) {
            runtimeError(vm, "SetLength: array length must be non-negative.");
            free(new_lower);
            free(new_upper);
            pscalObjRelease(&old->header);
            return false;
        }
        if (len == 0) {
            new_lower[i] = 0;
            new_upper[i] = -1;
            saw_zero = true;
        } else {
            if (len > INT_MAX) {
                runtimeError(vm, "SetLength: array length exceeds supported range.");
                free(new_lower);
                free(new_upper);
                pscalObjRelease(&old->header);
                return false;
            }
            new_lower[i] = 0;
            new_upper[i] = (int)len - 1;
            if (!saw_zero) {
                if (new_total > SIZE_MAX / (size_t)len) {
                    runtimeError(vm, "SetLength: requested array size is too large.");
                    free(new_lower);
                    free(new_upper);
                    pscalObjRelease(&old->header);
                    return false;
                }
                new_total *= (size_t)len;
            }
        }
    }

    if (saw_zero) {
        new_total = 0;
    }

    size_t old_total = 0;
    int* old_lower_bounds = old->lower_bounds;
    int* old_upper_bounds = old->upper_bounds;
    Value* old_array_val = old->elements;
    unsigned char* old_array_raw = old->raw;
    bool old_array_is_packed = old->is_packed;
    if (old->dimensions > 0 && old_lower_bounds && old_upper_bounds) {
        old_total = 1;
        for (int i = 0; i < old->dimensions; ++i) {
            int span = old_upper_bounds[i] - old_lower_bounds[i] + 1;
            if (span <= 0) {
                old_total = 0;
                break;
            }
            old_total *= (size_t)span;
        }
    }

    // Amortized in-place growth fast path. The general path below always
    // publishes a brand-new ArrayObj sized to exactly new_total, deep-
    // copying (makeCopyOfValue) every surviving element and freshly
    // constructing (makeValueForType) every slot -- correct, but O(current
    // length) per call, which makes a tight `arr = arr + [x]` loop (lowered
    // by ast_parser.c's buildArrayAppend to setlength(arr, length(arr)+1))
    // O(n^2) over n appends. That is exactly deb.aether's parsePackages
    // hot loop (pkgs = pkgs + [pr.pkg], once per Packages-index stanza) --
    // confirmed via a synthetic 40k-append benchmark scaling ~4.8x when n
    // doubled, matching quadratic growth, not the O(1)-amortized append
    // the compiler-level self-reassignment lowering was meant to enable.
    //
    // When this ArrayObj is the SOLE owner (refcount==1 -- no other Value,
    // PointerObj, or in-flight copyDynamicArraySnapshotValue() snapshot
    // shares it) and this is a pure 1-D grow-or-noop (same lower bound,
    // new_total >= old_total), there is nothing to protect via copy-on-
    // write: mutate `old` directly, over-allocating capacity geometrically
    // (doubling) so most calls need no reallocation at all, turning n
    // one-at-a-time appends into O(n) total instead of O(n^2). Every slot
    // up to `capacity` (not just the logically-visible ones) is kept a
    // valid, freeable Value at all times -- arrayObjDestroy frees up to
    // max(capacity, logical total) accordingly (see types.h's ArrayObj
    // comment). Checked and mutated entirely under
    // dynamic_array_refcount_mutex, matching copyDynamicArraySnapshotValue's
    // own read-then-retain critical section, so a concurrent snapshot can't
    // observe a torn grow and can't race this thread into wrongly taking
    // the fast path on a suddenly-shared array.
    //
    // Falls through to the general path (unchanged) whenever any condition
    // doesn't hold -- multi-dimensional resizes, shrinks, view arrays, and
    // shared (aliased) arrays are all untouched by this fast path.
    if (dimension_count == 1 && !old->view_of && is_dynamic &&
        (old->dimensions == 0 || old->dimensions == 1) &&
        new_lower[0] == 0 &&
        (old_total == 0 || (old_lower_bounds && old_lower_bounds[0] == 0)) &&
        new_total >= old_total) {
        pthread_mutex_lock(&dynamic_array_refcount_mutex);
        // This function already took its own defensive retain on `old`
        // (top of function, before dimension/type validation) so a truly
        // sole-owned array -- nothing but the live Value cell holding it --
        // reads refcount==2 here (1 original + this function's own), not 1.
        bool sole_owner =
            atomic_load_explicit(&old->header.refcount, memory_order_acquire) == 2;
        bool grew_in_place = false;
        if (sole_owner) {
            size_t old_capacity = (size_t)((old->capacity > (int)old_total) ? old->capacity : (int)old_total);
            bool alloc_ok = true;
            if (new_total > old_capacity) {
                size_t new_capacity = (old_capacity == 0) ? 4 : old_capacity * 2;
                if (new_capacity < new_total) new_capacity = new_total;
                if (use_packed) {
                    unsigned char* grown = (unsigned char*)realloc(old->raw, new_capacity * sizeof(unsigned char));
                    if (grown) {
                        memset(grown + old_capacity, 0, (new_capacity - old_capacity) * sizeof(unsigned char));
                        old->raw = grown;
                        old->capacity = (int)new_capacity;
                    } else {
                        alloc_ok = false;
                    }
                } else {
                    Value* grown = (Value*)realloc(old->elements, new_capacity * sizeof(Value));
                    if (grown) {
                        old->elements = grown;
                        for (size_t i = old_capacity; i < new_capacity; ++i) {
                            old->elements[i] = makeValueForType(element_type, element_type_def, NULL);
                        }
                        old->capacity = (int)new_capacity;
                    } else {
                        alloc_ok = false;
                    }
                }
            } else if (old->capacity == 0 && old_total > 0) {
                old->capacity = (int)old_total;
            }
            if (alloc_ok) {
                if (old->dimensions == 0) {
                    old->lower_bounds = (int*)malloc(sizeof(int));
                    old->upper_bounds = (int*)malloc(sizeof(int));
                    if (!old->lower_bounds || !old->upper_bounds) {
                        alloc_ok = false;
                    } else {
                        old->dimensions = 1;
                        old->element_type = element_type;
                        old->element_type_def = element_type_def;
                        old->is_packed = use_packed;
                        old->lower_bounds[0] = 0;
                    }
                }
                if (alloc_ok) {
                    old->upper_bounds[0] = new_upper[0];
                    old->lower_bound = 0;
                    old->upper_bound = new_upper[0];
                    grew_in_place = true;
                }
            }
        }
        pthread_mutex_unlock(&dynamic_array_refcount_mutex);
        if (grew_in_place) {
            free(new_lower);
            free(new_upper);
            pscalObjRelease(&old->header);
            return true;
        }
        // sole_owner was false, or an allocation failed and left `old`
        // untouched (realloc(NULL-on-failure) never frees/moves the
        // original buffer) -- fall through to the general path below,
        // which still has valid old_array_val/old_array_raw/old_total
        // captured above and will handle this resize correctly, just
        // without this fast path's O(1)-amortized benefit for this call.
    }

    Value* new_elements = NULL;
    unsigned char* new_raw = NULL;
    int* copy_lower = NULL;
    int* copy_upper = NULL;
    int* copy_indices = NULL;
    if (new_total > 0) {
        if (use_packed) {
            new_raw = (unsigned char*)calloc(new_total, sizeof(unsigned char));
            if (!new_raw) {
                runtimeError(vm, "SetLength: memory allocation failed for packed array contents.");
                goto setlength_cleanup_failure;
            }
        } else {
            new_elements = (Value*)malloc(sizeof(Value) * new_total);
            if (!new_elements) {
                runtimeError(vm, "SetLength: memory allocation failed for array contents.");
                goto setlength_cleanup_failure;
            }

            for (size_t i = 0; i < new_total; ++i) {
                new_elements[i] = makeValueForType(element_type, element_type_def, NULL);
            }
        }

        if (old_total > 0 && old_lower_bounds && old_upper_bounds &&
            old->dimensions == own_dims &&
            ((use_packed && (old_array_raw || old_array_val)) ||
             (!use_packed && old_array_val))) {
            copy_lower = (int*)malloc(sizeof(int) * own_dims);
            copy_upper = (int*)malloc(sizeof(int) * own_dims);
            if (!copy_lower || !copy_upper) {
                runtimeError(vm, "SetLength: memory allocation failed while preserving array contents.");
                goto setlength_cleanup_failure;
            }

            bool has_overlap = true;
            for (int i = 0; i < own_dims; ++i) {
                int overlap_low = old_lower_bounds[i] > new_lower[i] ?
                                  old_lower_bounds[i] : new_lower[i];
                int overlap_high = old_upper_bounds[i] < new_upper[i] ?
                                   old_upper_bounds[i] : new_upper[i];
                if (overlap_high < overlap_low) {
                    has_overlap = false;
                    break;
                }
                copy_lower[i] = overlap_low;
                copy_upper[i] = overlap_high;
            }

            if (has_overlap) {
                copy_indices = (int*)malloc(sizeof(int) * own_dims);
                if (!copy_indices) {
                    runtimeError(vm, "SetLength: memory allocation failed while preserving array contents.");
                    goto setlength_cleanup_failure;
                }

                for (int i = 0; i < own_dims; ++i) {
                    copy_indices[i] = copy_lower[i];
                }

                while (true) {
                    int old_offset = flatOffsetFromBounds(own_dims, old_lower_bounds, old_upper_bounds, copy_indices);
                    int new_offset = flatOffsetFromBounds(own_dims, new_lower, new_upper, copy_indices);
                    if (use_packed) {
                        unsigned char byte = 0;
                        if (old_array_is_packed && old_array_raw) {
                            byte = old_array_raw[old_offset];
                        } else if (old_array_val) {
                            byte = valueToByte(&old_array_val[old_offset]);
                        }
                        new_raw[new_offset] = byte;
                    } else {
                        freeValue(&new_elements[new_offset]);
                        new_elements[new_offset] = makeCopyOfValue(&old_array_val[old_offset]);
                    }

                    int dim = own_dims - 1;
                    while (dim >= 0) {
                        if (copy_indices[dim] < copy_upper[dim]) {
                            copy_indices[dim]++;
                            break;
                        }
                        copy_indices[dim] = copy_lower[dim];
                        --dim;
                    }
                    if (dim < 0) {
                        break;
                    }
                }
            }
        }
    }

    if (element_type == TYPE_ARRAY && dimension_count > 1 && new_elements) {
        for (size_t i = 0; i < new_total; ++i) {
            if (!resizeDynamicArrayValue(vm, &new_elements[i], dimension_count - 1, lengths + 1)) {
                goto setlength_cleanup_failure;
            }
        }
    }

    if (copy_indices) {
        free(copy_indices);
        copy_indices = NULL;
    }
    if (copy_lower) {
        free(copy_lower);
        copy_lower = NULL;
    }
    if (copy_upper) {
        free(copy_upper);
        copy_upper = NULL;
    }

    // Always allocate a fresh ArrayObj wrapper rather than mutating `old`
    // in place: a dynamic array's ArrayObj can be shared (refcount > 1)
    // with other Values holding the SAME pointer, and mutating it in
    // place would make this SetLength visible through every alias instead
    // of just this one. pscalObjRelease below reproduces the pre-4e/4f
    // "decrement if shared, actually free storage if this was the last
    // owner" behavior without this function needing to know which case
    // applies -- see core/types.h's ArrayObj comment.
    ArrayObj* new_obj = pscalArrayObjCreate();
    new_obj->element_type = element_type;
    new_obj->element_type_def = element_type_def;
    new_obj->is_packed = use_packed;
    new_obj->is_dynamic = is_dynamic;
    new_obj->dimensions = own_dims;
    new_obj->lower_bounds = new_lower;
    new_obj->upper_bounds = new_upper;
    new_obj->lower_bound = (own_dims >= 1) ? new_lower[0] : 0;
    new_obj->upper_bound = (own_dims >= 1) ? new_upper[0] : -1;
    if (new_total > 0) {
        if (use_packed) {
            new_obj->raw = new_raw;
        } else {
            new_obj->elements = new_elements;
        }
    }

    // VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core
    // dbac64a): that fix serializes replaceValueCell()'s whole-value
    // reassignment against copyDynamicArraySnapshotValue()'s retain-under-
    // lock read, but SetLength's in-place resize publishes a fresh ArrayObj
    // through this exact same array_value cell via a completely different,
    // previously-unlocked path. Without the same two mutexes (same order:
    // value_cell_mutex, then dynamic_array_refcount_mutex, per
    // core/utils.h's dynamic_array_refcount_mutex comment), a concurrent
    // reader's snapshot/bounds read can retain `old` right as it's being
    // released here, or dereference it just after release frees its
    // storage -- both confirmed as real heap-corruption crashes under
    // concurrent SetLength()/read stress.
    //
    // f65432e follow-up (two-writer case): re-read the cell's CURRENT
    // ArrayObj here rather than assuming it is still `old` -- a second
    // thread racing this same SetLength() may already have published its
    // own new_obj into this cell before this thread got here. Releasing
    // whatever is actually installed right now (not necessarily `old`)
    // is what keeps the cell's own reference-count bookkeeping balanced
    // regardless of which thread's resize reaches this step first; this
    // function's own defensive retain of `old` (top of function) is then
    // released separately below, independent of that outcome.
    pthread_mutex_lock(&value_cell_mutex);
    pthread_mutex_lock(&dynamic_array_refcount_mutex);
    ArrayObj* current = PSCAL_VALUE_PTR(*array_value, ArrayObj);
    if (current) {
        pscalObjRelease(&current->header);
    }
    pscalValueSetHeapPtrBits(array_value, new_obj);
    pthread_mutex_unlock(&dynamic_array_refcount_mutex);
    pthread_mutex_unlock(&value_cell_mutex);
    pscalObjRelease(&old->header);

    return true;

setlength_cleanup_failure:
    if (copy_indices) {
        free(copy_indices);
    }
    if (copy_lower) {
        free(copy_lower);
    }
    if (copy_upper) {
        free(copy_upper);
    }
    if (new_elements) {
        for (size_t i = 0; i < new_total; ++i) {
            freeValue(&new_elements[i]);
        }
        free(new_elements);
    }
    if (new_raw) {
        free(new_raw);
    }
    free(new_lower);
    free(new_upper);
    pscalObjRelease(&old->header);
    return false;
}

Value vmBuiltinSetlength(VM* vm, int arg_count, Value* args) {
    if (arg_count < 2 || VALUE_TYPE(args[0]) != TYPE_POINTER) {
        runtimeError(vm, "SetLength expects a pointer target followed by length arguments.");
        return makeVoid();
    }

    Value* target = (Value*)AS_POINTER(args[0]);
    if (!target) {
        runtimeError(vm, "SetLength received a nil pointer.");
        return makeVoid();
    }

    if (VALUE_TYPE(*target) != TYPE_ARRAY) {
        if (arg_count != 2 || !IS_INTLIKE(args[1])) {
            runtimeError(vm, "SetLength expects (var string, integer).");
            return makeVoid();
        }

        long long new_len = AS_INTEGER(args[1]);
        if (new_len < 0) new_len = 0;

        if (!isPascalStringType(VALUE_TYPE(*target))) {
            runtimeError(vm, "SetLength expects a string or dynamic array target.");
            return makeVoid();
        }

        // The buffer is replaced inside the target's StringObj below, which
        // copies of the variable (and a string literal it was assigned from)
        // may share by reference count: unshare it first.
        valueEnsureUnique(target);

        size_t alloc_len = (size_t)new_len;
        size_t copy_len = 0;
        bool unicode_target = (VALUE_TYPE(*target) == TYPE_UNICODE_STRING);

        if (unicode_target) {
            const char *src = AS_STRING(*target) ? AS_STRING(*target) : "";
            size_t src_len = strlen(src);
            size_t src_cp_len = utf8CodepointCount(src, src_len);
            size_t copy_cp_len = src_cp_len;
            if (copy_cp_len > (size_t)new_len) {
                copy_cp_len = (size_t)new_len;
            }
            copy_len = utf8ByteOffsetForCodepointIndex(src, src_len, copy_cp_len);
            alloc_len = copy_len + (((size_t)new_len > copy_cp_len) ? ((size_t)new_len - copy_cp_len) : 0);
        }

        char* new_buf = (char*)malloc(alloc_len + 1);
        if (!new_buf) {
            runtimeError(vm, "SetLength: memory allocation failed.");
            return makeVoid();
        }

        if (AS_STRING(*target)) {
            if (!unicode_target) {
                copy_len = strlen(AS_STRING(*target));
                if (copy_len > (size_t)new_len) copy_len = (size_t)new_len;
            }
            memcpy(new_buf, AS_STRING(*target), copy_len);
            free(AS_STRING(*target));
        }

        if ((size_t)new_len > (unicode_target ? utf8CodepointCount(new_buf, copy_len) : copy_len)) {
            size_t fill_len = unicode_target
                ? ((size_t)new_len - utf8CodepointCount(new_buf, copy_len))
                : ((size_t)new_len - copy_len);
            memset(new_buf + copy_len, ' ', fill_len);
            copy_len += fill_len;
        }
        new_buf[copy_len] = '\0';

        AS_STRING(*target) = new_buf;
        STRING_MAX_LENGTH(*target) = -1;
        return makeVoid();
    }

    // VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core
    // f65432e / this fix): this back-fill (for a target whose element type
    // wasn't yet known, e.g. from an inferred/generic declaration) reads
    // *target's ArrayObj fields and, if still unset, mutates them in place
    // -- unprotected, both the read and the later mutate race a SECOND
    // thread's concurrent SetLength() on the SAME array publishing (and
    // freeing the old) ArrayObj via resizeDynamicArrayValue below, exactly
    // like that function's own now-fixed initial read did (confirmed via
    // Tests/vm_thread_stress/setlength_concurrent_writers_race.pas hitting
    // a use-after-free HERE, not in resizeDynamicArrayValue, under ASan).
    // Each of the two dereferences below re-fetches *target's CURRENT
    // ArrayObj fresh under dynamic_array_refcount_mutex rather than
    // trusting a pointer captured before or across makeValueForType()'s
    // (lock-free) work, so neither can observe a torn read or a freed
    // ArrayObj; value_cell_mutex is not needed here since this block only
    // ever mutates fields of whatever ArrayObj *target currently points
    // to, never swaps *target's own pointer bits the way
    // replaceValueCell/resizeDynamicArrayValue's publish step does.
    bool needs_type_backfill;
    {
        pthread_mutex_lock(&dynamic_array_refcount_mutex);
        ArrayObj *target_array = PSCAL_VALUE_PTR(*target, ArrayObj);
        needs_type_backfill = target_array &&
            (target_array->element_type == TYPE_UNKNOWN || target_array->element_type == TYPE_VOID ||
             target_array->element_type_def == NULL) &&
            PTR_BASE_TYPE_NODE(args[0]) != NULL;
        pthread_mutex_unlock(&dynamic_array_refcount_mutex);
    }
    if (needs_type_backfill) {
        Value typed_template = makeValueForType(TYPE_ARRAY, PTR_BASE_TYPE_NODE(args[0]), NULL);
        if (VALUE_TYPE(typed_template) == TYPE_ARRAY) {
            pthread_mutex_lock(&dynamic_array_refcount_mutex);
            ArrayObj *target_array = PSCAL_VALUE_PTR(*target, ArrayObj);
            if (target_array) {
                if (target_array->element_type == TYPE_UNKNOWN || target_array->element_type == TYPE_VOID) {
                    target_array->element_type = ARRAY_ELEMENT_TYPE(typed_template);
                }
                if (target_array->element_type_def == NULL) {
                    target_array->element_type_def = ARRAY_ELEMENT_TYPE_DEF(typed_template);
                }
                target_array->is_packed = ARRAY_IS_PACKED(typed_template);
            }
            pthread_mutex_unlock(&dynamic_array_refcount_mutex);
        }
        freeValue(&typed_template);
    }

    int dimension_count = arg_count - 1;
    long long* lengths = (long long*)malloc(sizeof(long long) * (size_t)dimension_count);
    if (!lengths) {
        runtimeError(vm, "SetLength: memory allocation failed.");
        return makeVoid();
    }

    for (int i = 0; i < dimension_count; ++i) {
        if (!IS_INTLIKE(args[i + 1])) {
            free(lengths);
            runtimeError(vm, "SetLength dimension arguments must be integers.");
            return makeVoid();
        }
        lengths[i] = AS_INTEGER(args[i + 1]);
    }

    if (!resizeDynamicArrayValue(vm, target, dimension_count, lengths)) {
        free(lengths);
        return makeVoid();
    }

    free(lengths);
    return makeVoid();
}

Value vmBuiltinRealtostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isRealType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "RealToStr expects 1 real argument.");
        return makeString("");
    }
    if (frontendIsAether()) {
        char buffer[128];
        pscalFormatRealRepr((double)AS_REAL(args[0]), buffer, sizeof(buffer)); /* D74 */
        return makeString(buffer);
    }
    // "%Lf" of a large real runs to hundreds of digits (1e300 is 308 of
    // them); a fixed 128-byte buffer silently cut it short.
    char *text = builtinAsprintf("%Lf", AS_REAL(args[0]));
    if (!text) {
        runtimeError(vm, "RealToStr failed to format value.");
        return makeString("");
    }
    Value result = makeString(text);
    free(text);
    return result;
}

static bool appendFormatResultChunk(char** buffer, size_t* length, size_t* capacity, const char* text) {
    if (!buffer || !length || !capacity || !text) {
        return false;
    }
    size_t add_len = strlen(text);
    size_t needed = *length + add_len + 1;
    if (needed > *capacity) {
        size_t new_capacity = (*capacity == 0) ? DEFAULT_STRING_CAPACITY : *capacity;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        char* resized = (char*)realloc(*buffer, new_capacity);
        if (!resized) {
            return false;
        }
        *buffer = resized;
        *capacity = new_capacity;
    }
    memcpy(*buffer + *length, text, add_len + 1);
    *length += add_len;
    return true;
}

Value vmBuiltinFormat(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "Format expects a format string followed by values.");
        return makeString("");
    }

    const char* fmt = AS_STRING(args[0]) ? AS_STRING(args[0]) : "";
    size_t flen = strlen(fmt);
    int arg_index = 1;
    char* result = NULL;
    size_t result_len = 0;
    size_t result_cap = 0;

    if (!appendFormatResultChunk(&result, &result_len, &result_cap, "")) {
        runtimeError(vm, "Format: memory allocation failed.");
        return makeString("");
    }

    for (size_t i = 0; i < flen; ++i) {
        if (fmt[i] != '%') {
            char literal[2] = { fmt[i], '\0' };
            if (!appendFormatResultChunk(&result, &result_len, &result_cap, literal)) {
                free(result);
                runtimeError(vm, "Format: memory allocation failed.");
                return makeString("");
            }
            continue;
        }

        if (i + 1 < flen && fmt[i + 1] == '%') {
            if (!appendFormatResultChunk(&result, &result_len, &result_cap, "%")) {
                free(result);
                runtimeError(vm, "Format: memory allocation failed.");
                return makeString("");
            }
            ++i;
            continue;
        }

        if (arg_index >= arg_count) {
            free(result);
            runtimeError(vm, "Format: not enough arguments for format string.");
            return makeString("");
        }

        size_t j = i + 1;
        char flags[16];
        size_t flag_len = 0;
        while (j < flen && strchr("-+ #0", fmt[j]) != NULL && flag_len + 1 < sizeof(flags)) {
            flags[flag_len++] = fmt[j++];
        }
        flags[flag_len] = '\0';

        bool width_specified = false;
        int width = 0;
        while (j < flen && isdigit((unsigned char)fmt[j])) {
            width_specified = true;
            width = builtinPrintfAccumDigit(width, fmt[j]);
            ++j;
        }

        int precision = -1;
        if (j < flen && fmt[j] == '.') {
            ++j;
            precision = 0;
            while (j < flen && isdigit((unsigned char)fmt[j])) {
                precision = builtinPrintfAccumDigit(precision, fmt[j]);
                ++j;
            }
        }

        // Length modifiers are accepted and skipped: the spec handed to the C
        // library is rebuilt below with the one matching the argument this
        // code passes (see builtinPrintfCLength).
        const char* length_mods = "hlLjzt";
        while (j < flen && strchr(length_mods, fmt[j]) != NULL) {
            ++j;
        }

        char spec = (j < flen) ? fmt[j] : '\0';
        char fmtbuf[64];
        {
            const char *c_length = builtinPrintfCLength(spec);
            int n = 0;
            if (width_specified && precision >= 0) {
                n = snprintf(fmtbuf, sizeof(fmtbuf), "%%%s%d.%d%s%c", flags, width, precision, c_length, spec);
            } else if (width_specified) {
                n = snprintf(fmtbuf, sizeof(fmtbuf), "%%%s%d%s%c", flags, width, c_length, spec);
            } else if (precision >= 0) {
                n = snprintf(fmtbuf, sizeof(fmtbuf), "%%%s.%d%s%c", flags, precision, c_length, spec);
            } else {
                n = snprintf(fmtbuf, sizeof(fmtbuf), "%%%s%s%c", flags, c_length, spec);
            }
            if (n < 0 || (size_t)n >= sizeof(fmtbuf)) {
                fmtbuf[0] = '\0';
            }
        }

        // Each piece is sized to its own result: a fixed 255-byte buffer cut
        // Format('%s', [s]) to 255 characters and truncated any wide field.
        char* piece = NULL;
        Value v = args[arg_index++];

        switch (spec) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                long long s = 0LL;
                if (isIntlikeType(VALUE_TYPE(v)) || VALUE_TYPE(v) == TYPE_BOOLEAN || VALUE_TYPE(v) == TYPE_CHAR || VALUE_TYPE(v) == TYPE_ENUM) {
                    s = AS_INTEGER(v);
                }
                bool is_unsigned = (spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X');
                if (is_unsigned) {
                    piece = builtinAsprintf(fmtbuf, (unsigned long long)s);
                } else {
                    piece = builtinAsprintf(fmtbuf, s);
                }
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                long double rv = isRealType(VALUE_TYPE(v)) ? AS_REAL(v) : (long double)AS_INTEGER(v);
                piece = builtinAsprintf(fmtbuf, rv);
                break;
            }
            case 'c': {
                char ch = (VALUE_TYPE(v) == TYPE_CHAR) ? AS_CHAR(v) : (char)AS_INTEGER(v);
                piece = builtinAsprintf(fmtbuf, (unsigned int)(unsigned char)ch);
                break;
            }
            case 's': {
                char char_text[8] = {0};
                const char* sv = "";
                if (isPascalStringType(VALUE_TYPE(v)) && AS_STRING(v)) {
                    sv = AS_STRING(v);
                } else if (VALUE_TYPE(v) == TYPE_CHAR) {
                    char_text[0] = (char)AS_CHAR(v);
                    sv = char_text;
                } else if (VALUE_TYPE(v) == TYPE_WIDECHAR) {
                    encodeUtf8Codepoint((uint32_t)AS_CHAR(v), char_text);
                    sv = char_text;
                }
                piece = builtinAsprintf(fmtbuf, sv);
                break;
            }
            default: {
                if (width_specified && precision >= 0) {
                    piece = builtinAsprintf("%*.*s", width, precision, "");
                } else if (width_specified) {
                    piece = builtinAsprintf("%*s", width, "");
                } else {
                    piece = builtinAsprintf("%s", "");
                }
                break;
            }
        }

        if (!piece) {
            free(result);
            runtimeError(vm, "Format: could not format argument %d.", arg_index - 1);
            return makeString("");
        }
        bool appended = appendFormatResultChunk(&result, &result_len, &result_cap, piece);
        free(piece);
        if (!appended) {
            free(result);
            runtimeError(vm, "Format: memory allocation failed.");
            return makeString("");
        }
        i = j;
    }

    Value out = makeString(result ? result : "");
    free(result);
    return out;
}

Value vmBuiltinFormatfloat(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2 || !IS_NUMERIC(args[0])) {
        runtimeError(vm, "FormatFloat expects (numeric [, integer precision]).");
        return makeString("");
    }

    long double value = isRealType(VALUE_TYPE(args[0])) ? AS_REAL(args[0]) : (long double)AS_INTEGER(args[0]);

    int precision = PASCAL_DEFAULT_FLOAT_PRECISION;
    if (arg_count == 2) {
        if (!IS_INTLIKE(args[1])) {
            runtimeError(vm, "FormatFloat precision must be an integer.");
            return makeString("");
        }
        long long requested = AS_INTEGER(args[1]);
        if (requested < 0) {
            requested = 0;
        } else if (requested > 18) {
            requested = 18;
        }
        precision = (int)requested;
    }

    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%dLf", precision);

    // Sized to the result: FormatFloat(1e200, 2) is 204 characters, which a
    // fixed 128-byte buffer turned into a runtime error.
    char *buffer = builtinAsprintf(fmt, value);
    if (!buffer) {
        runtimeError(vm, "FormatFloat failed to format value.");
        return makeString("");
    }

    Value result = makeString(buffer);
    free(buffer);
    return result;
}

Value vmBuiltinParamcount(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ParamCount expects 0 arguments.");
        return makeInt64(0);
    }
    /*
     * ParamCount should reflect the number of command line parameters as an
     * integer.  Since the VM now supports multiple integer widths, use the
     * widest standard signed integer to avoid inadvertent promotion to a
     * floating type or other category.
     */
    return makeInt64(gParamCount);
}

Value vmBuiltinParamstr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "ParamStr expects 1 integer argument.");
        return makeString("");
    }
    long long idx = AS_INTEGER(args[0]);
    if (idx < 0 || idx > gParamCount) {
         return makeString("");
    }
    if (idx == 0) {
        // ParamStr(0) returns the program name, which we don't store yet.
        return makeString("");
    }
    return makeString(gParamValues[idx -1]);
}

Value vmBuiltinWherex(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "WhereX expects 0 arguments.");
        return makeInt(1);
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(c - gWindowLeft + 1);
    }
    return makeInt(1); // Default on error
}

Value vmBuiltinWherey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "WhereY expects 0 arguments.");
        return makeInt(1);
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(r - gWindowTop + 1);
    }
    return makeInt(1); // Default on error
}

// --- Terminal helper for VM input routines ---
static struct termios vm_orig_termios;
static int vm_termios_saved = 0;
static pthread_mutex_t vm_term_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Thread_local int vm_raw_mode = 0;
static _Thread_local int vm_alt_screen_depth = 0; // Track nested alternate screen buffers

/* What the signal handlers put back, captured outside signal context. A
 * handler may only make async-signal-safe calls (tcsetattr, write, sigaction,
 * raise, _exit): the old one ran vmAtExitCleanup, which takes vm_term_mutex
 * (a deadlock if the signal lands while it is held), fcloses the fx journal
 * and calls getenv/fprintf. Set with vm_term_mutex held. */
static struct termios g_vm_signal_termios;
static volatile sig_atomic_t g_vm_signal_termios_valid = 0;

static void vmNoteSignalTermios(void) {
    if (!g_vm_signal_termios_valid) {
        g_vm_signal_termios = vm_orig_termios;
        atomic_signal_fence(memory_order_seq_cst);
        g_vm_signal_termios_valid = 1;
    }
}

typedef struct {
    char fg[32];
    char bg[32];
    int valid;
} VmColorState;

#define VM_COLOR_STACK_MAX 16
static _Thread_local VmColorState vm_color_stack[VM_COLOR_STACK_MAX];
static _Thread_local int vm_color_stack_depth = 0;
// _Atomic (not plain volatile sig_atomic_t): set from vmSignalHandler
// (actual SIGINT handler context, so only lock-free atomic ops are safe
// here) but also read/cleared from ordinary multi-threaded code
// (pscalRuntimeConsumeSigint(), called concurrently by TaskSpawn'd worker
// VMs) -- volatile alone only guarantees signal-safety within one thread,
// not cross-thread visibility/ordering. atomic_int is still async-signal-
// safe for a lock-free type (C11 7.17.4) while fixing the cross-thread race.
// Not static: the VM's instruction loop reads these three inline through
// runtime_signal_flags.h.
atomic_int g_vm_sigint_seen = 0;
static int g_vm_sigint_pipe[2] = {-1, -1};
static pthread_once_t g_vm_sigint_pipe_once = PTHREAD_ONCE_INIT;
atomic_bool g_vm_interrupt_broadcast = false;
atomic_bool g_vm_suspend_requested = false;
/* Set when a cursor-position probe (getCursorPosition) gave up before its
 * ESC[row;colR reply arrived, so the reply may still turn up in the input;
 * the next canonical read flushes pending input only in that case. */
static atomic_bool g_vm_dsr_reply_pending = false;
static void vmDrainSigintFd(int fd, bool is_host);

static void vmEnableRawMode(void); // Forward declaration
static void vmSetupTermHandlers(void);
static void vmRegisterRestoreHandlers(void);
static void vmPushColorState(void);
static void vmPopColorState(void);
static void vmRestoreColorState(void);
static int vmQueryColor(const char *query, char *dest, size_t dest_size);
static void vmAtExitCleanup(void);

static pthread_key_t vm_thread_cleanup_key;
static pthread_once_t vm_thread_cleanup_key_once = PTHREAD_ONCE_INIT;
static pthread_once_t vm_restore_once = PTHREAD_ONCE_INIT;

static void vmRestoreTerminal(void);

/* A thread that set up the terminal handlers is exiting. It undoes raw mode
 * it set itself (vm_raw_mode is per thread) and nothing else: this used to
 * run the whole exit cleanup, so any worker thread finishing ended the
 * process-wide fx record/replay session and took the terminal out of raw
 * mode under the main thread. The process-level cleanup belongs to
 * vmExitWithCleanup and the atexit handler. */
static void vmThreadCleanup(void *unused) {
    (void)unused;
    if (vm_raw_mode) {
        vmRestoreTerminal();
    }
}

static void vmCreateThreadKey(void) {
    pthread_key_create(&vm_thread_cleanup_key, vmThreadCleanup);
}

static int vmTcgetattr(int fd, struct termios *term) {
    int res;
#if defined(PSCAL_TARGET_IOS)
    if (term) {
#if defined(TIOCGETA)
        do {
            res = vprocIoctlShim(fd, TIOCGETA, term);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            return 0;
        }
#endif
#if defined(TCGETS)
        do {
            res = vprocIoctlShim(fd, TCGETS, term);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            return 0;
        }
#endif
        if (vprocSessionStdioFetchTermios(fd, term)) {
            return 0;
        }
    }
#endif
    do {
        res = tcgetattr(fd, term);
    } while (res < 0 && errno == EINTR);
    return res;
}

static int vmTcsetattr(int fd, int optional_actions, const struct termios *term) {
    int res;
#if defined(PSCAL_TARGET_IOS)
    int cmd = 0;
#if defined(TCSETS)
    switch (optional_actions) {
        case TCSANOW:
            cmd = TCSETS;
            break;
        case TCSADRAIN:
            cmd = TCSETSW;
            break;
        case TCSAFLUSH:
            cmd = TCSETSF;
            break;
        default:
            cmd = 0;
            break;
    }
#elif defined(TIOCSETA)
    switch (optional_actions) {
        case TCSANOW:
            cmd = TIOCSETA;
            break;
        case TCSADRAIN:
            cmd = TIOCSETAW;
            break;
        case TCSAFLUSH:
            cmd = TIOCSETAF;
            break;
        default:
            cmd = 0;
            break;
    }
#endif
    if (cmd != 0) {
        do {
            res = vprocIoctlShim(fd, (unsigned long)cmd, (void *)term);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            return 0;
        }
    }
    if (vprocSessionStdioApplyTermios(fd, optional_actions, term)) {
        return 0;
    }
#endif
    do {
        res = tcsetattr(fd, optional_actions, term);
    } while (res < 0 && errno == EINTR);
    return res;
}

static bool vmTermiosIsRaw(const struct termios *term) {
    if (!term) {
        return false;
    }
    return (term->c_lflag & (ICANON | ECHO)) == 0;
}

static bool vmTermiosDebugEnabled(void) {
    return (getenv("PSCALI_TOOL_DEBUG") != NULL) || (getenv("PSCALI_VPROC_DEBUG") != NULL);
}

static void vmLogTermios(const char *tag, const struct termios *term) {
    if (!vmTermiosDebugEnabled() || !tag || !term) {
        return;
    }
    fprintf(stderr,
            "[termios] %s lflag=0x%lx iflag=0x%lx oflag=0x%lx cflag=0x%lx vmin=%u vtime=%u verase=0x%02x raw=%d icanon=%d echo=%d icrnl=%d\n",
            tag,
            (unsigned long)term->c_lflag,
            (unsigned long)term->c_iflag,
            (unsigned long)term->c_oflag,
            (unsigned long)term->c_cflag,
            (unsigned int)term->c_cc[VMIN],
            (unsigned int)term->c_cc[VTIME],
            (unsigned int)term->c_cc[VERASE],
            vmTermiosIsRaw(term) ? 1 : 0,
            (term->c_lflag & ICANON) ? 1 : 0,
            (term->c_lflag & ECHO) ? 1 : 0,
            (term->c_iflag & ICRNL) ? 1 : 0);
}

static void vmRestoreTerminal(void) {
    pthread_mutex_lock(&vm_term_mutex);
    if (!vm_termios_saved) {
        if (vmTcgetattr(STDIN_FILENO, &vm_orig_termios) == 0) {
            vm_termios_saved = 1;
            vmNoteSignalTermios();
        }
    }

    if (vm_termios_saved) {
        if (vmTermiosDebugEnabled()) {
            vmLogTermios("restore target", &vm_orig_termios);
        }
        bool should_restore = vm_raw_mode;
        struct termios current;
        if (vmTcgetattr(STDIN_FILENO, &current) == 0) {
            vmLogTermios("restore current", &current);
            if ((current.c_lflag & (ICANON | ECHO)) != (vm_orig_termios.c_lflag & (ICANON | ECHO))) {
                should_restore = true;
            }
        } else if (vmTermiosDebugEnabled()) {
            fprintf(stderr, "[termios] restore get failed errno=%d\n", errno);
        }
        if (should_restore) {
            if (vmTermiosDebugEnabled()) {
                fprintf(stderr, "[termios] restore apply raw_mode=%d\n", vm_raw_mode);
            }
            if (vmTcsetattr(STDIN_FILENO, TCSANOW, &vm_orig_termios) == 0) {
                vm_raw_mode = 0;
                vmLogTermios("restore applied", &vm_orig_termios);
            } else if (vmTermiosDebugEnabled()) {
                fprintf(stderr, "[termios] restore set failed errno=%d\n", errno);
            }
        } else if (vmTermiosDebugEnabled()) {
            fprintf(stderr, "[termios] restore skipped raw_mode=%d\n", vm_raw_mode);
        }
    } else if (vmTermiosDebugEnabled()) {
        fprintf(stderr, "[termios] restore skipped (termios not saved)\n");
    }
    pthread_mutex_unlock(&vm_term_mutex);
}

// Query terminal for current color (OSC 10/11) and store result in dest
/* Wait up to the OSC-response timeout for stdin to become readable. Returns
 * false on timeout or error, meaning the terminal is not going to answer. */
static bool vmQueryColorWaitReadable(int fd) {
    const int timeout_ms = 500; /* matches the VTIME=5 intent above */
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0)
            return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
        if (rc == 0)
            return false; /* terminal did not answer */
        if (errno == EINTR)
            continue;
        return false;
    }
}

static int vmQueryColor(const char *query, char *dest, size_t dest_size) {
    struct termios oldt, raw;
    char buf[64];
    size_t i = 0;
    char ch;

    // The query goes out on stdout and the reply comes back on stdin, so both
    // must be the terminal; with stdout redirected the query would only land
    // in the output.
    if (!pscalRuntimeStdinIsInteractive() || !pscalRuntimeStdoutIsInteractive())
        return -1;

    if (vmTcgetattr(STDIN_FILENO, &oldt) < 0)
        return -1;

    raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 5; // 0.5s timeout

    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return -1;
    }

    if (write(STDOUT_FILENO, query, strlen(query)) == -1) {
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return -1;
    }

    /* VMIN=0/VTIME=5 bounds this read on a real tty, but stdin here can be a
     * vproc session PTY whose read shim waits on a condition variable and
     * ignores VMIN/VTIME. A terminal that never answers the OSC query -- the
     * app's hterm session, for one -- would block the program forever before
     * it executed a single statement. Gate every read on poll() so the wait is
     * bounded no matter which read path is underneath. */
    while (i < sizeof(buf) - 1) {
        if (!vmQueryColorWaitReadable(STDIN_FILENO))
            break;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0)
            break;
        if (ch == '\a')
            break; // BEL terminator
        if (ch == '\x1B') {
            if (!vmQueryColorWaitReadable(STDIN_FILENO))
                break;
            ssize_t n2 = read(STDIN_FILENO, &ch, 1);
            if (n2 <= 0)
                break;
            if (ch == '\\')
                break; // ESC \ terminator
            buf[i++] = '\x1B';
        }
        buf[i++] = ch;
    }
    buf[i] = '\0';

    vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    char *p = strchr(buf, ';');
    if (!p)
        return -1;
    p++;
    strncpy(dest, p, dest_size - 1);
    dest[dest_size - 1] = '\0';
    return 0;
}

// Save current terminal foreground and background colors
static void vmPushColorState(void) {
    if (vm_color_stack_depth >= VM_COLOR_STACK_MAX)
        return;
    VmColorState *cs = &vm_color_stack[vm_color_stack_depth];
    cs->valid = 0;
    if (vmQueryColor("\x1B]10;?\x07", cs->fg, sizeof(cs->fg)) == 0 &&
        vmQueryColor("\x1B]11;?\x07", cs->bg, sizeof(cs->bg)) == 0) {
        cs->valid = 1;
    }
    vm_color_stack_depth++;
}

static void vmPopColorState(void) {
    if (vm_color_stack_depth > 1)
        vm_color_stack_depth--;
}

// Restore terminal colors from the top of the stack
static void vmRestoreColorState(void) {
    if (vm_color_stack_depth == 0)
        return;
    VmColorState *cs = &vm_color_stack[vm_color_stack_depth - 1];
    if (!cs->valid)
        return;
    char seq[64];
    int len = snprintf(seq, sizeof(seq), "\x1B]10;%s\x07", cs->fg);
    if (len > 0) {
        if (write(STDOUT_FILENO, seq, len) != len) {
            perror("vmRestoreColorState: write fg");
        }
    }
    len = snprintf(seq, sizeof(seq), "\x1B]11;%s\x07", cs->bg);
    if (len > 0) {
        if (write(STDOUT_FILENO, seq, len) != len) {
            perror("vmRestoreColorState: write bg");
        }
    }
}

// atexit handler: restore terminal settings and ensure cursor visibility
static void vmAtExitCleanup(void) {
    pscalFxEndSession();
    vmRestoreTerminal();
    if (pscalRuntimeStdoutIsInteractive()) {
        const char show_cursor[] = "\x1B[?25h"; // Ensure cursor is visible
        if (write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1) != (ssize_t)(sizeof(show_cursor) - 1)) {
            perror("vmAtExitCleanup: write show_cursor");
        }
        if (vm_color_stack_depth > 0)
            vm_color_stack_depth = 1; // Restore base screen colors
        vmRestoreColorState();
    }
}

/* Put the terminal back the way the program found it, from signal context:
 * the saved termios and a visible cursor, nothing that is not
 * async-signal-safe. */
static void vmSignalRestoreTerminal(void) {
    if (g_vm_signal_termios_valid) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_vm_signal_termios);
    }
    if (isatty(STDOUT_FILENO)) {
        static const char show_cursor[] = "\x1B[?25h";
        (void)!write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
    }
}

// Signal handler to ensure terminal state is restored on interrupts.
static void vmSignalHandler(int signum) {
    if (signum == SIGINT) {
        atomic_store(&g_vm_sigint_seen, 1);
        if (g_vm_sigint_pipe[1] >= 0) {
            char c = 'i';
            (void)write(g_vm_sigint_pipe[1], &c, 1);
        }
        return;
    }
    vmSignalRestoreTerminal();
    _exit(128 + signum);
}

#if !defined(PSCAL_TARGET_IOS)
/* A crash (SIGSEGV, SIGBUS, SIGABRT) is not turned into a plain exit any
 * more: that hid it (exit status 139 with no core dump, and no report from
 * a sanitizer whose handler this one had replaced). The terminal is restored
 * and the signal goes on to whatever handled it before -- the default action
 * or a sanitizer -- with its original details: a hardware fault re-faults on
 * return, a sent signal is raised again. */
static struct sigaction g_vm_prev_segv_action;
static struct sigaction g_vm_prev_bus_action;
static struct sigaction g_vm_prev_abrt_action;

static void vmFatalSignalHandler(int signum, siginfo_t *info, void *context) {
    (void)context;
    vmSignalRestoreTerminal();
    const struct sigaction *prev = (signum == SIGSEGV) ? &g_vm_prev_segv_action
                                 : (signum == SIGBUS)  ? &g_vm_prev_bus_action
                                                       : &g_vm_prev_abrt_action;
    (void)sigaction(signum, prev, NULL);
#if defined(__APPLE__)
    /* Darwin fills in a SIGSEGV sent with kill() exactly like a fault
     * (si_code SEGV_ACCERR, si_pid 0), so the two cannot be told apart.
     * With the default action behind us, raising is right for both (a real
     * fault's raised signal is delivered at the faulting instruction as this
     * handler returns). A handler behind us (a sanitizer) needs the fault's
     * own details, so a fault is left to re-fault into it; a signal sent
     * from outside is then absorbed once. */
    (void)info;
    bool hardware_fault = (signum == SIGSEGV || signum == SIGBUS) &&
                          prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN;
#else
    bool hardware_fault = (signum == SIGSEGV || signum == SIGBUS) && info && info->si_code > 0;
#endif
    if (!hardware_fault) {
        (void)raise(signum);
    }
}
#endif

static void vmRegisterRestoreHandlers(void) {
    atexit(vmAtExitCleanup);
    struct sigaction sa;
    sa.sa_handler = vmSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#if !defined(PSCAL_TARGET_IOS)
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    struct sigaction fatal;
    memset(&fatal, 0, sizeof(fatal));
    fatal.sa_sigaction = vmFatalSignalHandler;
    sigemptyset(&fatal.sa_mask);
    // SA_ONSTACK: a stack overflow can only be handled on an alternate
    // signal stack (a sanitizer installs one).
    fatal.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &fatal, &g_vm_prev_segv_action);
    sigaction(SIGBUS, &fatal, &g_vm_prev_bus_action);
    sigaction(SIGABRT, &fatal, &g_vm_prev_abrt_action);
#endif
}

static void vmSetupTermHandlers(void) {
    pthread_once(&vm_thread_cleanup_key_once, vmCreateThreadKey);
    pthread_setspecific(vm_thread_cleanup_key, (void *)1);

    pthread_mutex_lock(&vm_term_mutex);
    if (!vm_termios_saved) {
        if (vmTcgetattr(STDIN_FILENO, &vm_orig_termios) == 0) {
            vm_termios_saved = 1;
            vmNoteSignalTermios();
        }
    }
    pthread_mutex_unlock(&vm_term_mutex);

    pthread_once(&vm_restore_once, vmRegisterRestoreHandlers);
}

static void init_pipe_once(void);

static void vmEnsureSigintPipe(void) {
    pthread_once(&g_vm_sigint_pipe_once, init_pipe_once);
}

static void init_pipe_once(void) {
#if defined(PSCAL_TARGET_IOS)
    int host_pipe[2] = {-1, -1};
    if (vprocHostPipe(host_pipe) != 0) {
        g_vm_sigint_pipe[0] = -1;
        g_vm_sigint_pipe[1] = -1;
        return;
    }
    g_vm_sigint_pipe[0] = host_pipe[0];
    g_vm_sigint_pipe[1] = host_pipe[1];
#else
    if (pipe(g_vm_sigint_pipe) != 0) {
        g_vm_sigint_pipe[0] = -1;
        g_vm_sigint_pipe[1] = -1;
        return;
    }
#endif
    for (int i = 0; i < 2; ++i) {
        int target_fd = g_vm_sigint_pipe[i];
        fcntl(target_fd, F_SETFD, FD_CLOEXEC);
        int flags = fcntl(target_fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(target_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

static void vmDrainSigintFd(int fd, bool is_host) {
    if (fd < 0) {
        return;
    }
    // Force non-blocking to avoid wedging on drained host pipes.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && !(flags & O_NONBLOCK)) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    while (true) {
        int pending = 0;
        if (ioctl(fd, FIONREAD, &pending) != 0) {
            pending = 0;
        }
        char drain[64];
        size_t want = (pending > (int)sizeof(drain)) ? sizeof(drain)
                                                     : (pending > 0 ? (size_t)pending : sizeof(drain));
#if defined(PSCAL_TARGET_IOS)
        ssize_t n = is_host ? vprocHostRead(fd, drain, want)
                            : read(fd, drain, want);
#else
        (void)is_host;
        ssize_t n = read(fd, drain, want);
#endif
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            break; // pipe closed
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

// Exposed for platform bridges to request an interrupt (e.g., hardware Ctrl-C on iOS)
void pscalRuntimeRequestSigint(void) {
#if defined(PSCAL_TARGET_IOS)
    bool dbg = (getenv("PSCALI_TOOL_DEBUG") != NULL) || (getenv("PSCALI_VPROC_DEBUG") != NULL);
    bool from_vproc = (vprocCurrent() != NULL);
    int shell_pid = -1;
    int shell_pgid = -1;
    int sid = -1;
    int fg_pgid = -1;
    (void)vprocGetShellJobControlState(&shell_pid, &shell_pgid, &sid, &fg_pgid);
    bool delivered_to_foreground = false;
    if (!from_vproc && shell_pid > 0 && fg_pgid > 0 && shell_pgid > 0 && fg_pgid != shell_pgid) {
        /* Foreground belongs to a non-shell job (e.g. ssh). Route Ctrl-C as a
         * virtual process-group signal instead of broadcasting a shell/VM-wide
         * interrupt request. */
        errno = 0;
        int rc = vprocKillShim(-fg_pgid, SIGINT);
        int kill_errno = errno;
        if (rc == 0) {
            delivered_to_foreground = true;
        }
        if (dbg) {
            fprintf(stderr, "[sigint] routed-to-fg shell=%d shell_pgid=%d sid=%d fg=%d rc=%d errno=%d\n",
                    shell_pid, shell_pgid, sid, fg_pgid, rc, kill_errno);
        }
    }
    if (dbg && shell_pid > 0) {
        fprintf(stderr,
                "[sigint] shell=%d shell_pgid=%d sid=%d fg=%d delivered_fg=%d from_vproc=%d\n",
                shell_pid, shell_pgid, sid, fg_pgid, delivered_to_foreground ? 1 : 0, from_vproc ? 1 : 0);
    }
    if (delivered_to_foreground) {
        return;
    }
#endif
    vmEnsureSigintPipe();
    atomic_store(&g_vm_sigint_seen, 1);
    atomic_store(&g_vm_interrupt_broadcast, true);
    if (g_vm_sigint_pipe[1] >= 0) {
        char c = 'i';
#if defined(PSCAL_TARGET_IOS)
        (void)vprocHostWrite(g_vm_sigint_pipe[1], &c, 1);
#else
        (void)write(g_vm_sigint_pipe[1], &c, 1);
#endif
    }
}

void pscalRuntimeRequestSigtstp(void) {
#if defined(PSCAL_TARGET_IOS)
    bool dbg = (getenv("PSCALI_TOOL_DEBUG") != NULL) || (getenv("PSCALI_VPROC_DEBUG") != NULL);
    int shell_pid = -1;
    int shell_pgid = -1;
    int sid = -1;
    int fg_pgid = -1;
    (void)vprocGetShellJobControlState(&shell_pid, &shell_pgid, &sid, &fg_pgid);
    bool delivered = false;
    if (shell_pid <= 0) {
        if (dbg) {
            fprintf(stderr, "[sigtstp] no shell pid\n");
        }
    } else if (fg_pgid <= 0) {
        if (dbg) {
            fprintf(stderr, "[sigtstp] no fg pgid shell=%d sid=%d\n", shell_pid, sid);
        }
    } else if (shell_pgid > 0 && fg_pgid != shell_pgid) {
        if (dbg) {
            fprintf(stderr, "[sigtstp] shell=%d shell_pgid=%d sid=%d fg=%d\n",
                    shell_pid, shell_pgid, sid, fg_pgid);
        }
        errno = 0;
        int rc = vprocKillShim(-fg_pgid, SIGTSTP);
        int kill_errno = errno;
        if (rc == 0) {
            delivered = true;
        }
        if (dbg) {
            fprintf(stderr, "[sigtstp] kill rc=%d errno=%d\n", rc, kill_errno);
        }
    } else if (dbg) {
        fprintf(stderr, "[sigtstp] local suspend shell_pgid=%d fg=%d sid=%d\n",
                shell_pgid, fg_pgid, sid);
    }

    /* When the shell owns the foreground, still try virtual SIGTSTP delivery
     * first so foreground vprocs can enter a stopped state instead of forcing
     * a VM-wide suspend request. */
    if (!delivered && shell_pgid > 0) {
        errno = 0;
        int rc = vprocKillShim(-shell_pgid, SIGTSTP);
        int kill_errno = errno;
        if (rc == 0) {
            delivered = true;
        }
        if (dbg) {
            fprintf(stderr, "[sigtstp] shell-pgid rc=%d errno=%d\n", rc, kill_errno);
        }
    }

    if (!delivered) {
        atomic_store(&g_vm_suspend_requested, true);
    }
#else
    raise(SIGTSTP);
#endif
}

int pscalRuntimeCurrentForegroundPgid(void) {
#if defined(PSCAL_TARGET_IOS)
    int fg_pgid = -1;
    if (vprocGetShellJobControlState(NULL, NULL, NULL, &fg_pgid) && fg_pgid > 0) {
        return fg_pgid;
    }
#endif
    return -1;
}

bool pscalRuntimeSigintPending(void) {
    if (atomic_load(&g_vm_sigint_seen) != 0) {
        return true;
    }
    if (atomic_load(&g_vm_interrupt_broadcast)) {
        return true;
    }
    return false;
}

bool pscalRuntimeSigtstpPending(void) {
    return atomic_load(&g_vm_suspend_requested);
}

bool pscalRuntimeInterruptFlag(void) {
    return atomic_load(&g_vm_interrupt_broadcast);
}

void pscalRuntimeClearInterruptFlag(void) {
    atomic_store(&g_vm_interrupt_broadcast, false);
}

bool pscalRuntimeConsumeSigint(void) {
    vmEnsureSigintPipe();
    bool seen = atomic_exchange(&g_vm_sigint_seen, 0) != 0;
    int fd = g_vm_sigint_pipe[0];
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0 && !(flags & O_NONBLOCK)) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        char drain[64];
        while (true) {
            ssize_t n;
#if defined(PSCAL_TARGET_IOS)
            n = vprocHostRead(fd, drain, sizeof(drain));
#else
            n = read(fd, drain, sizeof(drain));
#endif
            if (n > 0) {
                seen = true;
                continue;
            }
            if (n == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
    }
    if (seen) {
        atomic_store(&g_vm_interrupt_broadcast, false);
    }
    return seen;
}

bool pscalRuntimeConsumeSigtstp(void) {
    return atomic_exchange(&g_vm_suspend_requested, false);
}

void vmInitTerminalState(void) {
    vmSetupTermHandlers();
    // The terminal's colours are no longer queried here: the OSC 10/11 query
    // waited up to half a second per colour on a terminal that does not
    // answer, and read away whatever input was already typed ahead -- on
    // every Pascal start, for programs that never touch the screen. Only the
    // screen stack needs them; PushScreen saves them on first use.
    vmEnableRawMode();
    if (isatty(STDOUT_FILENO)) {
        setvbuf(stdout, NULL, _IOFBF, 65536);
    }
}

/*
// Pause to allow the user to read messages before the VM exits.  The pause
// occurs before any terminal cleanup is performed.
void vmPauseBeforeExit(void) {
    // Only pause when running interactively.
    if (!pscalRuntimeStdinIsInteractive() || !pscalRuntimeStdoutIsInteractive())
        return;

    sleep(10);

    fprintf(stderr, "Press any key to exit");
    fflush(stderr);

    tcflush(STDIN_FILENO, TCIFLUSH); // Discard any pending input
    vmEnableRawMode();               // Ensure we can read single key presses
    const char show_cursor[] = "\x1B[?25h";
    if (write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1) != (ssize_t)(sizeof(show_cursor) - 1)) {
        perror("vmPrepareCanonicalInput: write show_cursor");
    }

    char ch;
    while (read(STDIN_FILENO, &ch, 1) < 0) {
        if (errno != EINTR) {
            break; // Ignore other read errors and continue with cleanup
        }
    }
}
 */

int vmExitWithCleanup(int status) {
    // vmPauseBeforeExit();
    vmAtExitCleanup();
    return status;
}

static void vmEnableRawMode(void) {
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    if (session_stdio && vprocSessionStdioIsDefault(session_stdio) &&
        PSCALRuntimeGetCurrentRuntimeStdio) {
        VProcSessionStdio *runtime_stdio = PSCALRuntimeGetCurrentRuntimeStdio();
        if (runtime_stdio && !vprocSessionStdioIsDefault(runtime_stdio)) {
            vprocSessionStdioActivate(runtime_stdio);
        }
    }
#endif
    vmSetupTermHandlers();
    pthread_mutex_lock(&vm_term_mutex);
    if (vm_raw_mode) {
        struct termios current;
        if (vmTcgetattr(STDIN_FILENO, &current) == 0 && vmTermiosIsRaw(&current)) {
            if (vmTermiosDebugEnabled()) {
                vmLogTermios("raw already", &current);
            }
            pthread_mutex_unlock(&vm_term_mutex);
            return;
        }
    }

    if (!vm_termios_saved) {
        if (vmTcgetattr(STDIN_FILENO, &vm_orig_termios) != 0) {
            if (vmTermiosDebugEnabled()) {
                fprintf(stderr, "[termios] raw get failed errno=%d\n", errno);
            }
            pthread_mutex_unlock(&vm_term_mutex);
            return;
        }
        vm_termios_saved = 1;
        vmNoteSignalTermios();
    }
    vmLogTermios("raw base", &vm_orig_termios);

    struct termios raw = vm_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        vm_raw_mode = 1;
        vmLogTermios("raw applied", &raw);
    } else if (vmTermiosDebugEnabled()) {
        fprintf(stderr, "[termios] raw set failed errno=%d\n", errno);
    }
    pthread_mutex_unlock(&vm_term_mutex);
}

// Restore the terminal to a canonical, line-buffered state suitable for
// Read()/ReadLn().  This undoes any prior ReadKey-induced raw mode,
// discards leftover input, ensures echoing, and makes the cursor visible.
static void vmPrepareCanonicalInput(void) {
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    if (session_stdio && vprocSessionStdioIsDefault(session_stdio) &&
        PSCALRuntimeGetCurrentRuntimeStdio) {
        VProcSessionStdio *runtime_stdio = PSCALRuntimeGetCurrentRuntimeStdio();
        if (runtime_stdio && !vprocSessionStdioIsDefault(runtime_stdio)) {
            vprocSessionStdioActivate(runtime_stdio);
        }
    }
#endif
    vmRestoreTerminal();
    pthread_mutex_lock(&vm_term_mutex);
    struct termios term;
    if (vmTcgetattr(STDIN_FILENO, &term) == 0) {
        vmLogTermios("canon before", &term);
        bool changed = false;
        if ((term.c_lflag & (ICANON | ECHO)) != (ICANON | ECHO)) {
            term.c_lflag |= (ICANON | ECHO);
            changed = true;
        }
#if defined(PSCAL_TARGET_IOS)
        /* Keep control keys byte-oriented for virtual signal handling.
         * With ISIG enabled, the tty layer can consume VSUSP/VINTR before
         * vmReadLineInterruptible sees ^Z/^C bytes. */
        if (term.c_lflag & ISIG) {
            term.c_lflag &= ~ISIG;
            changed = true;
        }
#endif
        if ((term.c_iflag & ICRNL) == 0) {
            term.c_iflag |= ICRNL;
            changed = true;
        }
        if (term.c_iflag & IGNCR) {
            term.c_iflag &= ~IGNCR;
            changed = true;
        }
        if (term.c_iflag & INLCR) {
            term.c_iflag &= ~INLCR;
            changed = true;
        }
        if (term.c_cc[VERASE] != 0x7f) {
            term.c_cc[VERASE] = 0x7f;
            changed = true;
        }
        if (term.c_cc[VMIN] != 1 || term.c_cc[VTIME] != 0) {
            term.c_cc[VMIN] = 1;
            term.c_cc[VTIME] = 0;
            changed = true;
        }
        if (changed) {
            if (vmTcsetattr(STDIN_FILENO, TCSANOW, &term) == 0) {
                vm_raw_mode = 0;
                vmLogTermios("canon after", &term);
            } else if (vmTermiosDebugEnabled()) {
                fprintf(stderr, "[termios] canon set failed errno=%d\n", errno);
            }
        } else if (vmTermiosDebugEnabled()) {
            fprintf(stderr, "[termios] canon unchanged raw_mode=%d\n", vm_raw_mode);
        }
    } else if (vmTermiosDebugEnabled()) {
        fprintf(stderr, "[termios] canon get failed errno=%d\n", errno);
    }
    pthread_mutex_unlock(&vm_term_mutex);
    // Flushing pending input here on every read threw away whatever the user
    // had typed ahead: pasting three lines into three readln calls kept only
    // some of them. The flush is only for a late reply to a cursor-position
    // probe, so it happens only while one may be outstanding; a reply that
    // still gets through is swallowed by vmReadLineInterruptible's DSR filter.
    if (atomic_exchange(&g_vm_dsr_reply_pending, false)) {
        tcflush(STDIN_FILENO, TCIFLUSH);
    }
    // The cursor escape is for a terminal. Written into a pipe or a file it
    // becomes stray bytes in the program's output (every stdin readln added
    // ESC[?25h ahead of the next line), so it goes out only when stdout is
    // interactive -- the same gate vmAtExitCleanup uses, which also covers
    // the iOS session stdio where fd 1 is not a plain tty.
    if (pscalRuntimeStdoutIsInteractive()) {
        const char show_cursor[] = "\x1B[?25h";
        if (write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1) != (ssize_t)(sizeof(show_cursor) - 1)) {
            perror("vmPrepareCanonicalInput: write show_cursor");
        }
    }
    fflush(stdout);
}

static bool vmBufferIsPureDsr(const char *buf, size_t len);

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} VmLineBuf;

#if defined(PSCAL_TARGET_IOS)
typedef void (*VmSuspendWaitHookFn)(void);
static __thread VmSuspendWaitHookFn gVmSuspendBeforeWaitHook = NULL;
static __thread VmSuspendWaitHookFn gVmSuspendAfterWaitHook = NULL;

void vmSetSuspendWaitHooks(VmSuspendWaitHookFn before_hook, VmSuspendWaitHookFn after_hook) {
    gVmSuspendBeforeWaitHook = before_hook;
    gVmSuspendAfterWaitHook = after_hook;
}

static bool vmShouldUseCooperativeSuspend(void) {
    if (vprocIsShellSelfThread()) {
        return true;
    }
    int pid = vprocGetPidShim();
    int shell_pid = vprocGetShellSelfPid();
    if (pid <= 0) {
        pid = shell_pid;
    }
    if (pid <= 0) {
        return true;
    }
    if (shell_pid > 0 && pid == shell_pid) {
        return true;
    }
    return vprocGetStopUnsupported(pid);
}

static int vmSuspendReleaseGlobalsMutex(void) {
    int released = 0;
    /*
     * Frontend threads can be suspended while inside builtins that hold the
     * recursive globals mutex. Release any ownership before parking so the
     * shell thread can keep running while the frontend is stopped.
     */
    while (pthread_mutex_unlock(&globals_mutex) == 0) {
        released++;
    }
    return released;
}

static void vmSuspendReacquireGlobalsMutex(int count) {
    for (int i = 0; i < count; ++i) {
        pthread_mutex_lock(&globals_mutex);
    }
}

static void vmWaitIfStoppedWithCheckpoint(void) {
    int released_globals_locks = vmSuspendReleaseGlobalsMutex();
    if (gVmSuspendBeforeWaitHook) {
        gVmSuspendBeforeWaitHook();
    }
    (void)vprocWaitIfStopped(vprocCurrent());
    if (gVmSuspendAfterWaitHook) {
        gVmSuspendAfterWaitHook();
    }
    if (released_globals_locks > 0) {
        vmSuspendReacquireGlobalsMutex(released_globals_locks);
    }
}

static bool vmRequestHardSuspendCurrentVproc(void) {
    int pid = vprocGetPidShim();
    int shell_pid = vprocGetShellSelfPid();
    if (pid <= 0) {
        pid = shell_pid;
    }
    int pgid = (pid > 0) ? vprocGetPgid(pid) : -1;

    /*
     * For non-shell foreground workers, group-directed delivery can be
     * downgraded to a cooperative pending signal on the current thread.
     * Prefer direct pid delivery first so Ctrl-Z results in a real stoppage
     * that fg/bg can later resume.
     */
    if (pid > 0 && vprocKillShim(pid, SIGTSTP) == 0) {
        return true;
    }
    if (pgid > 0 && vprocKillShim(-pgid, SIGTSTP) == 0) {
        return true;
    }
    if (vprocRequestControlSignal(SIGTSTP)) {
        return true;
    }
    return false;
}

static bool vmHandleCtrlZReadRequest(VM *vm, char *buffer) {
    if (getenv("PSCALI_TOOL_DEBUG")) {
        int pid_dbg = vprocGetPidShim();
        int shell_pid_dbg = vprocGetShellSelfPid();
        bool self_thread_dbg = vprocIsShellSelfThread();
        bool stop_unsupported_dbg = (pid_dbg > 0) ? vprocGetStopUnsupported(pid_dbg) : false;
        fprintf(stderr,
                "[ctrlz] pid=%d shell=%d self_thread=%d stop_unsupported=%d coop=%d\n",
                pid_dbg,
                shell_pid_dbg,
                (int)self_thread_dbg,
                (int)stop_unsupported_dbg,
                (int)vmShouldUseCooperativeSuspend());
    }
    if (!vmShouldUseCooperativeSuspend()) {
        if (vmRequestHardSuspendCurrentVproc()) {
            if (getenv("PSCALI_TOOL_DEBUG")) {
                fprintf(stderr, "[ctrlz] hard-stop wait begin\n");
            }
            vmWaitIfStoppedWithCheckpoint();
            if (getenv("PSCALI_TOOL_DEBUG")) {
                fprintf(stderr, "[ctrlz] hard-stop wait end\n");
            }
            return true;
        }
    }
    if (vm) {
        vm->abort_requested = false;
        vm->exit_requested = true;
        vm->suspend_unwind_requested = true;
    }
    shellRuntimeSetLastStatus(128 + SIGTSTP);
    if (buffer) {
        buffer[0] = '\0';
    }
    return false;
}
#endif

/* Make room for at least `need` bytes in `line`. */
static bool vmLineBufReserve(VmLineBuf *line, size_t need) {
    if (line->data && line->cap >= need) {
        return true;
    }
    size_t cap = line->cap ? line->cap : 128;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return false;
        }
        cap *= 2;
    }
    char *grown = (char *)realloc(line->data, cap);
    if (!grown) {
        return false;
    }
    line->data = grown;
    line->cap = cap;
    return true;
}

/* Read one line from `stream` into `line` (grown as needed, NUL-terminated,
 * line->len set; the caller frees line->data). The line break is not
 * stored. A line has no length limit: the fixed buffers this used to fill
 * (1024 bytes for readln, 1 MiB for read_line) returned a longer line as
 * several lines. */
static bool vmReadLineInterruptible(VM *vm, FILE *stream, VmLineBuf *line) {
    if (!stream || !line || !vmLineBufReserve(line, 128)) {
        return false;
    }
    line->len = 0;
    line->data[0] = '\0';
    char *buffer = line->data;
    int fd = fileno(stream);
    bool is_stdin_stream = vmStreamTargetsSharedStdin(stream);
#if defined(PSCAL_TARGET_IOS)
    bool is_shared_stdin_stream = is_stdin_stream;
    bool tool_dbg = getenv("PSCALI_TOOL_DEBUG") != NULL;
    if (is_shared_stdin_stream) {
        fd = STDIN_FILENO;
    } else if (fd < 0 && is_stdin_stream) {
        fd = STDIN_FILENO;
    }
#else
    bool tool_dbg = false;
#endif
    if (fd < 0) {
        return false;
    }

    size_t len = 0;
    vmEnsureSigintPipe();
    bool use_interruptible = (is_stdin_stream && pscalRuntimeStdinIsInteractive());
#if defined(PSCAL_TARGET_IOS)
    if (is_stdin_stream) {
        use_interruptible = true;
    }
    bool use_session_read = false;
    VProcSessionStdio *session_stdio = NULL;
    if (is_stdin_stream) {
        session_stdio = vprocSessionStdioCurrent();
        if (session_stdio && vprocSessionStdioIsDefault(session_stdio) &&
            PSCALRuntimeGetCurrentRuntimeStdio) {
            VProcSessionStdio *runtime_stdio = PSCALRuntimeGetCurrentRuntimeStdio();
            if (runtime_stdio && !vprocSessionStdioIsDefault(runtime_stdio)) {
                session_stdio = runtime_stdio;
                vprocSessionStdioActivate(session_stdio);
            }
        }
        if (session_stdio) {
            bool has_host_stdin = (session_stdio->stdin_host_fd >= 0);
            bool has_pscal_stdin = (session_stdio->stdin_pscal_fd != NULL);
            bool has_session_input = (session_stdio->input != NULL);
            bool session_backed_input = has_host_stdin || has_pscal_stdin || has_session_input;
            /* Shared stdin streams in the iOS runtime should consume input
             * from the per-session buffer when they are attached to a real
             * runtime session, even if isatty(0) is false on the current
             * worker thread. Frontend tools launched from applets like time
             * inherit the runtime context but may not present fd 0 as a tty
             * through the active VProc mapping, which leaves ReadLn blocked in
             * select/poll after the user has already typed a line. */
            if (is_shared_stdin_stream &&
                session_backed_input &&
                (!vprocSessionStdioIsDefault(session_stdio) ||
                 pscalRuntimeStdinIsInteractive())) {
                use_session_read = true;
            } else if (!vprocSessionStdioIsDefault(session_stdio) &&
                       has_host_stdin &&
                       !has_pscal_stdin) {
                use_session_read = true;
            }
        }
    }
    if (tool_dbg && is_stdin_stream) {
        fprintf(stderr,
                "[readln] init fd=%d use_session=%d use_interruptible=%d session=%p host=%d pscal=%p\n",
                fd,
                (int)use_session_read,
                (int)use_interruptible,
                (void *)session_stdio,
                session_stdio ? session_stdio->stdin_host_fd : -1,
                session_stdio ? (void *)session_stdio->stdin_pscal_fd : NULL);
    }
#endif
    if (use_interruptible) {
#if defined(PSCAL_TARGET_IOS)
        vmEnsureSigintPipe();
#endif
#if defined(PSCAL_TARGET_IOS)
        int read_fd = fd;
        VProc *vp = vprocCurrent();
        bool read_is_host = (vp == NULL);
        int host_fd = -1;
        if (vp) {
            host_fd = vprocTranslateFd(vp, fd);
            if (host_fd >= 0) {
                read_fd = host_fd;
                read_is_host = true;
            } else {
                read_is_host = false;
            }
        }
        if (tool_dbg && is_stdin_stream) {
            fprintf(stderr,
                    "[readln] start stdin fd=%d read_fd=%d host=%d host_fd=%d\n",
                    fd, read_fd, (int)read_is_host, host_fd);
        }
#else
        int read_fd = fd;
#endif
        int sigint_fd = g_vm_sigint_pipe[0];
        bool saw_newline = false;
#if defined(PSCAL_TARGET_IOS)
        bool pending_caret_control = false;
#endif
        for (;;) {
            /* Room for the most one pass of this loop can store: one byte,
             * a caret pair, or a pushed-back escape sequence (< 32 bytes). */
            if (!vmLineBufReserve(line, len + 64)) {
                break;
            }
            buffer = line->data;
#if defined(PSCAL_TARGET_IOS)
            /* If a frontend thread was hard-stopped while blocked in select/poll,
             * convert that stop into a cooperative pending signal so ReadLn can
             * unwind back to the shell prompt. */
            vmWaitIfStoppedWithCheckpoint();
#endif
            if (pscalRuntimeConsumeSigint()) {
                if (vm) {
                    vm->abort_requested = true;
                    vm->exit_requested = true;
                }
                buffer[0] = '\0';
                return false;
            }
            if (pscalRuntimeConsumeSigtstp()) {
#if defined(PSCAL_TARGET_IOS)
                if (!vmShouldUseCooperativeSuspend() && vmRequestHardSuspendCurrentVproc()) {
                    vmWaitIfStoppedWithCheckpoint();
                    continue;
                }
#endif
                if (vm) {
                    vm->abort_requested = false;
                    vm->exit_requested = true;
                    vm->suspend_unwind_requested = true;
                }
                shellRuntimeSetLastStatus(128 + SIGTSTP);
                buffer[0] = '\0';
                return false;
            }
#if defined(PSCAL_TARGET_IOS)
            {
                int cur_pid = vprocGetPidShim();
                if (cur_pid <= 0) {
                    cur_pid = vprocGetShellSelfPid();
                }
                if (cur_pid > 0) {
                    sigset_t pending;
                    sigemptyset(&pending);
                    if (vprocSigpending(cur_pid, &pending) == 0 &&
                        (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTSTP))) {
                        sigset_t watchset;
                        sigemptyset(&watchset);
                        sigaddset(&watchset, SIGINT);
                        sigaddset(&watchset, SIGTSTP);
                        int signo = 0;
                        if (vprocSigwait(cur_pid, &watchset, &signo) == 0) {
#if defined(PSCAL_TARGET_IOS)
                            if (signo == SIGTSTP &&
                                !vmShouldUseCooperativeSuspend() &&
                                vmRequestHardSuspendCurrentVproc()) {
                                vmWaitIfStoppedWithCheckpoint();
                                continue;
                            }
#endif
                            if (vm) {
                                vm->abort_requested = (signo == SIGINT);
                                vm->exit_requested = true;
                                vm->suspend_unwind_requested = (signo == SIGTSTP);
                            }
                            if (signo == SIGTSTP) {
                                shellRuntimeSetLastStatus(128 + SIGTSTP);
                            }
                            buffer[0] = '\0';
                            return false;
                        }
                    }
                }
            }
#endif
            if (vm && (vm->abort_requested || vm->exit_requested)) {
                buffer[0] = '\0';
                return false;
            }
#if defined(PSCAL_TARGET_IOS)
            if (sigint_fd >= 0 && !read_is_host) {
                int sig_flags = fcntl(sigint_fd, F_GETFL, 0);
                if (sig_flags >= 0 && !(sig_flags & O_NONBLOCK)) {
                    (void)fcntl(sigint_fd, F_SETFL, sig_flags | O_NONBLOCK);
                }
                char drain[8];
                ssize_t drained = vprocHostRead(sigint_fd, drain, sizeof(drain));
                if (drained > 0) {
                    if (vm) {
                        vm->abort_requested = true;
                        vm->exit_requested = true;
                    }
                    buffer[0] = '\0';
                    return false;
                }
                if (drained < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    if (errno == EBADF) {
                        vmEnsureSigintPipe();
                        sigint_fd = g_vm_sigint_pipe[0];
                        continue;
                    }
                    if (tool_dbg && stream == stdin) {
                        fprintf(stderr, "[readln] sigint drain error=%d (%s)\n",
                                errno, strerror(errno));
                    }
                }
            }
#endif
#if defined(PSCAL_TARGET_IOS)
            if (use_session_read) {
                char ch;
                ssize_t n = vprocSessionReadInputShimMode(&ch, 1, false);
                if (n == 0) {
                    if (tool_dbg && stream == stdin) {
                        fprintf(stderr, "[readln] session read EOF len=%zu\n", len);
                    }
                    break;
                }
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    if (tool_dbg && stream == stdin) {
                        fprintf(stderr, "[readln] session read error=%d (%s)\n",
                                errno, strerror(errno));
                    }
                    break;
                }
                if (tool_dbg && is_stdin_stream) {
                    fprintf(stderr, "[readln] byte session ch=0x%02x len=%zu\n",
                            (unsigned int)(unsigned char)ch, len);
                }
                if (pending_caret_control) {
                    pending_caret_control = false;
                    if (ch == 'C' || ch == 'c') {
                        if (vm) {
                            vm->abort_requested = true;
                            vm->exit_requested = true;
                        }
                        buffer[0] = '\0';
                        return false;
                    }
                    if (ch == 'Z' || ch == 'z') {
#if defined(PSCAL_TARGET_IOS)
                        if (vmHandleCtrlZReadRequest(vm, buffer)) {
                            len = 0;
                            continue;
                        }
#else
                        if (vm) {
                            vm->abort_requested = false;
                            vm->exit_requested = true;
                            vm->suspend_unwind_requested = true;
                        }
                        shellRuntimeSetLastStatus(128 + SIGTSTP);
                        buffer[0] = '\0';
#endif
                        return false;
                    }
                    buffer[len++] = '^';
                }
                if (ch == '^') {
                    pending_caret_control = true;
                    continue;
                }
                if (ch == 0x03) { // Ctrl-C
                    if (vm) {
                        vm->abort_requested = true;
                        vm->exit_requested = true;
                    }
                    buffer[0] = '\0';
                    return false;
                }
                if (ch == 0x1a) { // Ctrl-Z
#if defined(PSCAL_TARGET_IOS)
                    if (vmHandleCtrlZReadRequest(vm, buffer)) {
                        len = 0;
                        continue;
                    }
#else
                    if (vm) {
                        vm->abort_requested = false;
                        vm->exit_requested = true;
                        vm->suspend_unwind_requested = true;
                    }
                    shellRuntimeSetLastStatus(128 + SIGTSTP);
                    buffer[0] = '\0';
#endif
                    return false;
                }
                if (ch == '\r') {
                    saw_newline = true;
                    break;
                }
                if (ch == '\n') {
                    saw_newline = true;
                    break;
                }
                buffer[len++] = ch;
                continue;
            }
#endif
            fd_set rfds;
            FD_ZERO(&rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000; // 100ms
            int ready = -1;
#if defined(PSCAL_TARGET_IOS)
            if (read_is_host) {
                FD_SET(read_fd, &rfds);
                if (sigint_fd >= 0) {
                    FD_SET(sigint_fd, &rfds);
                }
                int maxfd = read_fd;
                if (sigint_fd >= 0 && sigint_fd > maxfd) {
                    maxfd = sigint_fd;
                }
                ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
            } else {
                FD_SET(read_fd, &rfds);
                ready = vprocSelectShim(read_fd + 1, &rfds, NULL, NULL, &tv);
            }
#else
            FD_SET(read_fd, &rfds);
            if (sigint_fd >= 0) {
                FD_SET(sigint_fd, &rfds);
            }
            int maxfd = read_fd;
            if (sigint_fd >= 0 && sigint_fd > maxfd) {
                maxfd = sigint_fd;
            }
            ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
#endif
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EBADF) {
#if defined(PSCAL_TARGET_IOS)
                    if (tool_dbg && stream == stdin) {
                        fprintf(stderr, "[readln] select EBADF read_fd=%d sigint_fd=%d\n",
                                read_fd, sigint_fd);
                    }
#endif
                    return false;
                }
                if (errno == EINTR) {
                    continue;
                }
#if defined(PSCAL_TARGET_IOS)
                if (tool_dbg && stream == stdin) {
                    fprintf(stderr, "[readln] select error=%d (%s) read_fd=%d sigint_fd=%d\n",
                            errno, strerror(errno), read_fd, sigint_fd);
                }
#endif
                break;
            }
            if (ready == 0) {
                continue;
            }
#if defined(PSCAL_TARGET_IOS)
            if (read_is_host && sigint_fd >= 0 && FD_ISSET(sigint_fd, &rfds)) {
#else
            if (sigint_fd >= 0 && FD_ISSET(sigint_fd, &rfds)) {
#endif
                vmDrainSigintFd(sigint_fd,
#if defined(PSCAL_TARGET_IOS)
                                true
#else
                                false
#endif
                );
                if (vm) {
                    vm->abort_requested = true;
                    vm->exit_requested = true;
                }
                buffer[0] = '\0';
                return false;
            }
            char ch;
#if defined(PSCAL_TARGET_IOS)
            ssize_t n = read_is_host ? vprocHostRead(read_fd, &ch, 1)
                                     : vprocReadShim(read_fd, &ch, 1);
#else
            ssize_t n = read(read_fd, &ch, 1);
#endif
            if (n == 0) {
#if defined(PSCAL_TARGET_IOS)
                if (tool_dbg && stream == stdin) {
                    fprintf(stderr, "[readln] read EOF read_fd=%d host=%d len=%zu\n",
                            read_fd, (int)read_is_host, len);
                }
#endif
                break;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
#if defined(PSCAL_TARGET_IOS)
                if (tool_dbg && stream == stdin) {
                    fprintf(stderr, "[readln] read error=%d (%s) read_fd=%d host=%d\n",
                            errno, strerror(errno), read_fd, (int)read_is_host);
                }
#endif
                break;
            }
            if (tool_dbg && is_stdin_stream) {
                fprintf(stderr, "[readln] byte direct ch=0x%02x len=%zu\n",
                        (unsigned int)(unsigned char)ch, len);
            }
#if defined(PSCAL_TARGET_IOS)
            /* Caret notation ("^C", "^Z" as two characters) stands for a
             * control key only in the iOS app's session input. On a real
             * terminal Ctrl-C arrives as 0x03 or SIGINT, and a "^c" the user
             * typed is text: treating it as Ctrl-C aborted `readln` on input
             * like "^cat". */
            if (pending_caret_control) {
                pending_caret_control = false;
                if (ch == 'C' || ch == 'c') {
                    if (vm) {
                        vm->abort_requested = true;
                        vm->exit_requested = true;
                    }
                    buffer[0] = '\0';
                    return false;
                }
                if (ch == 'Z' || ch == 'z') {
                    if (vmHandleCtrlZReadRequest(vm, buffer)) {
                        len = 0;
                        continue;
                    }
                    return false;
                }
                buffer[len++] = '^';
            }
            if (ch == '^') {
                pending_caret_control = true;
                continue;
            }
#endif
            if (ch == 0x03) { // Ctrl-C
                if (vm) {
                    vm->abort_requested = true;
                    vm->exit_requested = true;
                }
                buffer[0] = '\0';
                return false;
            }
            if (ch == 0x1a) { // Ctrl-Z
#if defined(PSCAL_TARGET_IOS)
                if (vmHandleCtrlZReadRequest(vm, buffer)) {
                    len = 0;
                    continue;
                }
#else
                if (vm) {
                    vm->abort_requested = false;
                    vm->exit_requested = true;
                    vm->suspend_unwind_requested = true;
                }
                shellRuntimeSetLastStatus(128 + SIGTSTP);
                buffer[0] = '\0';
#endif
                return false;
            }

            /*
             * Some terminals (notably iOS/macCatalyst hterm) may return a
             * lingering ANSI cursor-position report (ESC [ rows ; cols R)
             * that was triggered earlier.  If we leave it in the canonical
             * input buffer it shows up in prompts like "^[[25;34R" and the
             * user must backspace it away.  Detect and swallow that sequence
             * before it reaches user-facing buffers.
             */
            if (is_stdin_stream && ch == '\x1B') {
                char seq[32];
                seq[0] = ch;
                size_t seq_len = 1;
                int pending = 0;
#if defined(PSCAL_TARGET_IOS)
                vprocIoctlShim(read_fd, FIONREAD, &pending);
#else
                ioctl(read_fd, FIONREAD, &pending);
#endif
                if (pending > 0) {
                    if (pending > (int)(sizeof(seq) - seq_len - 1)) {
                        pending = (int)(sizeof(seq) - seq_len - 1);
                    }
                    ssize_t extra = 0;
#if defined(PSCAL_TARGET_IOS)
                    extra = read_is_host ? vprocHostRead(read_fd, seq + seq_len, (size_t)pending)
                                         : vprocReadShim(read_fd, seq + seq_len, (size_t)pending);
#else
                    extra = read(read_fd, seq + seq_len, (size_t)pending);
#endif
                    if (extra > 0) {
                        seq_len += (size_t)extra;
                    }
                }
                seq[seq_len] = '\0';
                bool looks_dsr = (seq_len >= 4 && seq[0] == '\x1B' && seq[1] == '[' && seq[seq_len - 1] == 'R');
                bool saw_semi = false;
                if (looks_dsr) {
                    for (size_t k = 2; k + 1 < seq_len; k++) {
                        char c = seq[k];
                        if (c == ';') {
                            if (saw_semi) { looks_dsr = false; break; }
                            saw_semi = true;
                            continue;
                        }
                        if (c < '0' || c > '9') { looks_dsr = false; break; }
                    }
                    if (!saw_semi) looks_dsr = false;
                }
                if (looks_dsr) {
                    // Swallow the entire report and start fresh.
                    len = 0;
                    continue;
                }
                // Not a DSR; push everything we pulled back into the user buffer.
                for (size_t k = 0; k < seq_len; k++) {
                    buffer[len++] = seq[k];
                }
                continue;
            }

            if (is_stdin_stream && len > 0 && vmBufferIsPureDsr(buffer, len)) {
                len = 0;
                continue;
            }

            if (ch == '\r') {
#if defined(PSCAL_TARGET_IOS)
                saw_newline = true;
                break;
#else
                continue;
#endif
            }
            if (ch == '\n') {
                saw_newline = true;
                break;
            }
            buffer[len++] = ch;
        }
        buffer[len] = '\0';
        line->len = len;
#if defined(PSCAL_TARGET_IOS)
        if (tool_dbg && stream == stdin) {
            fprintf(stderr, "[readln] done len=%zu saw_nl=%d eof=%d\n",
                    len, (int)saw_newline, (int)feof(stream));
        }
#endif
        if (saw_newline || len > 0 || feof(stream)) {
            return true;
        }
        return false;
    }

    ssize_t got = getline(&line->data, &line->cap, stream);
    if (got < 0) {
        if (line->data) line->data[0] = '\0';
        return false;
    }
    line->len = strcspn(line->data, "\r\n");
    line->data[line->len] = '\0';
    return true;
}

static void vmCommitLastIoError(int value) {
    last_io_error = value; // thread-local; no lock needed
}

static bool vmBufferIsPureDsr(const char *buf, size_t len) {
    size_t i = 0;
    while (i < len && (buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i >= len || buf[i] != '\x1B') return false;
    i++;
    if (i >= len || buf[i] != '[') return false;
    i++;
    size_t digits = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') { digits++; i++; }
    if (digits == 0 || i >= len || buf[i] != ';') return false;
    i++;
    digits = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') { digits++; i++; }
    if (digits == 0 || i >= len || buf[i] != 'R') return false;
    i++;
    while (i < len && (buf[i] == '\r' || buf[i] == '\n')) i++;
    return i == len;
}

// Attempts to get the current cursor position using ANSI DSR query.
// Returns 0 on success, -1 on failure.
// Stores results in *row and *col.
static int getCursorPosition(int *row, int *col) {
    struct termios oldt, newt;
    char buf[32];       // Buffer for response: ESC[<row>;<col>R
    int i = 0;
    char ch;
    int ret_status = -1; // Default to critical failure
    int read_errno = 0; // Store errno from read() operation
    bool termiosApplied = false;
    int stdin_flags = -1;
    bool restore_blocking = false;
    bool query_sent = false;

    // Default row/col in case of non-critical failure
    *row = 1;
    *col = 1;

    // --- Check if Input is a Terminal ---
    if (!pscalRuntimeStdinIsInteractive()) {
        ret_status = 0; // Non-interactive: fall back to default 1,1 silently.
        goto cleanup;
    }

    // Some callers may have left stdin in non-blocking mode; temporarily
    // make it blocking so VTIME/VMIN can take effect.
    stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (stdin_flags != -1 && (stdin_flags & O_NONBLOCK)) {
        if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags & ~O_NONBLOCK) == 0) {
            restore_blocking = true;
        }
    }

    // --- Save Current Terminal Settings ---
    if (vmTcgetattr(STDIN_FILENO, &oldt) < 0) {
        perror("getCursorPosition: tcgetattr failed");
        goto cleanup; // Critical failure
    }

    // --- Prepare and Set Raw Mode ---
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    newt.c_cc[VMIN] = 0;              // Non-blocking; we'll poll below
    newt.c_cc[VTIME] = 0;

    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0) {
        int setup_errno = errno;
        perror("getCursorPosition: tcsetattr (set raw) failed");
        // Attempt to restore original settings even if setting new ones failed
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Best effort restore
        errno = setup_errno; // Restore errno for accurate reporting
        goto cleanup; // Critical failure
    }
    termiosApplied = true;

    // --- Write DSR Query ---
    static const char dsr_query[] = "\x1B[6n"; // ANSI Device Status Report for cursor position
    if (write(STDOUT_FILENO, dsr_query, sizeof(dsr_query) - 1) == -1) {
        int write_errno = errno;
        perror("getCursorPosition: write DSR query failed");
        errno = write_errno;
        goto cleanup; // Critical failure
    }
    query_sent = true;

    // --- Read Response ---
    memset(buf, 0, sizeof(buf));
    i = 0;
    int elapsed_ms = 0;
    const int poll_step_ms = 10;
    const int max_wait_ms = 150; // keep cursor probe lightweight
#if defined(PSCAL_TARGET_IOS)
    while (i < (int)sizeof(buf) - 1 && elapsed_ms <= max_wait_ms) {
        errno = 0;
        ssize_t bytes_read = vprocSessionReadInputShimMode(&ch, 1, true);
        read_errno = errno;
        if (bytes_read == 1) {
            buf[i++] = ch;
            if (ch == 'R') {
                break;
            }
            elapsed_ms = 0; // reset timer on progress
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0 && (read_errno == EAGAIN || read_errno == EWOULDBLOCK)) {
            usleep((useconds_t)poll_step_ms * 1000);
            elapsed_ms += poll_step_ms;
            continue;
        }
        if (bytes_read < 0 && read_errno == EINTR) {
            continue;
        }
        break;
    }
#else
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    while (i < (int)sizeof(buf) - 1 && elapsed_ms <= max_wait_ms) {
        int pr = poll(&pfd, 1, poll_step_ms);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            errno = 0;
            ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
            read_errno = errno;
            if (bytes_read == 1) {
                buf[i++] = ch;
                if (ch == 'R') {
                    break;
                }
                elapsed_ms = 0; // reset timer on progress
                continue;
            }
            if (bytes_read < 0 && (read_errno == EAGAIN || read_errno == EWOULDBLOCK)) {
                continue;
            }
            break;
        }
        elapsed_ms += poll_step_ms;
    }
#endif
    // Give a brief grace period for late-arriving bytes so they don't leak into
    // subsequent user input (observed on iOS terminals).
    if (i < (int)sizeof(buf) - 1 && buf[i != 0 ? i - 1 : 0] != 'R') {
        int grace_ms = 100;
#if defined(PSCAL_TARGET_IOS)
        while (grace_ms > 0 && i < (int)sizeof(buf) - 1) {
            errno = 0;
            ssize_t bytes_read = vprocSessionReadInputShimMode(&ch, 1, true);
            read_errno = errno;
            if (bytes_read == 1) {
                buf[i++] = ch;
                if (ch == 'R') {
                    break;
                }
                continue;
            }
            if (bytes_read == 0) {
                break;
            }
            if (bytes_read < 0 && (read_errno == EAGAIN || read_errno == EWOULDBLOCK)) {
                usleep(10 * 1000);
            } else if (bytes_read < 0 && read_errno == EINTR) {
                continue;
            } else {
                break;
            }
            grace_ms -= 10;
        }
#else
        while (grace_ms > 0 && i < (int)sizeof(buf) - 1) {
            int pr = poll(&pfd, 1, 10);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                errno = 0;
                ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
                read_errno = errno;
                if (bytes_read == 1) {
                    buf[i++] = ch;
                    if (ch == 'R') {
                        break;
                    }
                    continue;
                }
                if (bytes_read < 0 && (read_errno == EAGAIN || read_errno == EWOULDBLOCK)) {
                    // keep waiting within grace window
                } else {
                    break;
                }
            }
            grace_ms -= 10;
        }
#endif
    }
    buf[i] = '\0'; // Null-terminate the buffer

    // --- Parse Response ---
    // Expected format: \x1B[<row>;<col>R
    int parsed_row = 0, parsed_col = 0;
    if (i > 0 && buf[0] == '\x1B' && buf[1] == '[' && buf[i-1] == 'R') {
        // Attempt to parse using sscanf
        if (sscanf(buf, "\x1B[%d;%dR", &parsed_row, &parsed_col) == 2) {
            *row = parsed_row;
            *col = parsed_col;
            ret_status = 0; // Success!
#ifdef DEBUG
            if (dumpExec) fprintf(stderr, "[DEBUG] getCursorPosition: Parsed Row=%d, Col=%d from response '%s'\n", *row, *col, buf);
#endif
        } else {
#ifdef DEBUG
             if (dumpExec) fprintf(stderr, "Warning: Failed to parse cursor position response values: '%s'\n", buf);
#endif
             ret_status = 0; // Non-critical failure, return default 1,1
        }
    } else {
#ifdef DEBUG
         if (dumpExec) fprintf(stderr, "Warning: Invalid or incomplete cursor position response format: '%s'\n", buf);
#endif
         ret_status = 0; // Non-critical failure, return default 1,1
    }

cleanup:
    if (query_sent && !(i > 0 && buf[i - 1] == 'R')) {
        atomic_store(&g_vm_dsr_reply_pending, true);
    }
    /* Flush any pending input so delayed DSR replies don't echo later. */
    tcflush(STDIN_FILENO, TCIFLUSH);
    /* Drain any residual DSR bytes so later reads (e.g., ReadKey) are not
     * satisfied by a leftover ESC[ row ; col R response. */
    {
#if defined(PSCAL_TARGET_IOS)
        char discard[64];
        while (true) {
            errno = 0;
            ssize_t drained = vprocSessionReadInputShimMode(discard, sizeof(discard), true);
            if (drained > 0) {
                continue;
            }
            if (drained == 0) {
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EBADF) {
                break;
            }
            break;
        }
#else
        int cur_flags = fcntl(STDIN_FILENO, F_GETFL);
        if (cur_flags != -1 && (cur_flags & O_NONBLOCK) == 0) {
            if (fcntl(STDIN_FILENO, F_SETFL, cur_flags | O_NONBLOCK) == 0) {
                char discard[64];
                while (read(STDIN_FILENO, discard, sizeof(discard)) > 0) {
                }
                fcntl(STDIN_FILENO, F_SETFL, cur_flags);
            }
        }
#endif
    }

    // --- Restore Original Terminal Settings ---
    if (termiosApplied) {
        if (vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0) {
            perror("getCursorPosition: tcsetattr (restore) failed - Terminal state may be unstable!");
            // Continue processing, but be aware terminal might be left in raw mode
        }
    }
    if (restore_blocking && stdin_flags != -1) {
        fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    }
    return ret_status; // 0 for success or non-critical error, -1 for critical error
}

Value vmBuiltinKeypressed(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "KeyPressed expects 0 arguments.");
        return makeBoolean(false);
    }
    fflush(stdout);
    vmEnableRawMode();

    int bytes_available = 0;
#if defined(PSCAL_TARGET_IOS)
    vprocIoctlShim(STDIN_FILENO, FIONREAD, &bytes_available);
#else
    ioctl(STDIN_FILENO, FIONREAD, &bytes_available);
#endif
    return makeBoolean(bytes_available > 0);
}

Value vmBuiltinPollkeyany(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "PollKeyAny expects 0 arguments.");
        return makeInt(0);
    }

#ifdef SDL
    SDL_Keycode sdl_code;
    if (sdlPollNextKey(&sdl_code)) {
        return makeInt((int)sdl_code);
    }
#endif

    vmEnableRawMode();
    int bytes_available = 0;
#if defined(PSCAL_TARGET_IOS)
    vprocIoctlShim(STDIN_FILENO, FIONREAD, &bytes_available);
#else
    ioctl(STDIN_FILENO, FIONREAD, &bytes_available);
#endif
    if (bytes_available > 0) {
        unsigned char ch_byte = 0;
#if defined(PSCAL_TARGET_IOS)
        if (vprocReadShim(STDIN_FILENO, &ch_byte, 1) == 1) {
#else
        if (read(STDIN_FILENO, &ch_byte, 1) == 1) {
#endif
            return makeInt((int)ch_byte);
        }
    }

    return makeInt(0);
}

Value vmBuiltinReadkey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0 && arg_count != 1) {
        runtimeError(vm, "ReadKey expects 0 or 1 argument.");
        return makeChar('\0');
    }

    fflush(stdout);
    int c = 0;
    for (;;) {
        if (pscalRuntimeConsumeSigint()) {
            if (vm) {
                vm->abort_requested = false;
                vm->exit_requested = true;
                vm->suspend_unwind_requested = false;
            }
            shellRuntimeSetLastStatus(130);
            c = 0;
            break;
        }
#if defined(PSCAL_TARGET_IOS)
        if (pscalRuntimeConsumeSigtstp()) {
            char scratch[1] = {'\0'};
            if (vmHandleCtrlZReadRequest(vm, scratch)) {
                continue;
            }
            c = 0;
            break;
        }
#endif
#ifdef SDL
        if (sdlIsGraphicsActive()) {
            c = sdlFetchReadKeyChar();
            if (c < 0) {
                c = 0;
            }
        } else
#endif
        {
            vmEnableRawMode();
            c = readKeyFetchConsoleByte();
        }

        if (c == 0) {
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
            (void)poll(&pfd, 1, 10);
            continue;
        }

#if defined(PSCAL_TARGET_IOS)
        /* Caret notation ("^C" typed as two characters) means a control key
         * only in the iOS app's session input; on a terminal "^" is a key
         * like any other, and the "c" after it aborted the program. */
        if (gReadKeyPendingCaretControl) {
            gReadKeyPendingCaretControl = false;
            if (c == 'C' || c == 'c') {
                if (vm) {
                    vm->abort_requested = false;
                    vm->exit_requested = true;
                    vm->suspend_unwind_requested = false;
                }
                shellRuntimeSetLastStatus(130);
                c = 0;
                break;
            }
            if (c == 'Z' || c == 'z') {
                char scratch[1] = {'\0'};
                if (vmHandleCtrlZReadRequest(vm, scratch)) {
                    continue;
                }
                c = 0;
                break;
            }
        }

        if (c == '^') {
            gReadKeyPendingCaretControl = true;
            break;
        }
#endif
        if (c == 0x03) { // Ctrl-C
            if (vm) {
                vm->abort_requested = false;
                vm->exit_requested = true;
                vm->suspend_unwind_requested = false;
            }
            shellRuntimeSetLastStatus(130);
            c = 0;
            break;
        }
        if (c == 0x1a) { // Ctrl-Z
#if defined(PSCAL_TARGET_IOS)
            char scratch[1] = {'\0'};
            if (vmHandleCtrlZReadRequest(vm, scratch)) {
                continue;
            }
            c = 0;
            break;
#else
            if (vm) {
                vm->abort_requested = false;
                vm->exit_requested = true;
                vm->suspend_unwind_requested = true;
            }
            shellRuntimeSetLastStatus(128 + SIGTSTP);
            c = 0;
            break;
#endif
        }
        break;
    }

    if (arg_count == 1) {
        if (VALUE_TYPE(args[0]) != TYPE_POINTER || AS_POINTER(args[0]) == NULL) {
            runtimeError(vm, "ReadKey argument must be a VAR char.");
        } else {
            Value* dst = (Value*)AS_POINTER(args[0]);
                if (VALUE_TYPE(*dst) == TYPE_CHAR) {
                    SET_CHAR_VALUE(dst, c);
                    SET_INT_VALUE(dst, AS_CHAR(*dst));
                } else {
                    runtimeError(vm, "ReadKey argument must be of type CHAR.");
                }
        }
    }

    return makeChar(c);
}

Value vmBuiltinQuitrequested(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "QuitRequested expects 0 arguments.");
        // Return a default value in case of error
        return makeBoolean(false);
    }
    // Access the global flag and return it as a Pscal boolean
    return makeBoolean(atomic_load(&break_requested) != 0);
}

Value vmBuiltinGotoxy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "GotoXY expects 2 integer arguments.");
        return makeVoid();
    }
    if (vmShouldBypassTerminalControl()) {
        return makeVoid();
    }
    long long x = AS_INTEGER(args[0]);
    long long y = AS_INTEGER(args[1]);
    long long absX = gWindowLeft + x - 1;
    long long absY = gWindowTop + y - 1;
    printf("\x1B[%lld;%lldH", absY, absX);
    return makeVoid();
}

Value vmBuiltinTextcolor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextColor expects 1 integer argument.");
        return makeVoid();
    }
    long long colorCode = AS_INTEGER(args[0]);
    // Reduced into 0..15 also for a negative code (C's % keeps the sign,
    // and TextColor(-100) indexed the colour table at -4).
    gCurrentTextColor = (int)(((colorCode % 16) + 16) % 16);
    gCurrentTextBold = (colorCode >= 8 && colorCode <= 15);
    gCurrentColorIsExt = false;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinTextbackground(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextBackground expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)(((AS_INTEGER(args[0]) % 8) + 8) % 8);
    gCurrentBgIsExt = false;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}
Value vmBuiltinTextcolore(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextColorE expects an integer argument.");
        return makeVoid();
    }
    gCurrentTextColor = (int)AS_INTEGER(args[0]);
    gCurrentTextBold = false;
    gCurrentColorIsExt = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinTextbackgrounde(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextBackgroundE expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)AS_INTEGER(args[0]);
    gCurrentBgIsExt = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinBoldtext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "BoldText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBold = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinUnderlinetext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "UnderlineText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextUnderline = true;
    markTextAttrDirty();
    return makeVoid();
}

Value vmBuiltinBlinktext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "BlinkText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBlink = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinLowvideo(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "LowVideo expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBold = false;
    gCurrentTextColor &= 0x07;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinNormvideo(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "NormVideo expects no arguments.");
        return makeVoid();
    }
    gCurrentTextColor = 7;
    gCurrentTextBackground = 0;
    gCurrentTextBold = false;
    gCurrentColorIsExt = false;
    gCurrentBgIsExt = false;
    gCurrentTextUnderline = false;
    gCurrentTextBlink = false;
    printf("\x1B[0m");
    fflush(stdout);

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinClrscr(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ClrScr expects no arguments.");
        return makeVoid();
    }

    if (!pscalRuntimeStdoutIsInteractive()) {
        return makeVoid();
    }
    if (vmShouldBypassTerminalControl()) {
        return makeVoid();
    }

    /* Prefer the portable clear-screen sequence that embedded terminals reliably honor. */
    fputs("\x1B[2J\x1B[H", stdout);
    fflush(stdout);

    return makeVoid();
}

Value vmBuiltinClreol(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ClrEol expects no arguments.");
        return makeVoid();
    }
    if (vmShouldBypassTerminalControl()) {
        return makeVoid();
    }
    /* Do not probe cursor position here: DSR queries interfere with some embedded terminals. */
    printf("\x1B[K");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinHidecursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "HideCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[?25l");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinShowcursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ShowCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[?25h");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinCursoroff(VM* vm, int arg_count, Value* args) {
    return vmBuiltinHidecursor(vm, arg_count, args);
}

Value vmBuiltinCursoron(VM* vm, int arg_count, Value* args) {
    return vmBuiltinShowcursor(vm, arg_count, args);
}

Value vmBuiltinDeline(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "DelLine expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[M");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinInsline(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "InsLine expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[L");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinInvertcolors(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "InvertColors expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[7m");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinNormalcolors(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "NormalColors expects no arguments.");
        return makeVoid();
    }
    gCurrentTextColor = 7;
    gCurrentTextBackground = 0;
    gCurrentTextBold = false;
    gCurrentColorIsExt = false;
    gCurrentBgIsExt = false;
    gCurrentTextUnderline = false;
    gCurrentTextBlink = false;
    printf("\x1B[0m");
    fflush(stdout);

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinBeep(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "Beep expects no arguments.");
        return makeVoid();
    }
    fputc('\a', stdout);
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinSavecursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "SaveCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[s");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinRestorecursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "RestoreCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[u");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinPushscreen(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "PushScreen expects no arguments.");
        return makeVoid();
    }
    if (pscalRuntimeStdoutIsInteractive()) {
        if (vm_color_stack_depth == 0) {
            vmPushColorState(); // the base colours, restored at exit
        }
        vmPushColorState();
        if (vm_alt_screen_depth == 0) {
            const char enter_alt[] = "\x1B[?1049h";
            if (write(STDOUT_FILENO, enter_alt, sizeof(enter_alt) - 1) != (ssize_t)(sizeof(enter_alt) - 1)) {
                perror("vmBuiltinPushscreen: write enter_alt");
            }
        }
        vm_alt_screen_depth++;
        vmRestoreColorState();
        fflush(stdout);
    }
    return makeVoid();
}

Value vmBuiltinPopscreen(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "PopScreen expects no arguments.");
        return makeVoid();
    }
    if (vm_alt_screen_depth > 0) {
        vm_alt_screen_depth--;
        vmPopColorState();
        if (pscalRuntimeStdoutIsInteractive()) {
            if (vm_alt_screen_depth == 0) {
                const char exit_alt[] = "\x1B[?1049l";
                if (write(STDOUT_FILENO, exit_alt, sizeof(exit_alt) - 1) != (ssize_t)(sizeof(exit_alt) - 1)) {
                    perror("vmBuiltinPopscreen: write exit_alt");
                }
            }
            vmRestoreColorState();
            fflush(stdout);
        }
    }
    return makeVoid();
}

Value vmBuiltinHighvideo(VM* vm, int arg_count, Value* args) {
    return vmBuiltinBoldtext(vm, arg_count, args);
}

Value vmBuiltinWindow(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4 ||
        !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) ||
        !IS_INTLIKE(args[2]) || !IS_INTLIKE(args[3])) {
        runtimeError(vm, "Window expects 4 integer arguments.");
        return makeVoid();
    }
    int screen_rows = 24, screen_cols = 80;
    getTerminalSize(&screen_rows, &screen_cols);

    gWindowLeft = (int)AS_INTEGER(args[0]);
    gWindowTop = (int)AS_INTEGER(args[1]);
    gWindowRight = (int)AS_INTEGER(args[2]);
    gWindowBottom = (int)AS_INTEGER(args[3]);

    if (gWindowLeft < 1) gWindowLeft = 1;
    if (gWindowTop < 1) gWindowTop = 1;
    if (gWindowRight < gWindowLeft) gWindowRight = gWindowLeft;
    if (gWindowBottom < gWindowTop) gWindowBottom = gWindowTop;
    if (gWindowRight > screen_cols) gWindowRight = screen_cols;
    if (gWindowBottom > screen_rows) gWindowBottom = screen_rows;

    printf("\x1B[%d;%dr", gWindowTop, gWindowBottom);
    printf("\x1B[%d;%dH", gWindowTop, gWindowLeft);
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinRewrite(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "Rewrite requires 1 or 2 arguments.");
        return makeVoid();
    }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "Rewrite: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(args[0]); // Dereference the pointer

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) { runtimeError(vm, "Argument to Rewrite must be a file variable."); return makeVoid(); }
    if (FILE_FILENAME(*fileVarLValue) == NULL) { runtimeError(vm, "File variable not assigned a name before Rewrite."); return makeVoid(); }
    vmMaybeCloseFileValue(fileVarLValue);

    bool has_record_size_arg = (arg_count == 2);
    int new_record_size = FILE_RECORD_SIZE(*fileVarLValue);
    if (has_record_size_arg) {
        if (!IS_INTLIKE(args[1])) {
            runtimeError(vm, "Rewrite: Record size must be an integer value.");
            return makeVoid();
        }
        long long size_val = AS_INTEGER(args[1]);
        if (size_val <= 0 || size_val > INT_MAX) {
            runtimeError(vm, "Rewrite: Record size must be between 1 and %d.", INT_MAX);
            return makeVoid();
        }
        new_record_size = (int)size_val;
        FILE_RECORD_SIZE_EXPLICIT(*fileVarLValue) = true;
    } else if (new_record_size <= 0) {
        new_record_size = PSCAL_DEFAULT_FILE_RECORD_SIZE;
        FILE_RECORD_SIZE_EXPLICIT(*fileVarLValue) = false;
    }
    FILE_RECORD_SIZE(*fileVarLValue) = new_record_size;

    bool use_binary_mode = has_record_size_arg || FILE_RECORD_SIZE_EXPLICIT(*fileVarLValue) ||
                           FILE_ELEMENT_TYPE(*fileVarLValue) != TYPE_VOID;
    const char* mode = use_binary_mode ? "wb" : "w";

    FILE* f = fopen(FILE_FILENAME(*fileVarLValue), mode);
    if (f == NULL) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
    AS_FILE(*fileVarLValue) = f;
    return makeVoid();
}

Value vmBuiltinSqrt(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sqrt expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    long double x = IS_INTLIKE(arg) ? (long double)AS_INTEGER(arg) : AS_REAL(arg);
    if (x < 0) { runtimeError(vm, "sqrt expects a non-negative argument."); return makeReal(0.0); }
    if (VALUE_TYPE(arg) == TYPE_LONG_DOUBLE) {
        return makeLongDouble(sqrtl(x));
    }
    return makeReal(sqrt((double)x));
}

Value vmBuiltinExp(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "exp expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(exp(x));
}

Value vmBuiltinLn(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ln expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    if (x <= 0) { runtimeError(vm, "ln expects a positive argument."); return makeReal(0.0); }
    return makeReal(log(x));
}

Value vmBuiltinCos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cos expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(cos(x));
}

Value vmBuiltinSin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sin expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(sin(x));
}

Value vmBuiltinTan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "tan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(tan(x));
}

Value vmBuiltinAtan2(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "atan2 expects 2 arguments.");
        return makeReal(0.0);
    }

    Value yArg = args[0];
    Value xArg = args[1];
    double y = IS_INTLIKE(yArg) ? (double)AS_INTEGER(yArg) : (double)AS_REAL(yArg);
    double x = IS_INTLIKE(xArg) ? (double)AS_INTEGER(xArg) : (double)AS_REAL(xArg);
    return makeReal(atan2(y, x));
}

Value vmBuiltinArctan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "arctan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(atan(x));
}

Value vmBuiltinArcsin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "arcsin expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(asin(x));
}

Value vmBuiltinArccos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "arccos expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(acos(x));
}

Value vmBuiltinCotan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cotan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    double t = tan(x);
    return makeReal(1.0 / t);
}

Value vmBuiltinPower(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "power expects 2 arguments.");
        return makeReal(0.0);
    }

    bool base_is_int = IS_INTLIKE(args[0]);
    bool exp_is_int  = IS_INTLIKE(args[1]);

    if (base_is_int && exp_is_int) {
        long long base = AS_INTEGER(args[0]);
        long long exp  = AS_INTEGER(args[1]);
        if (exp >= 0) {
            long long result = 1;
            long long b = base;
            long long e = exp;
            bool overflow = false;
            while (e > 0 && !overflow) {
                if (e & 1) {
                    overflow |= __builtin_mul_overflow(result, b, &result);
                }
                e >>= 1;
                if (e)
                    overflow |= __builtin_mul_overflow(b, b, &b);
            }
            if (!overflow) {
                return pscalIntResultLike2(args[0], args[1], result);
            }
            // fall through to real computation on overflow
        }
        // negative exponent falls through to real computation
    }

    double base = base_is_int ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    double exponent = exp_is_int ? (double)AS_INTEGER(args[1]) : (double)AS_REAL(args[1]);
    return makeReal(pow(base, exponent));
}

Value vmBuiltinLog10(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "log10 expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(log10(x));
}

Value vmBuiltinSinh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sinh expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(sinh(x));
}

Value vmBuiltinCosh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cosh expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(cosh(x));
}

Value vmBuiltinTanh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "tanh expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(tanh(x));
}

Value vmBuiltinMax(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "max expects 2 arguments."); return makeInt(0); }

    bool aInt = IS_INTLIKE(args[0]);
    bool bInt = IS_INTLIKE(args[1]);

    if (aInt && bInt) {
        long long a = AS_INTEGER(args[0]);
        long long b = AS_INTEGER(args[1]);
        return pscalIntResultLike2(args[0], args[1], (a > b) ? a : b);
    } else {
        double a = aInt ? (double)AS_INTEGER(args[0]) : AS_REAL(args[0]);
        double b = bInt ? (double)AS_INTEGER(args[1]) : AS_REAL(args[1]);
        return makeReal((a > b) ? a : b);
    }
}

Value vmBuiltinMin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "min expects 2 arguments."); return makeInt(0); }

    bool aInt = IS_INTLIKE(args[0]);
    bool bInt = IS_INTLIKE(args[1]);

    if (aInt && bInt) {
        long long a = AS_INTEGER(args[0]);
        long long b = AS_INTEGER(args[1]);
        return pscalIntResultLike2(args[0], args[1], (a < b) ? a : b);
    } else {
        double a = aInt ? (double)AS_INTEGER(args[0]) : AS_REAL(args[0]);
        double b = bInt ? (double)AS_INTEGER(args[1]) : AS_REAL(args[1]);
        return makeReal((a < b) ? a : b);
    }
}

Value vmBuiltinClamp(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) { runtimeError(vm, "clamp expects 3 arguments (value, lo, hi)."); return makeInt(0); }
    if (IS_INTLIKE(args[0]) && IS_INTLIKE(args[1]) && IS_INTLIKE(args[2])) {
        long long x = AS_INTEGER(args[0]);
        long long lo = AS_INTEGER(args[1]);
        long long hi = AS_INTEGER(args[2]);
        bool anyUInt64 = VALUE_TYPE(args[0]) == TYPE_UINT64 || VALUE_TYPE(args[1]) == TYPE_UINT64 || VALUE_TYPE(args[2]) == TYPE_UINT64;
        bool anyInt64 = VALUE_TYPE(args[0]) == TYPE_INT64 || VALUE_TYPE(args[1]) == TYPE_INT64 || VALUE_TYPE(args[2]) == TYPE_INT64;
        long long result = (x < lo) ? lo : (x > hi) ? hi : x;
        if (anyUInt64) return makeUInt64((unsigned long long)result);
        if (anyInt64) return makeInt64(result);
        return makeInt(result);
    }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : AS_REAL(args[0]);
    double lo = IS_INTLIKE(args[1]) ? (double)AS_INTEGER(args[1]) : AS_REAL(args[1]);
    double hi = IS_INTLIKE(args[2]) ? (double)AS_INTEGER(args[2]) : AS_REAL(args[2]);
    if (x < lo) return makeReal(lo);
    if (x > hi) return makeReal(hi);
    return makeReal(x);
}

Value vmBuiltinFloor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "floor expects 1 argument."); return makeInt(0); }
    if (IS_INTLIKE(args[0])) return pscalIntResultLike1(args[0], AS_INTEGER(args[0]));
    return vmRealToIntResult(vm, "floor", floorl(AS_REAL(args[0])));
}

Value vmBuiltinCeil(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ceil expects 1 argument."); return makeInt(0); }
    if (IS_INTLIKE(args[0])) return pscalIntResultLike1(args[0], AS_INTEGER(args[0]));
    return vmRealToIntResult(vm, "ceil", ceill(AS_REAL(args[0])));
}

Value vmBuiltinTrunc(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "trunc expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) return pscalIntResultLike1(arg, AS_INTEGER(arg));
    if (isRealType(VALUE_TYPE(arg))) return vmRealToIntResult(vm, "trunc", truncl(AS_REAL(arg)));
    runtimeError(vm, "trunc expects a numeric argument.");
    return makeInt(0);
}

static inline bool isOrdinalDelta(const Value* v) {
    return isIntlikeType(VALUE_TYPE(*v)) || VALUE_TYPE(*v) == TYPE_CHAR /* || v->type == TYPE_BOOLEAN */;
}

static inline long long coerceDeltaToI64(const Value* v) {
    switch (VALUE_TYPE(*v)) {
        case TYPE_CHAR:
            return AS_CHAR(*v);
        default:
            /* isOrdinalDelta() (the caller's gate) already restricts this
             * to intlike types or TYPE_CHAR -- AS_INTEGER's generic
             * dispatch covers every intlike width (INT8/16/32/64,
             * UINT8/16/32/64, WORD, BYTE, BOOLEAN), unlike the old
             * hand-picked switch here, which silently returned 0 (a
             * no-op delta) for any width it didn't explicitly list, e.g.
             * TYPE_INT16 ("SmallInt"). */
            return isIntlikeType(VALUE_TYPE(*v)) ? AS_INTEGER(*v) : 0;
    }
}

Value vmBuiltinOrd(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ord expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (VALUE_TYPE(arg) == TYPE_CHAR || VALUE_TYPE(arg) == TYPE_WIDECHAR) return makeInt(AS_CHAR(arg));
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        size_t len = strlen(AS_STRING(arg));
        uint32_t codepoint = 0;
        size_t advance = 0;
        /* `len > 0` (not `len > 1`): a single-byte ASCII string (the common
         * case, e.g. ord("a")) is exactly 1 byte long and decodes in one
         * step (decodeUtf8Codepoint sets advance=1 for any byte <= 0x7F).
         * The old `len > 1` bound excluded every single-character ASCII
         * string from this branch entirely, so ord() fell through to the
         * "not an ordinal type" error for the most common input shape. */
        if (len > 0 && decodeUtf8Codepoint(AS_STRING(arg), len, &codepoint, &advance) && advance == len) {
            return makeInt((long long)codepoint);
        }
    }
    if (VALUE_TYPE(arg) == TYPE_BOOLEAN) return makeInt(VAL_INT(arg));
    if (VALUE_TYPE(arg) == TYPE_ENUM) return makeInt(AS_ENUM(arg).ordinal);
    if (IS_INTLIKE(arg)) return pscalIntResultLike1(arg, AS_INTEGER(arg));
    runtimeError(vm, "ord expects an ordinal type argument.");
    return makeInt(0);
}

Value vmBuiltinInc(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "Inc expects 1 or 2 arguments.");
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_POINTER || AS_POINTER(args[0]) == NULL) {
        runtimeError(vm, "First argument to Inc must be a variable (pointer).");
        return makeVoid();
    }

    Value* target = (Value*)AS_POINTER(args[0]);

    long long delta = 1;
    if (arg_count == 2) {
        if (!isOrdinalDelta(&args[1])) {
            runtimeError(vm, "Inc amount must be an ordinal (integer/byte/word/char).");
            return makeVoid();
        }
        delta = coerceDeltaToI64(&args[1]);
    }

    switch (VALUE_TYPE(*target)) {
        case TYPE_INT8: {
            long long next = VAL_INT(*target) + delta;
            if (next < INT8_MIN || next > INT8_MAX) {
                runtimeWarning(vm, "Warning: Range check error incrementing INT8 to %lld.", next);
            }
            SET_INT_VALUE(target, (int8_t)next);
            break;
        }

        case TYPE_UINT8: {
            long long next = VAL_INT(*target) + delta;
            if (next < 0 || next > UINT8_MAX) {
                runtimeWarning(vm, "Warning: Range check error incrementing UINT8 to %lld.", next);
            }
            SET_INT_VALUE(target, (uint8_t)next);
            break;
        }

        case TYPE_INT16: {
            long long next = VAL_INT(*target) + delta;
            if (next < INT16_MIN || next > INT16_MAX) {
                runtimeWarning(vm, "Warning: Range check error incrementing INT16 to %lld.", next);
            }
            SET_INT_VALUE(target, (int16_t)next);
            break;
        }

        case TYPE_UINT16: {
            long long next = VAL_INT(*target) + delta;
            if (next < 0 || next > UINT16_MAX) {
                runtimeWarning(vm, "Warning: Range check error incrementing UINT16 to %lld.", next);
            }
            SET_INT_VALUE(target, (uint16_t)next);
            break;
        }

        case TYPE_INTEGER:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
            SET_INT_VALUE(target, VAL_INT(*target) + delta);
            break;

        case TYPE_BYTE: {
            long long next = VAL_INT(*target) + delta;
            if (next < 0 || next > 255) {
                runtimeWarning(vm, "Warning: Range check error incrementing BYTE to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFF);
            break;
        }

        case TYPE_WORD: {
            long long next = VAL_INT(*target) + delta;
            if (next < 0 || next > 65535) {
                runtimeWarning(vm, "Warning: Range check error incrementing WORD to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFFFF);
            break;
        }

        case TYPE_CHAR: {
            long long next = AS_CHAR(*target) + delta;
            if (next < 0 || next > PASCAL_CHAR_MAX) {
                runtimeWarning(vm, "Warning: Range check error incrementing CHAR to %lld.", next);
            }
            SET_CHAR_VALUE(target, (int)next);
            SET_INT_VALUE(target, AS_CHAR(*target));
            break;
        }

        case TYPE_ENUM:
            // The EnumObj may be shared by reference count with copies of
            // the variable (a by-value parameter): unshare before the
            // in-place write.
            valueEnsureUnique(target);
            AS_ENUM(*target).ordinal += (int)delta;
            break;

        default:
            runtimeError(vm, "Cannot Inc a non-ordinal type.");
            break;
    }

    return makeVoid(); // procedure
}

Value vmBuiltinDec(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "Dec expects 1 or 2 arguments.");
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_POINTER || AS_POINTER(args[0]) == NULL) {
        runtimeError(vm, "First argument to Dec must be a variable (pointer).");
        return makeVoid();
    }

    Value* target = (Value*)AS_POINTER(args[0]);

    long long delta = 1;
    if (arg_count == 2) {
        if (!isOrdinalDelta(&args[1])) {
            runtimeError(vm, "Dec amount must be an ordinal (integer/byte/word/char).");
            return makeVoid();
        }
        delta = coerceDeltaToI64(&args[1]);
    }

    switch (VALUE_TYPE(*target)) {
        case TYPE_INT8: {
            long long next = VAL_INT(*target) - delta;
            if (next < INT8_MIN || next > INT8_MAX) {
                runtimeWarning(vm, "Warning: Range check error decrementing INT8 to %lld.", next);
            }
            SET_INT_VALUE(target, (int8_t)next);
            break;
        }

        case TYPE_UINT8: {
            long long next = VAL_INT(*target) - delta;
            if (next < 0 || next > UINT8_MAX) {
                runtimeWarning(vm, "Warning: Range check error decrementing UINT8 to %lld.", next);
            }
            SET_INT_VALUE(target, (uint8_t)next);
            break;
        }

        case TYPE_INT16: {
            long long next = VAL_INT(*target) - delta;
            if (next < INT16_MIN || next > INT16_MAX) {
                runtimeWarning(vm, "Warning: Range check error decrementing INT16 to %lld.", next);
            }
            SET_INT_VALUE(target, (int16_t)next);
            break;
        }

        case TYPE_UINT16: {
            long long next = VAL_INT(*target) - delta;
            if (next < 0 || next > UINT16_MAX) {
                runtimeWarning(vm, "Warning: Range check error decrementing UINT16 to %lld.", next);
            }
            SET_INT_VALUE(target, (uint16_t)next);
            break;
        }

        case TYPE_INTEGER:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
            SET_INT_VALUE(target, VAL_INT(*target) - delta);
            break;

        case TYPE_BYTE: {
            long long next = VAL_INT(*target) - delta;
            if (next < 0 || next > 255) {
                runtimeWarning(vm, "Warning: Range check error decrementing BYTE to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFF);
            break;
        }

        case TYPE_WORD: {
            long long next = VAL_INT(*target) - delta;
            if (next < 0 || next > 65535) {
                runtimeWarning(vm, "Warning: Range check error decrementing WORD to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFFFF);
            break;
        }

        case TYPE_CHAR: {
            long long next = AS_CHAR(*target) - delta;
            if (next < 0 || next > PASCAL_CHAR_MAX) {
                runtimeWarning(vm, "Warning: Range check error decrementing CHAR to %lld.", next);
            }
            SET_CHAR_VALUE(target, (int)next);
            SET_INT_VALUE(target, AS_CHAR(*target));
            break;
        }

        case TYPE_ENUM:
            valueEnsureUnique(target); // shared EnumObj: see Inc
            AS_ENUM(*target).ordinal -= (int)delta;
            break;

        default:
            runtimeError(vm, "Cannot Dec a non-ordinal type.");
            break;
    }

    return makeVoid(); // procedure
}

typedef struct {
    bool hasBounds;
    bool hitNilPointer;
    int lower;
    int upper;
} ArrayBoundsResult;

static ArrayBoundsResult resolveFirstDimBounds(Value* arg) {
    ArrayBoundsResult result;
    result.hasBounds = false;
    result.hitNilPointer = false;
    result.lower = 0;
    result.upper = -1;

    Value* current = arg;
    for (int depth = 0; depth < 8 && current; ++depth) {
        if (VALUE_TYPE(*current) == TYPE_ARRAY) {
            int lower = 0;
            int upper = -1;
            // `current` here can be a direct alias into a shared global's
            // live Value cell (e.g. Length(sharedDyn) resolving through a
            // GET_GSLOT_ADDRESS-passed pointer), not a private copy. For a
            // dynamic array, resizeDynamicArrayValue's SetLength publish
            // step frees the old ArrayObj (including its lower_bounds/
            // upper_bounds arrays) under dynamic_array_refcount_mutex.
            // Peeking at ARRAY_IS_DYNAMIC(*current) here first (before any
            // lock) would itself race the publish step -- it dereferences
            // *current's live bits pointer same as the bounds read below.
            // copyDynamicArraySnapshotValue() takes the whole-struct copy,
            // the is_dynamic check, and the retain all under one critical
            // section (matching this exact race's existing fix in
            // vm.c's LOAD_ELEMENT_VALUE), so call it unconditionally and
            // only decide dynamic-vs-static from the now-safe local copy.
            Value snapshot = copyDynamicArraySnapshotValue(current);
            bool dynamic_array = ARRAY_IS_DYNAMIC(snapshot);
            Value* bounds_src = dynamic_array ? &snapshot : current;
            if (ARRAY_DIMENSIONS(*bounds_src) > 0 && ARRAY_LOWER_BOUNDS(*bounds_src) && ARRAY_UPPER_BOUNDS(*bounds_src)) {
                lower = ARRAY_LOWER_BOUNDS(*bounds_src)[0];
                upper = ARRAY_UPPER_BOUNDS(*bounds_src)[0];
            } else {
                lower = ARRAY_LOWER_BOUND(*bounds_src);
                upper = ARRAY_UPPER_BOUND(*bounds_src);
            }
            if (dynamic_array) freeValue(&snapshot);
            result.hasBounds = true;
            result.lower = lower;
            result.upper = upper;
            return result;
        }

        if (VALUE_TYPE(*current) != TYPE_POINTER) {
            break;
        }

        if (!AS_POINTER(*current)) {
            result.hitNilPointer = true;
            return result;
        }

        Value* next = (Value*)AS_POINTER(*current);
        if (next == current) {
            break;
        }
        current = next;
    }

    return result;
}

/* The integer type a Low()/High() type-name argument names: a Pascal name
 * (longint, cardinal, shortint, smallint, int64, qword, longword, ...) or
 * the VarType spelling the compiler passes for a variable's type (INT8,
 * UINT16, INT64, UINT64, ...). TYPE_VOID if it names no integer type. */
static VarType lowHighIntegerTypeFromName(const char* name) {
    VarType t = lookupBuiltinPascalTypeName(name);
    if (isIntlikeType(t)) {
        return t;
    }
    static const VarType kIntTypes[] = {
        TYPE_INT8, TYPE_UINT8, TYPE_INT16, TYPE_UINT16, TYPE_INT32,
        TYPE_UINT32, TYPE_INT64, TYPE_UINT64, TYPE_BYTE, TYPE_WORD
    };
    for (size_t i = 0; i < sizeof(kIntTypes) / sizeof(kIntTypes[0]); ++i) {
        if (strcasecmp(name, varTypeToString(kIntTypes[i])) == 0) {
            return kIntTypes[i];
        }
    }
    if (strcasecmp(name, "longword") == 0 || strcasecmp(name, "dword") == 0) return TYPE_UINT32;
    if (strcasecmp(name, "int32") == 0) return TYPE_INT32;
    return TYPE_VOID;
}

/* Low()/High() of the sized integer types; false for any other type. Only
 * Integer, Byte and Word were handled, so High(QWord), Low(Int64),
 * Low(ShortInt) and the like were "not supported". */
static bool lowHighIntegerBound(VarType t, bool high, Value* out) {
    switch (t) {
        case TYPE_INT8:   *out = makeInt(high ? INT8_MAX : INT8_MIN); return true;
        case TYPE_UINT8:  *out = makeInt(high ? UINT8_MAX : 0); return true;
        case TYPE_INT16:  *out = makeInt(high ? INT16_MAX : INT16_MIN); return true;
        case TYPE_UINT16: *out = makeInt(high ? UINT16_MAX : 0); return true;
        case TYPE_UINT32: *out = high ? makeInt64((long long)UINT32_MAX) : makeInt(0); return true;
        case TYPE_INT64:  *out = makeInt64(high ? LLONG_MAX : LLONG_MIN); return true;
        case TYPE_UINT64: *out = makeUInt64(high ? ULLONG_MAX : 0ULL); return true;
        default:          return false;
    }
}

Value vmBuiltinLow(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Low() expects a single array or type identifier argument.");
        return makeInt(0);
    }

    Value arg = args[0];
    const char* typeName = NULL;
    AST* typeDef = NULL;
    VarType t = TYPE_UNKNOWN;

    ArrayBoundsResult bounds = resolveFirstDimBounds(&arg);
    if (bounds.hasBounds) {
        return makeInt(bounds.lower);
    }
    if (bounds.hitNilPointer) {
        runtimeError(vm, "Low() cannot dereference a nil array reference.");
        return makeInt(0);
    }

    // Extract type name or type information from the argument
    if (isPascalStringType(VALUE_TYPE(arg))) {
        typeName = AS_STRING(arg);
    } else if (VALUE_TYPE(arg) == TYPE_ENUM) {
        typeName = AS_ENUM(arg).enum_name;
        t = TYPE_ENUM;
    } else {
        t = VALUE_TYPE(arg);
    }

    // Map the provided name to a VarType if we haven't yet
    if (t == TYPE_UNKNOWN && typeName) {
        if (strcasecmp(typeName, "integer") == 0)      t = TYPE_INTEGER;
        else if (strcasecmp(typeName, "char") == 0)    t = TYPE_CHAR;
        else if (strcasecmp(typeName, "boolean") == 0) t = TYPE_BOOLEAN;
        else if (strcasecmp(typeName, "byte") == 0)    t = TYPE_BYTE;
        else if (strcasecmp(typeName, "word") == 0)    t = TYPE_WORD;
        else if (lowHighIntegerTypeFromName(typeName) != TYPE_VOID) t = lowHighIntegerTypeFromName(typeName);
        else {
            typeDef = lookupType(typeName);
            if (typeDef) t = typeDef->var_type;
        }
    } else if (t == TYPE_ENUM && typeName) {
        typeDef = lookupType(typeName);
    }

    switch (t) {
        case TYPE_INTEGER: return makeInt(-2147483648);
        case TYPE_CHAR:    return makeChar(0);
        case TYPE_BOOLEAN: return makeBoolean(false);
        case TYPE_BYTE:    return makeInt(0);
        case TYPE_WORD:    return makeInt(0);
        case TYPE_ENUM: {
            if (typeDef && typeDef->var_type == TYPE_ENUM && typeName) {
                return makeEnum(typeName, 0);
            }
            break;
        }
        default: {
            Value bound;
            if (lowHighIntegerBound(t, false, &bound)) {
                return bound;
            }
            break;
        }
    }

    if (typeName)
        runtimeError(vm, "Low() not supported for type '%s'.", typeName);
    else
        runtimeError(vm, "Low() not supported for provided type.");
    return makeInt(0);
}

Value vmBuiltinHigh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "High() expects a single array or type identifier argument.");
        return makeInt(0);
    }

    Value arg = args[0];
    const char* typeName = NULL;
    AST* typeDef = NULL;
    VarType t = TYPE_UNKNOWN;

    ArrayBoundsResult bounds = resolveFirstDimBounds(&arg);
    if (bounds.hasBounds) {
        return makeInt(bounds.upper);
    }
    if (bounds.hitNilPointer) {
        runtimeError(vm, "High() cannot dereference a nil array reference.");
        return makeInt(0);
    }

    if (isPascalStringType(VALUE_TYPE(arg))) {
        typeName = AS_STRING(arg);
    } else if (VALUE_TYPE(arg) == TYPE_ENUM) {
        typeName = AS_ENUM(arg).enum_name;
        t = TYPE_ENUM;
    } else {
        t = VALUE_TYPE(arg);
    }

    if (t == TYPE_UNKNOWN && typeName) {
        if (strcasecmp(typeName, "integer") == 0)      t = TYPE_INTEGER;
        else if (strcasecmp(typeName, "char") == 0)    t = TYPE_CHAR;
        else if (strcasecmp(typeName, "boolean") == 0) t = TYPE_BOOLEAN;
        else if (strcasecmp(typeName, "byte") == 0)    t = TYPE_BYTE;
        else if (strcasecmp(typeName, "word") == 0)    t = TYPE_WORD;
        else if (lowHighIntegerTypeFromName(typeName) != TYPE_VOID) t = lowHighIntegerTypeFromName(typeName);
        else {
            typeDef = lookupType(typeName);
            if (typeDef) t = typeDef->var_type;
        }
    } else if (t == TYPE_ENUM && typeName) {
        typeDef = lookupType(typeName);
    }

    switch (t) {
        case TYPE_INTEGER: return makeInt(2147483647);
        case TYPE_CHAR:    return makeChar(PASCAL_CHAR_MAX);
        case TYPE_BOOLEAN: return makeBoolean(true);
        case TYPE_BYTE:    return makeInt(255);
        case TYPE_WORD:    return makeInt(65535);
        case TYPE_ENUM: {
            if (typeDef && typeDef->var_type == TYPE_ENUM && typeName) {
                return makeEnum(typeName, typeDef->child_count - 1);
            }
            break;
        }
        default: {
            Value bound;
            if (lowHighIntegerBound(t, true, &bound)) {
                return bound;
            }
            break;
        }
    }

    if (typeName)
        runtimeError(vm, "High() not supported for type '%s'.", typeName);
    else
        runtimeError(vm, "High() not supported for provided type.");
    return makeInt(0);
}

static AST* gNewIntegerBaseNode = NULL;
static pthread_once_t gNewIntegerBaseOnce = PTHREAD_ONCE_INIT;

static void newIntegerBaseInit(void) {
    Token* baseTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0);
    AST* baseNode = newASTNode(AST_VARIABLE, baseTok);
    setTypeAST(baseNode, TYPE_INT32);
    freeToken(baseTok);
    gNewIntegerBaseNode = baseNode;
}

Value vmBuiltinNew(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_POINTER) {
        runtimeError(vm, "new() expects a single pointer variable argument.");
        return makeVoid();
    }
    
    // The argument on the stack is a pointer TO the pointer variable's Value struct
    Value* pointerVarValuePtr = (Value*)AS_POINTER(args[0]);
    if (!pointerVarValuePtr) {
        runtimeError(vm, "VM internal error: new() received a null LValue pointer.");
        return makeVoid();
    }
    if (VALUE_TYPE(*pointerVarValuePtr) != TYPE_POINTER) {
        runtimeError(vm, "Argument to new() must be of pointer type. Got %s.", varTypeToString(VALUE_TYPE(*pointerVarValuePtr)));
        return makeVoid();
    }

    AST *baseTypeNode = PTR_BASE_TYPE_NODE(*pointerVarValuePtr);
    // (debug logging removed)
    // Determine base type. Default to INTEGER if metadata is unavailable.
    VarType baseVarType = baseTypeNode ? TYPE_VOID : TYPE_INT32;
    AST* actualBaseTypeDef = baseTypeNode;

    if (actualBaseTypeDef && actualBaseTypeDef->type == AST_VARIABLE && actualBaseTypeDef->token) {
        const char* typeName = actualBaseTypeDef->token->value;
        if (strcasecmp(typeName, "integer")==0) { baseVarType=TYPE_INTEGER; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "real")==0 || strcasecmp(typeName, "double")==0) { baseVarType=TYPE_DOUBLE; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "float")==0) { baseVarType=TYPE_FLOAT; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "str")==0 ||
                 strcasecmp(typeName, "string")==0) { baseVarType=TYPE_STRING; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "unicodestring")==0) { baseVarType=TYPE_UNICODE_STRING; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "boolean")==0 || strcasecmp(typeName, "bool")==0) { baseVarType=TYPE_BOOLEAN; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "int")==0) { baseVarType=TYPE_INT32; actualBaseTypeDef = NULL; }
        else {
            baseVarType = lookupBuiltinPascalTypeName(typeName);
            if (baseVarType != TYPE_VOID) {
                actualBaseTypeDef = NULL;
            } else {
                AST* lookedUpType = lookupType(typeName);
                if (!lookedUpType) { runtimeError(vm, "Cannot resolve base type '%s' in new().", typeName); return makeVoid(); }
                actualBaseTypeDef = lookedUpType;
                baseVarType = actualBaseTypeDef->var_type;
            }
        }
    } else if (actualBaseTypeDef) {
         baseVarType = actualBaseTypeDef->var_type;
    }

    if (baseVarType == TYPE_VOID) {
        // Final fallback: allocate as INTEGER
        baseVarType = TYPE_INT32;
        actualBaseTypeDef = NULL;
    }
    
    Value* allocated_memory = malloc(sizeof(Value));
    if (!allocated_memory) { runtimeError(vm, "Memory allocation failed in new()."); return makeVoid(); }
    
    *(allocated_memory) = makeValueForType(baseVarType, actualBaseTypeDef, NULL);
    // (debug logging removed)

    // Update the pointer variable that was passed by reference. It is
    // already TYPE_POINTER (checked above), so it keeps its PointerObj: a
    // SET_VALUE_TYPE here reset .bits to a nil placeholder, which orphaned
    // that PointerObj -- and with it the base type -- so every new() leaked
    // one and then fell into the "no base type" fallback below.
    pscalPointerEnsureObj(pointerVarValuePtr);
    // It no longer points into an array or string it kept alive.
    pscalPointerDropOwners(PSCAL_VALUE_PTR(*pointerVarValuePtr, PointerObj));
    AS_POINTER(*pointerVarValuePtr) = allocated_memory;

    // Safety: if base type metadata is unknown, treat as integer for subsequent
    // dereferences. One shared node, built once: it was a fresh AST per call.
    if (!PTR_BASE_TYPE_NODE(*pointerVarValuePtr)) {
        pthread_once(&gNewIntegerBaseOnce, newIntegerBaseInit);
        PTR_BASE_TYPE_NODE(*pointerVarValuePtr) = gNewIntegerBaseNode;
    }
    // (debug logging removed)

    return makeVoid();
}

// newobj(typeName: string): pointer
Value vmBuiltinNewObj(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0])) || !AS_STRING(args[0])) {
        runtimeError(vm, "newobj expects 1 string type name.");
        return makeNil();
    }
    const char* typeName = AS_STRING(args[0]);
    AST* typeDef = lookupType(typeName);
    if (!typeDef) {
        runtimeError(vm, "newobj: unknown type '%s'", typeName ? typeName : "");
        return makeNil();
    }
    VarType vt = typeDef->var_type;
    Value* allocated = (Value*)malloc(sizeof(Value));
    if (!allocated) { runtimeError(vm, "newobj: allocation failed"); return makeNil(); }
    *allocated = makeValueForType(vt, typeDef, NULL);
    Value ret = makeVoid();
    SET_VALUE_TYPE(&ret, TYPE_POINTER);
    pscalPointerEnsureObj(&ret);
    AS_POINTER(ret) = allocated;
    PTR_BASE_TYPE_NODE(ret) = typeDef;
    return ret;
}

Value vmBuiltinExit(VM* vm, int arg_count, Value* args) {
    if (arg_count > 1 || (arg_count == 1 && !IS_INTLIKE(args[0]))) {
        runtimeError(vm, "exit expects 0 or 1 integer argument.");
        return makeVoid();
    }
    // Signal the VM to unwind the current call frame on return from the builtin
    vm->exit_requested = true;
    return makeVoid();
}

Value vmBuiltinDispose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_POINTER) {
        runtimeError(vm, "dispose() expects a single pointer variable argument.");
        return makeVoid();
    }
    
    Value* pointerVarValuePtr = (Value*)AS_POINTER(args[0]);
    if (!pointerVarValuePtr) {
        runtimeError(vm, "VM internal error: dispose() received a null LValue pointer.");
        return makeVoid();
    }
    if (VALUE_TYPE(*pointerVarValuePtr) != TYPE_POINTER) {
        runtimeError(vm, "Argument to dispose() must be a pointer.");
        return makeVoid();
    }

    Value* valueToDispose = AS_POINTER(*pointerVarValuePtr);
    if (valueToDispose == NULL) {
        // Disposing a nil pointer is a safe no-op.
        return makeVoid();
    }
    if (vmValueIsDisposed(valueToDispose)) {
        runtimeError(vm, "dispose(): the pointer's target was already disposed.");
        return makeVoid();
    }
    
    // Get the address value BEFORE freeing the memory
    uintptr_t disposedAddrValue = (uintptr_t)valueToDispose;
    
    // Free what the cell holds; the cell itself becomes a quarantined
    // tombstone so copies of the pointer that vmNullifyAliases cannot reach
    // (record fields, array elements) fail cleanly instead of using freed
    // memory.
    vmDisposeValueCell(valueToDispose);
    
    // Set the original pointer variable to nil
    AS_POINTER(*pointerVarValuePtr) = NULL;
    
    // Call the new helper to find and nullify any dangling aliases
    vmNullifyAliases(vm, disposedAddrValue);
    
    return makeVoid();
}

Value vmBuiltinAssign(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "Assign requires 2 arguments.");
        return makeVoid();
    }

    // --- FIX: Arguments are in reverse order from the stack ---
    // For assign(f, filename), filename is at args[1] and a pointer to f is at args[0].
    Value fileVarPtr  = args[0];
    Value fileNameVal = args[1];

    if (VALUE_TYPE(fileVarPtr) != TYPE_POINTER || !AS_POINTER(fileVarPtr)) {
        runtimeError(vm, "Assign: First argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(fileVarPtr);

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) {
        runtimeError(vm, "First arg to Assign must be a file variable.");
        return makeVoid();
    }
    if (!isPascalStringType(VALUE_TYPE(fileNameVal))) {
        runtimeError(vm, "Second arg to Assign must be a string. Got type %s.", varTypeToString(VALUE_TYPE(fileNameVal)));
        return makeVoid();
    }

    if (FILE_FILENAME(*fileVarLValue)) {
        free(FILE_FILENAME(*fileVarLValue));
    }

    // Use strdup to create a persistent copy of the filename
    FILE_FILENAME(*fileVarLValue) = AS_STRING(fileNameVal) ? strdup(AS_STRING(fileNameVal)) : NULL;
    if (AS_STRING(fileNameVal) && !FILE_FILENAME(*fileVarLValue)) {
        runtimeError(vm, "Memory allocation failed for filename in assign.");
    }

    return makeVoid();
}

Value vmBuiltinReset(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "Reset requires 1 or 2 arguments.");
        return makeVoid();
    }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "Reset: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(args[0]); // Dereference the pointer

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) { runtimeError(vm, "Argument to Reset must be a file variable."); return makeVoid(); }
    if (FILE_FILENAME(*fileVarLValue) == NULL) { runtimeError(vm, "File variable not assigned a name before Reset."); return makeVoid(); }
    vmMaybeCloseFileValue(fileVarLValue);

    bool has_record_size_arg = (arg_count == 2);
    int new_record_size = FILE_RECORD_SIZE(*fileVarLValue);
    if (has_record_size_arg) {
        if (!IS_INTLIKE(args[1])) {
            runtimeError(vm, "Reset: Record size must be an integer value.");
            return makeVoid();
        }
        long long size_val = AS_INTEGER(args[1]);
        if (size_val <= 0 || size_val > INT_MAX) {
            runtimeError(vm, "Reset: Record size must be between 1 and %d.", INT_MAX);
            return makeVoid();
        }
        new_record_size = (int)size_val;
        FILE_RECORD_SIZE_EXPLICIT(*fileVarLValue) = true;
    } else if (new_record_size <= 0) {
        new_record_size = PSCAL_DEFAULT_FILE_RECORD_SIZE;
        FILE_RECORD_SIZE_EXPLICIT(*fileVarLValue) = false;
    }
    FILE_RECORD_SIZE(*fileVarLValue) = new_record_size;

    bool use_binary_mode = has_record_size_arg || FILE_RECORD_SIZE_EXPLICIT(*fileVarLValue) ||
                           FILE_ELEMENT_TYPE(*fileVarLValue) != TYPE_VOID;
    const char* mode = use_binary_mode ? "rb" : "r";

    FILE* f = fopen(FILE_FILENAME(*fileVarLValue), mode);
    if (f == NULL) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
    AS_FILE(*fileVarLValue) = f;
    return makeVoid();
}

Value vmBuiltinAppend(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Append requires 1 argument."); return makeVoid(); }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "Append: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(args[0]);

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) { runtimeError(vm, "Argument to Append must be a file variable."); return makeVoid(); }
    if (FILE_FILENAME(*fileVarLValue) == NULL) { runtimeError(vm, "File variable not assigned a name before Append."); return makeVoid(); }
    vmMaybeCloseFileValue(fileVarLValue);

    FILE* f = fopen(FILE_FILENAME(*fileVarLValue), "a");
    if (f == NULL) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
    AS_FILE(*fileVarLValue) = f;
    return makeVoid();
}

Value vmBuiltinClose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Close requires 1 argument."); return makeVoid(); }
    
    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "Close: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(args[0]); // Dereference the pointer

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) { runtimeError(vm, "Argument to Close must be a file variable."); return makeVoid(); }
    vmMaybeCloseFileValue(fileVarLValue);
    // Standard Pascal does not de-assign the filename on Close.
    return makeVoid();
}

Value vmBuiltinRename(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "Rename requires 2 arguments."); return makeVoid(); }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "Rename: First argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(args[0]);

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) { runtimeError(vm, "First argument to Rename must be a file variable."); return makeVoid(); }
    if (FILE_FILENAME(*fileVarLValue) == NULL) { runtimeError(vm, "File variable not assigned a name before Rename."); return makeVoid(); }
    if (!isPascalStringType(VALUE_TYPE(args[1]))) { runtimeError(vm, "Second argument to Rename must be a string."); return makeVoid(); }
    vmMaybeCloseFileValue(fileVarLValue);

    int res = rename(FILE_FILENAME(*fileVarLValue), AS_STRING(args[1]));
    if (res != 0) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
        free(FILE_FILENAME(*fileVarLValue));
        FILE_FILENAME(*fileVarLValue) = AS_STRING(args[1]) ? strdup(AS_STRING(args[1])) : NULL;
        if (AS_STRING(args[1]) && !FILE_FILENAME(*fileVarLValue)) {
            runtimeError(vm, "Memory allocation failed for filename in Rename.");
        }
    }
    return makeVoid();
}

Value vmBuiltinErase(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Erase requires 1 argument."); return makeVoid(); }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "Erase: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)AS_POINTER(args[0]);

    if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) { runtimeError(vm, "Argument to Erase must be a file variable."); return makeVoid(); }
    if (FILE_FILENAME(*fileVarLValue) == NULL) { runtimeError(vm, "File variable not assigned a name before Erase."); return makeVoid(); }
    vmMaybeCloseFileValue(fileVarLValue);

    int res = remove(FILE_FILENAME(*fileVarLValue));
    if (res != 0) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
    return makeVoid();
}

Value vmBuiltinFilesize(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "FileSize requires exactly 1 argument.");
        last_io_error = 1;
        return makeInt(0);
    }

    Value *fileValue = NULL;
    if (VALUE_TYPE(args[0]) == TYPE_POINTER && AS_POINTER(args[0])) {
        fileValue = (Value*)AS_POINTER(args[0]);
    } else if (VALUE_TYPE(args[0]) == TYPE_FILE) {
        fileValue = (Value*)&args[0];
    }

    if (!fileValue || VALUE_TYPE(*fileValue) != TYPE_FILE) {
        runtimeError(vm, "FileSize argument must be a file variable.");
        last_io_error = 1;
        return makeInt(0);
    }

    long long sizeBytes = -1;

    if (AS_FILE(*fileValue)) {
        int fd = fileno(AS_FILE(*fileValue));
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0) {
                sizeBytes = (long long)st.st_size;
            }
        }

        if (sizeBytes < 0) {
            errno = 0;
#ifdef _WIN32
            long current = ftell(AS_FILE(*fileValue));
            if (current >= 0L) {
                if (fseek(AS_FILE(*fileValue), 0L, SEEK_END) == 0) {
                    long end = ftell(AS_FILE(*fileValue));
                    if (end >= 0L) {
                        sizeBytes = (long long)end;
                    }
                }
                fseek(AS_FILE(*fileValue), current, SEEK_SET);
            }
#else
            off_t current = ftello(AS_FILE(*fileValue));
            if (current >= (off_t)0) {
                if (fseeko(AS_FILE(*fileValue), 0, SEEK_END) == 0) {
                    off_t end = ftello(AS_FILE(*fileValue));
                    if (end >= (off_t)0) {
                        sizeBytes = (long long)end;
                    }
                }
                fseeko(AS_FILE(*fileValue), current, SEEK_SET);
            }
#endif
        }
    } else if (FILE_FILENAME(*fileValue)) {
        struct stat st;
        if (stat(FILE_FILENAME(*fileValue), &st) == 0) {
            sizeBytes = (long long)st.st_size;
        }
    }

    if (sizeBytes < 0) {
        last_io_error = errno ? errno : 1;
        return makeInt(0);
    }

    last_io_error = 0;

    long long result = sizeBytes;
    int recordSize = FILE_RECORD_SIZE(*fileValue);
    if (recordSize > 0 && (FILE_RECORD_SIZE_EXPLICIT(*fileValue) || FILE_ELEMENT_TYPE(*fileValue) != TYPE_VOID)) {
        result = sizeBytes / recordSize;
    }

    if (result < 0) {
        result = 0;
    }
    if (result > INT_MAX) {
        result = INT_MAX;
    }

    return makeInt(result);
}

Value vmBuiltinEof(VM* vm, int arg_count, Value* args) {
    FILE* stream = NULL;

    if (arg_count == 0) {
        if (vm->vmGlobalSymbols) {
            Symbol* inputSym = hashTableLookup(vm->vmGlobalSymbols, "input");
            if (inputSym && inputSym->value &&
                VALUE_TYPE(*inputSym->value) == TYPE_FILE) {
                stream = AS_FILE(*inputSym->value);
            }
        }
        if (!stream) {
            // No default input file has been opened; treat as EOF
            return makeBoolean(true);
        }
    } else if (arg_count == 1) {
        if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
            runtimeError(vm, "Eof: Argument must be a VAR file parameter.");
            return makeBoolean(true);
        }
        Value* fileVarLValue = (Value*)AS_POINTER(args[0]); // Dereference the pointer
        if (VALUE_TYPE(*fileVarLValue) != TYPE_FILE) {
            runtimeError(vm, "Argument to Eof must be a file variable.");
            return makeBoolean(true);
        }
        if (!AS_FILE(*fileVarLValue)) {
            return makeBoolean(true); // Closed file is treated as EOF
        }
        stream = AS_FILE(*fileVarLValue);
    } else {
        runtimeError(vm, "Eof expects 0 or 1 arguments.");
        return makeBoolean(true);
    }

    int c = fgetc(stream);
    if (c == EOF) {
        return makeBoolean(true);
    }
    ungetc(c, stream); // Push character back
    return makeBoolean(false);
}

/* The most bytes BlockRead/BlockWrite may move through a raw buffer pointer
 * (`s[1]`, `buf[0]`): the rest of the string or packed byte array it points
 * into, rounded down to whole records. The pointer used to carry no bound,
 * so BlockRead(f, s[1], 4096) on an 8-character string overran the heap and
 * BlockWrite read past it into the file. A string's extent is its current
 * length, as in Turbo Pascal (SetLength first to make room); an array
 * element pointer records its extent when the VM creates it. */
static size_t blockIoRawPointerLimit(const Value* pointerValue, const unsigned char* rawPointer,
                                     size_t requestedBytes, int recordSize) {
    if (!pointerValue || !rawPointer) {
        return requestedBytes;
    }
    size_t limit = SIZE_MAX;
    AST* base = PTR_BASE_TYPE_NODE(*pointerValue);
    PointerObj* po = PSCAL_VALUE_PTR(*pointerValue, PointerObj);
    if (base == BYTE_ARRAY_PTR_SENTINEL) {
        if (po && po->extent > 0) {
            limit = po->extent;
        }
    } else if (base == STRING_CHAR_PTR_SENTINEL || base == SERIALIZED_CHAR_PTR_SENTINEL) {
        limit = strlen((const char*)rawPointer);
    }
    if (limit != SIZE_MAX && recordSize > 1) {
        limit -= limit % (size_t)recordSize;
    }
    return requestedBytes < limit ? requestedBytes : limit;
}

Value vmBuiltinBlockread(VM* vm, int arg_count, Value* args) {
    unsigned char* tempBuffer = NULL;
    Value* resultSlot = NULL;
    Value* fileValue = NULL;
    Value* bufferValue = NULL;
    unsigned char* rawPointer = NULL;
    const Value* rawPointerValue = NULL;
    FILE* stream = NULL;
    bool parameterError = false;
    bool performedIO = false;
    bool bufferIsRawPointer = false;
    size_t bytesProcessed = 0;
    long long recordsProcessed = 0;

    last_io_error = 0;

    if (arg_count < 3 || arg_count > 4) {
        runtimeError(vm, "BlockRead requires 3 or 4 arguments.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "BlockRead: first argument must be a VAR file parameter.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    fileValue = (Value*)AS_POINTER(args[0]);
    if (!fileValue || VALUE_TYPE(*fileValue) != TYPE_FILE) {
        runtimeError(vm, "BlockRead: first argument must reference a file variable.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    if (!AS_FILE(*fileValue)) {
        runtimeError(vm, "BlockRead: file is not open.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    stream = AS_FILE(*fileValue);

    if (!IS_INTLIKE(args[2])) {
        runtimeError(vm, "BlockRead: count must be an integer value.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    long long requestedRecords = AS_INTEGER(args[2]);
    if (requestedRecords < 0) {
        requestedRecords = 0;
    }

    if (arg_count == 4) {
        if (VALUE_TYPE(args[3]) != TYPE_POINTER || !AS_POINTER(args[3])) {
            runtimeError(vm, "BlockRead: result argument must be a VAR parameter.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        resultSlot = (Value*)AS_POINTER(args[3]);
    }

    if (VALUE_TYPE(args[1]) != TYPE_POINTER || !AS_POINTER(args[1])) {
        runtimeError(vm, "BlockRead: buffer must be passed by reference.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }

    if (PTR_BASE_TYPE_NODE(args[1]) == STRING_CHAR_PTR_SENTINEL ||
        PTR_BASE_TYPE_NODE(args[1]) == SERIALIZED_CHAR_PTR_SENTINEL ||
        PTR_BASE_TYPE_NODE(args[1]) == BYTE_ARRAY_PTR_SENTINEL) {
        bufferIsRawPointer = true;
        rawPointer = (unsigned char*)AS_POINTER(args[1]);
        rawPointerValue = &args[1];
    } else {
        bufferValue = (Value*)AS_POINTER(args[1]);
        if (bufferValue && VALUE_TYPE(*bufferValue) == TYPE_POINTER &&
            (PTR_BASE_TYPE_NODE(*bufferValue) == STRING_CHAR_PTR_SENTINEL ||
             PTR_BASE_TYPE_NODE(*bufferValue) == SERIALIZED_CHAR_PTR_SENTINEL ||
             PTR_BASE_TYPE_NODE(*bufferValue) == BYTE_ARRAY_PTR_SENTINEL)) {
            bufferIsRawPointer = true;
            rawPointer = (unsigned char*)AS_POINTER(*bufferValue);
            rawPointerValue = bufferValue;
        }
    }

    if (!bufferIsRawPointer) {
        if (!bufferValue || VALUE_TYPE(*bufferValue) != TYPE_ARRAY) {
            runtimeError(vm, "BlockRead: buffer must be an array of byte-sized elements or a character pointer.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
    }

    int recordSize = FILE_RECORD_SIZE(*fileValue) > 0 ? FILE_RECORD_SIZE(*fileValue) : PSCAL_DEFAULT_FILE_RECORD_SIZE;
    if (recordSize <= 0) {
        recordSize = 1;
    }

    unsigned long long requestedRecordsULL = (unsigned long long)requestedRecords;
    unsigned long long requestedBytesULL = requestedRecordsULL * (unsigned long long)recordSize;
    size_t requestedBytes = (requestedBytesULL > SIZE_MAX) ? SIZE_MAX : (size_t)requestedBytesULL;

    if (bufferIsRawPointer) {
        if (!rawPointer && requestedBytes > 0) {
            runtimeError(vm, "BlockRead: buffer pointer is NULL.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        requestedBytes = blockIoRawPointerLimit(rawPointerValue, rawPointer, requestedBytes, recordSize);
        errno = 0;
        performedIO = true;
        bytesProcessed = (requestedBytes > 0) ? fread(rawPointer, 1, requestedBytes, stream) : 0;
        if (bytesProcessed < requestedBytes && ferror(stream)) {
            last_io_error = errno ? errno : 1;
        }
    } else {
        size_t available = 0;
        if (ARRAY_DIMENSIONS(*bufferValue) > 1) {
            runtimeError(vm, "BlockRead: multidimensional arrays are not supported.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        int totalElements = calculateArrayTotalSize(bufferValue);
        if (totalElements > 0) {
            available = (size_t)totalElements;
        }
        size_t requestedRecordsSize = (requestedRecordsULL > (unsigned long long)SIZE_MAX) ? SIZE_MAX : (size_t)requestedRecordsULL;
        size_t bytesToRead = requestedRecordsSize;
        if (bytesToRead > available) {
            bytesToRead = available;
        }
        if (recordSize != 1 && bytesToRead > 0) {
            runtimeError(vm, "BlockRead: record sizes larger than 1 require a raw pointer buffer.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        if (bytesToRead > 0) {
            bool elementByteCompatible =
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_BYTE ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_UINT8 ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_INT8 ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_CHAR ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_BOOLEAN;
            if (!elementByteCompatible && AS_ARRAY(*bufferValue) && available > 0) {
                elementByteCompatible = valueIsByteCompatible(&AS_ARRAY(*bufferValue)[0]);
            }
            if (!elementByteCompatible) {
                runtimeError(vm, "BlockRead: buffer array must contain byte-sized elements.");
                last_io_error = 1;
                parameterError = true;
                goto cleanup;
            }
            // The elements are overwritten in place: unshare a static array
            // that copies still share by reference count (dynamic arrays are
            // reference types and stay shared).
            valueEnsureUnique(bufferValue);
            if (ARRAY_IS_PACKED(*bufferValue) && ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_BYTE) {
                if (!AS_ARRAY_RAW(*bufferValue) && bytesToRead > 0) {
                    runtimeError(vm, "BlockRead: packed byte buffer is NULL.");
                    last_io_error = 1;
                    parameterError = true;
                    goto cleanup;
                }
                errno = 0;
                performedIO = true;
                bytesProcessed = fread(AS_ARRAY_RAW(*bufferValue), 1, bytesToRead, stream);
                if (bytesProcessed < bytesToRead && ferror(stream)) {
                    last_io_error = errno ? errno : 1;
                }
            } else {
                tempBuffer = (unsigned char*)malloc(bytesToRead);
                if (!tempBuffer) {
                    runtimeError(vm, "BlockRead: memory allocation failed.");
                    last_io_error = 1;
                    parameterError = true;
                    goto cleanup;
                }
                errno = 0;
                performedIO = true;
                bytesProcessed = fread(tempBuffer, 1, bytesToRead, stream);
                if (bytesProcessed < bytesToRead && ferror(stream)) {
                    last_io_error = errno ? errno : 1;
                }
                for (size_t i = 0; i < bytesProcessed && AS_ARRAY(*bufferValue); ++i) {
                    assignByteToValue(&AS_ARRAY(*bufferValue)[i], tempBuffer[i]);
                }
            }
        } else {
            performedIO = true;
            bytesProcessed = 0;
        }
    }

    if (recordSize > 0) {
        recordsProcessed = (long long)(bytesProcessed / (size_t)recordSize);
    } else {
        recordsProcessed = (long long)bytesProcessed;
    }

cleanup:
    if (tempBuffer) {
        free(tempBuffer);
        tempBuffer = NULL;
    }

    if (!parameterError) {
        if (!last_io_error && stream && ferror(stream)) {
            last_io_error = errno ? errno : 1;
        } else if (last_io_error != 1) {
            last_io_error = 0;
        }
        if (resultSlot && performedIO) {
            assignCountToResult(resultSlot, recordsProcessed);
        }
    }

    return makeVoid();
}

Value vmBuiltinBlockwrite(VM* vm, int arg_count, Value* args) {
    unsigned char* tempBuffer = NULL;
    Value* fileValue = NULL;
    Value* bufferValue = NULL;
    Value* resultSlot = NULL;
    unsigned char* rawPointer = NULL;
    const Value* rawPointerValue = NULL;
    FILE* stream = NULL;
    bool parameterError = false;
    bool performedIO = false;
    bool bufferIsRawPointer = false;
    size_t bytesProcessed = 0;
    long long recordsProcessed = 0;

    last_io_error = 0;

    if (arg_count < 3 || arg_count > 4) {
        runtimeError(vm, "BlockWrite requires 3 or 4 arguments.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }

    if (VALUE_TYPE(args[0]) != TYPE_POINTER || !AS_POINTER(args[0])) {
        runtimeError(vm, "BlockWrite: first argument must be a VAR file parameter.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    fileValue = (Value*)AS_POINTER(args[0]);
    if (!fileValue || VALUE_TYPE(*fileValue) != TYPE_FILE) {
        runtimeError(vm, "BlockWrite: first argument must reference a file variable.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    if (!AS_FILE(*fileValue)) {
        runtimeError(vm, "BlockWrite: file is not open.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    stream = AS_FILE(*fileValue);

    if (!IS_INTLIKE(args[2])) {
        runtimeError(vm, "BlockWrite: count must be an integer value.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }
    long long requestedRecords = AS_INTEGER(args[2]);
    if (requestedRecords < 0) {
        requestedRecords = 0;
    }

    if (arg_count == 4) {
        if (VALUE_TYPE(args[3]) != TYPE_POINTER || !AS_POINTER(args[3])) {
            runtimeError(vm, "BlockWrite: result argument must be a VAR parameter.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        resultSlot = (Value*)AS_POINTER(args[3]);
    }

    if (VALUE_TYPE(args[1]) != TYPE_POINTER || !AS_POINTER(args[1])) {
        runtimeError(vm, "BlockWrite: buffer must be passed by reference.");
        last_io_error = 1;
        parameterError = true;
        goto cleanup;
    }

    if (PTR_BASE_TYPE_NODE(args[1]) == STRING_CHAR_PTR_SENTINEL ||
        PTR_BASE_TYPE_NODE(args[1]) == SERIALIZED_CHAR_PTR_SENTINEL ||
        PTR_BASE_TYPE_NODE(args[1]) == BYTE_ARRAY_PTR_SENTINEL) {
        bufferIsRawPointer = true;
        rawPointer = (unsigned char*)AS_POINTER(args[1]);
        rawPointerValue = &args[1];
    } else {
        bufferValue = (Value*)AS_POINTER(args[1]);
        if (bufferValue && VALUE_TYPE(*bufferValue) == TYPE_POINTER &&
            (PTR_BASE_TYPE_NODE(*bufferValue) == STRING_CHAR_PTR_SENTINEL ||
             PTR_BASE_TYPE_NODE(*bufferValue) == SERIALIZED_CHAR_PTR_SENTINEL ||
             PTR_BASE_TYPE_NODE(*bufferValue) == BYTE_ARRAY_PTR_SENTINEL)) {
            bufferIsRawPointer = true;
            rawPointer = (unsigned char*)AS_POINTER(*bufferValue);
            rawPointerValue = bufferValue;
        }
    }

    if (!bufferIsRawPointer) {
        if (!bufferValue || VALUE_TYPE(*bufferValue) != TYPE_ARRAY) {
            runtimeError(vm, "BlockWrite: buffer must be an array of byte-sized elements or a character pointer.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
    }

    int recordSize = FILE_RECORD_SIZE(*fileValue) > 0 ? FILE_RECORD_SIZE(*fileValue) : PSCAL_DEFAULT_FILE_RECORD_SIZE;
    if (recordSize <= 0) {
        recordSize = 1;
    }

    unsigned long long requestedRecordsULL = (unsigned long long)requestedRecords;
    unsigned long long requestedBytesULL = requestedRecordsULL * (unsigned long long)recordSize;
    size_t requestedBytes = (requestedBytesULL > SIZE_MAX) ? SIZE_MAX : (size_t)requestedBytesULL;

    if (bufferIsRawPointer) {
        if (!rawPointer && requestedBytes > 0) {
            runtimeError(vm, "BlockWrite: buffer pointer is NULL.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        requestedBytes = blockIoRawPointerLimit(rawPointerValue, rawPointer, requestedBytes, recordSize);
        errno = 0;
        performedIO = true;
        bytesProcessed = (requestedBytes > 0) ? fwrite(rawPointer, 1, requestedBytes, stream) : 0;
        if (bytesProcessed < requestedBytes && ferror(stream)) {
            last_io_error = errno ? errno : 1;
        }
    } else {
        size_t available = 0;
        if (ARRAY_DIMENSIONS(*bufferValue) > 1) {
            runtimeError(vm, "BlockWrite: multidimensional arrays are not supported.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        int totalElements = calculateArrayTotalSize(bufferValue);
        if (totalElements > 0) {
            available = (size_t)totalElements;
        }
        size_t requestedRecordsSize = (requestedRecordsULL > (unsigned long long)SIZE_MAX) ? SIZE_MAX : (size_t)requestedRecordsULL;
        size_t bytesToWrite = requestedRecordsSize;
        if (bytesToWrite > available) {
            bytesToWrite = available;
        }
        if (recordSize != 1 && bytesToWrite > 0) {
            runtimeError(vm, "BlockWrite: record sizes larger than 1 require a raw pointer buffer.");
            last_io_error = 1;
            parameterError = true;
            goto cleanup;
        }
        if (bytesToWrite > 0) {
            bool elementByteCompatible =
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_BYTE ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_UINT8 ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_INT8 ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_CHAR ||
                ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_BOOLEAN;
            if (!elementByteCompatible && AS_ARRAY(*bufferValue) && available > 0) {
                elementByteCompatible = valueIsByteCompatible(&AS_ARRAY(*bufferValue)[0]);
            }
            if (!elementByteCompatible) {
                runtimeError(vm, "BlockWrite: buffer array must contain byte-sized elements.");
                last_io_error = 1;
                parameterError = true;
                goto cleanup;
            }
            if (ARRAY_IS_PACKED(*bufferValue) && ARRAY_ELEMENT_TYPE(*bufferValue) == TYPE_BYTE) {
                if (!AS_ARRAY_RAW(*bufferValue) && bytesToWrite > 0) {
                    runtimeError(vm, "BlockWrite: packed byte buffer is NULL.");
                    last_io_error = 1;
                    parameterError = true;
                    goto cleanup;
                }
                errno = 0;
                performedIO = true;
                bytesProcessed = fwrite(AS_ARRAY_RAW(*bufferValue), 1, bytesToWrite, stream);
                if (bytesProcessed < bytesToWrite && ferror(stream)) {
                    last_io_error = errno ? errno : 1;
                }
            } else {
                tempBuffer = (unsigned char*)malloc(bytesToWrite);
                if (!tempBuffer) {
                    runtimeError(vm, "BlockWrite: memory allocation failed.");
                    last_io_error = 1;
                    parameterError = true;
                    goto cleanup;
                }
                for (size_t i = 0; i < bytesToWrite && AS_ARRAY(*bufferValue); ++i) {
                    tempBuffer[i] = valueToByte(&AS_ARRAY(*bufferValue)[i]);
                }
                errno = 0;
                performedIO = true;
                bytesProcessed = fwrite(tempBuffer, 1, bytesToWrite, stream);
                if (bytesProcessed < bytesToWrite && ferror(stream)) {
                    last_io_error = errno ? errno : 1;
                }
            }
        } else {
            performedIO = true;
            bytesProcessed = 0;
        }
    }

    if (recordSize > 0) {
        recordsProcessed = (long long)(bytesProcessed / (size_t)recordSize);
    } else {
        recordsProcessed = (long long)bytesProcessed;
    }

cleanup:
    if (tempBuffer) {
        free(tempBuffer);
        tempBuffer = NULL;
    }

    if (!parameterError) {
        if (!last_io_error && stream && ferror(stream)) {
            last_io_error = errno ? errno : 1;
        } else if (last_io_error != 1) {
            last_io_error = 0;
        }
        if (resultSlot && performedIO) {
            assignCountToResult(resultSlot, recordsProcessed);
        }
    }

    return makeVoid();
}

/* The value range of an integer Read/Readln/Val destination. */
static void builtinIntTypeRange(VarType type, long long* lo, unsigned long long* hi) {
    switch (type) {
        case TYPE_INT8:   *lo = INT8_MIN;  *hi = INT8_MAX;   break;
        case TYPE_INT16:  *lo = INT16_MIN; *hi = INT16_MAX;  break;
        case TYPE_INT32:  *lo = INT32_MIN; *hi = INT32_MAX;  break;
        case TYPE_UINT8:
        case TYPE_BYTE:   *lo = 0;         *hi = UINT8_MAX;  break;
        case TYPE_UINT16:
        case TYPE_WORD:   *lo = 0;         *hi = UINT16_MAX; break;
        case TYPE_UINT32: *lo = 0;         *hi = UINT32_MAX; break;
        case TYPE_UINT64: *lo = 0;         *hi = UINT64_MAX; break;
        default:          *lo = LLONG_MIN; *hi = LLONG_MAX;  break;
    }
}

/* Parse the decimal integer at `text` (leading whitespace skipped) for a
 * destination of type `type`. False when there is no number or it does not
 * fit the destination; SET_INT_VALUE would otherwise wrap it silently
 * (99999999999 read into an Integer came back as 1215752191). On success
 * *endp points past the digits. */
static bool builtinParseIntForType(VarType type, const char* text, const char** endp, long long* out) {
    long long lo;
    unsigned long long hi;
    builtinIntTypeRange(type, &lo, &hi);
    const char* p = text;
    while (isspace((unsigned char)*p)) p++;
    char* e = NULL;
    errno = 0;
    if (lo == 0 && *p != '-') {
        unsigned long long v = strtoull(p, &e, 10);
        if (e == p || errno == ERANGE || v > hi) return false;
        *out = (long long)v;
    } else {
        long long v = strtoll(p, &e, 10);
        if (e == p || errno == ERANGE || v < lo || (v > 0 && (unsigned long long)v > hi)) return false;
        *out = v;
    }
    *endp = e;
    return true;
}

/* Turbo/Free Pascal's IOResult for text that is not a number of the
 * destination's type. */
#define VM_IO_INVALID_NUMERIC_FORMAT 106

/* The next whitespace-delimited token of `stream`, in a buffer grown to fit
 * (Read used to cut tokens at 1023 bytes and return the rest as the next
 * token). The caller frees *out. False at end of input. */
static bool readTokenAlloc(FILE* stream, char** out, size_t* out_len) {
    int ch;
    do {
        ch = fgetc(stream);
    } while (ch != EOF && isspace(ch));
    if (ch == EOF) {
        return false;
    }
    size_t len = 0, cap = 64;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        return false;
    }
    while (ch != EOF && !isspace(ch)) {
        if (len + 1 >= cap) {
            char* grown = (char*)realloc(buf, cap * 2);
            if (!grown) {
                free(buf);
                return false;
            }
            buf = grown;
            cap *= 2;
        }
        buf[len++] = (char)ch;
        ch = fgetc(stream);
    }
    if (ch != EOF) {
        ungetc(ch, stream);
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return true;
}

Value vmBuiltinRead(VM* vm, int arg_count, Value* args) {
    FILE* input_stream = stdin;
    int var_start_index = 0;
    int io_error = 0;
    bool binary_file = false;
    VarType binary_element_type = TYPE_VOID;
    size_t binary_element_size = 0;

    // Determine input stream: allow FILE or ^FILE
    if (arg_count > 0) {
        const Value* a0 = &args[0];
        if (VALUE_TYPE(*a0) == TYPE_POINTER && AS_POINTER(*a0)) a0 = (const Value*)AS_POINTER(*a0);
        if (VALUE_TYPE(*a0) == TYPE_FILE) {
            if (!AS_FILE(*a0)) {
                runtimeError(vm, "File not open for Read.");
                io_error = 1;
                vmCommitLastIoError(io_error);
                return makeVoid();
            }
            input_stream = AS_FILE(*a0);
            var_start_index = 1;
            // A typed file of a scalar type holds its elements in the binary
            // layout Write gives them (same test as Write's).
            if (FILE_ELEMENT_TYPE(*a0) != TYPE_VOID && FILE_ELEMENT_TYPE(*a0) != TYPE_UNKNOWN &&
                (FILE_RECORD_SIZE_EXPLICIT(*a0) || FILE_ELEMENT_TYPE_DEF(*a0) != NULL)) {
                long long size_bytes = 0;
                if (builtinSizeForVarType(FILE_ELEMENT_TYPE(*a0), &size_bytes) && size_bytes > 0 &&
                    (size_t)size_bytes <= sizeof(long double)) {
                    binary_file = true;
                    binary_element_type = FILE_ELEMENT_TYPE(*a0);
                    binary_element_size = (size_t)size_bytes;
                }
            }
        }
    }

    if (input_stream == stdin) {
        vmPrepareCanonicalInput();
    }

    for (int i = var_start_index; i < arg_count; i++) {
        if (VALUE_TYPE(args[i]) != TYPE_POINTER || !AS_POINTER(args[i])) {
            runtimeError(vm, "Read requires VAR parameters to read into.");
            io_error = 1;
            break;
        }
        Value* dst = (Value*)AS_POINTER(args[i]);

        if (binary_file) {
            int read_error = 0;
            if (!readBinaryElement(input_stream, dst, binary_element_type, binary_element_size, &read_error)) {
                io_error = read_error ? read_error : 1;
                break;
            }
            continue;
        }
        if (VALUE_TYPE(*dst) == TYPE_CHAR) {
            int ch = fgetc(input_stream);
            if (ch == EOF) { io_error = feof(input_stream) ? 0 : 1; break; }
            SET_CHAR_VALUE(dst, ch);
            SET_INT_VALUE(dst, AS_CHAR(*dst));
            continue;
        }
        if (VALUE_TYPE(*dst) == TYPE_RECORD || VALUE_TYPE(*dst) == TYPE_ARRAY) {
            if (!readStructuredValue(input_stream, dst)) {
                io_error = 1;
                break;
            }
            continue;
        }

        char* buffer = NULL;
        size_t buffer_len = 0;
        if (!readTokenAlloc(input_stream, &buffer, &buffer_len)) {
            io_error = feof(input_stream) ? 0 : 1;
            break;
        }

        // A number is the whole token: "12x" is not 12.
        switch (VALUE_TYPE(*dst)) {
            case TYPE_INT8:
            case TYPE_INT16:
            case TYPE_INT32: /* TYPE_INTEGER */
            case TYPE_INT64:
            case TYPE_UINT8:
            case TYPE_BYTE:
            case TYPE_UINT16:
            case TYPE_WORD:
            case TYPE_UINT32:
            case TYPE_UINT64: {
                const char* endp = NULL;
                long long v = 0;
                if (!builtinParseIntForType(VALUE_TYPE(*dst), buffer, &endp, &v) || *endp != '\0') {
                    io_error = VM_IO_INVALID_NUMERIC_FORMAT;
                    break;
                }
                SET_INT_VALUE(dst, v);
                break;
            }
            case TYPE_FLOAT:
            case TYPE_DOUBLE: /* TYPE_REAL */
            case TYPE_LONG_DOUBLE: {
                errno = 0;
                char* endp = NULL;
                long double v = strtold(buffer, &endp);
                if (endp == buffer || *endp != '\0' || errno == ERANGE) {
                    io_error = VM_IO_INVALID_NUMERIC_FORMAT;
                    break;
                }
                SET_REAL_VALUE(dst, v);
                break;
            }
            case TYPE_BOOLEAN: {
                if (strcasecmp(buffer, "true") == 0 || strcmp(buffer, "1") == 0) {
                    SET_INT_VALUE(dst, 1);
                } else if (strcasecmp(buffer, "false") == 0 || strcmp(buffer, "0") == 0) {
                    SET_INT_VALUE(dst, 0);
                } else {
                    SET_INT_VALUE(dst, 0);
                    io_error = 1;
                }
                break;
            }
            case TYPE_STRING:
            case TYPE_UNICODE_STRING:
            case TYPE_NIL: {
                // A fresh StringObj, the old one released (see
                // builtinRebindStringValue); it used to be orphaned.
                if (!builtinRebindStringValue(dst, buffer, buffer_len)) {
                    io_error = 1;
                }
                break;
            }
            default:
                runtimeError(vm, "Cannot Read into a variable of type %s.", varTypeToString(VALUE_TYPE(*dst)));
                io_error = 1;
                i = arg_count;
                break;
        }
        free(buffer);
        if (io_error) {
            break;
        }
    }

    if (!io_error && ferror(input_stream)) io_error = 1;

    if (input_stream == stdin) {
        vmEnableRawMode();
    }

    vmCommitLastIoError(io_error);
    return makeVoid();
}

Value vmBuiltinReadln(VM* vm, int arg_count, Value* args) {
    fflush(stdout);
    FILE* input_stream = stdin;
    int var_start_index = 0;
    // Clear any previous IO error so a successful read won't report stale
    // failures from earlier operations (e.g. failed parse on a prior call).
    int io_error = 0;

    // 1) Determine input stream: allow FILE or ^FILE
    if (arg_count > 0) {
        const Value* a0 = &args[0];
        if (VALUE_TYPE(*a0) == TYPE_POINTER && AS_POINTER(*a0)) a0 = (const Value*)AS_POINTER(*a0);
        if (VALUE_TYPE(*a0) == TYPE_FILE) {
            if (!AS_FILE(*a0)) {
                runtimeError(vm, "File not open for Readln.");
                io_error = 1;
                vmCommitLastIoError(io_error);
                return makeVoid();
            }
            input_stream = AS_FILE(*a0);
            var_start_index = 1;
        }
    }

    if (input_stream == stdin) {
        vmPrepareCanonicalInput();
    }

    // 2) Read full line
    VmLineBuf line = {0};
    if (!vmReadLineInterruptible(vm, input_stream, &line)) {
        free(line.data);
        io_error = feof(input_stream) ? 0 : 1;
        if (getenv("PSCALI_TOOL_DEBUG")) {
            fprintf(stderr,
                    "[readln] interrupted io_error=%d abort=%d exit=%d suspend=%d\n",
                    io_error,
                    vm ? (int)vm->abort_requested : -1,
                    vm ? (int)vm->exit_requested : -1,
                    vm ? (int)vm->suspend_unwind_requested : -1);
        }
        if (input_stream == stdin) vmEnableRawMode();
        vmCommitLastIoError(io_error);
        return makeVoid();
    }
    char* line_buffer = line.data;
    line_buffer[strcspn(line_buffer, "\r\n")] = '\0';

    // 3) Parse vars from buffer
    const char* p = line_buffer;
    for (int i = var_start_index; i < arg_count; i++) {
        if (VALUE_TYPE(args[i]) != TYPE_POINTER || !AS_POINTER(args[i])) {
            runtimeError(vm, "Readln requires VAR parameters to read into.");
            io_error = 1;
            break;
        }
        Value* dst = (Value*)AS_POINTER(args[i]);

        while (isspace((unsigned char)*p)) p++;

        // If the destination has not been initialized, default it to an empty
        // string so that Readln can populate it.  Variables declared as
        // strings start out with the TYPE_NIL tag which caused the builtin to
        // reject them.  Treating TYPE_NIL as a string matches Pascal's
        // semantics where uninitialised strings can be read into directly.
        if (VALUE_TYPE(*dst) == TYPE_NIL) {
            SET_VALUE_TYPE(dst, TYPE_STRING);
            pscalStringEnsureObj(dst); // was TYPE_NIL, no wrapper yet
            AS_STRING(*dst) = NULL;
        }

        switch (VALUE_TYPE(*dst)) {
            case TYPE_INT8:
            case TYPE_INT16:
            case TYPE_INT32: /* TYPE_INTEGER */
            case TYPE_INT64:
            case TYPE_UINT8:
            case TYPE_BYTE:
            case TYPE_UINT16:
            case TYPE_WORD:
            case TYPE_UINT32:
            case TYPE_UINT64: {
                // The number may be followed directly by the next field
                // ("42abc" reads 42, then "abc"), but it must fit the
                // variable: out of range is an error, not a wrapped value.
                const char* endp = p;
                long long v = 0;
                if (!builtinParseIntForType(VALUE_TYPE(*dst), p, &endp, &v)) {
                    io_error = VM_IO_INVALID_NUMERIC_FORMAT;
                    v = 0;
                    endp = p;
                }
                SET_INT_VALUE(dst, v);
                p = endp;
                break;
            }
            case TYPE_FLOAT:
            case TYPE_DOUBLE: /* TYPE_REAL */
            case TYPE_LONG_DOUBLE: {
                errno = 0;
                char* endp = NULL;
                long double v = strtold(p, &endp);
                if (endp == p || errno == ERANGE) { io_error = VM_IO_INVALID_NUMERIC_FORMAT; v = 0.0; endp = (char*)p; }
                SET_REAL_VALUE(dst, v);
                p = endp ? endp : p;
                break;
            }
            case TYPE_BOOLEAN: {
                if (strncasecmp(p, "true", 4) == 0) {
                    SET_INT_VALUE(dst, 1);
                    p += 4;
                } else if (strncasecmp(p, "false", 5) == 0) {
                    SET_INT_VALUE(dst, 0);
                    p += 5;
                } else {
                    errno = 0;
                    char* endp = NULL;
                    long long v = strtoll(p, &endp, 10);
                    if (endp == p || errno == ERANGE) { io_error = 1; v = 0; }
                    SET_INT_VALUE(dst, v ? 1 : 0);
                    p = endp ? endp : p;
                }
                break;
            }
            case TYPE_CHAR: {
                if (*p) {
                SET_CHAR_VALUE(dst, (unsigned char)*p++);
                SET_INT_VALUE(dst, AS_CHAR(*dst));
                } else {
                    SET_CHAR_VALUE(dst, '\0');
                    SET_INT_VALUE(dst, 0);
                    io_error = 1;
                }
                break;
            }

            case TYPE_STRING:
            case TYPE_UNICODE_STRING: {
                // Rebind to a FRESH StringObj rather than overwriting the one
                // already attached.
                //
                // Copying a string Value copies the StringObj pointer, so any
                // number of Values can share one object. Reusing it here --
                // free(old buffer), install the new one, re-tag the same
                // wrapper -- mutated every one of those sharers at once. In
                // Aether that made the ordinary read-and-keep loop silently
                // wrong:
                //
                //     readln(f, line); a = line;   // "one"
                //     readln(f, line); b = line;   // "two"
                //     readln(f, line);             // "three"
                //     -> a, b and line all read "three"
                //
                // Reading a file into an array produced N copies of the last
                // line, with no error. Allocating a new object leaves earlier
                // copies pointing at the value they were given, which is what
                // plain assignment already does (`s = t;` then `t = "x";`
                // never disturbed `s`). builtinRebindStringValue releases
                // this variable's reference to the previous object (it used
                // to leak, one object per line read) and keeps a string[N]
                // limit.
                if (!builtinRebindStringValue(dst, p, strlen(p))) {
                    runtimeError(vm, "Out of memory in Readln.");
                    io_error = 1;
                    break;
                }
                i = arg_count; // consume the line; ignore trailing params
                break;
            }

            default:
                runtimeError(vm, "Cannot Readln into a variable of type %s.", varTypeToString(VALUE_TYPE(*dst)));
                io_error = 1;
                i = arg_count;
                break;
        }
    }

    free(line.data);
    if (!io_error && ferror(input_stream)) io_error = 1;

    if (input_stream == stdin) {
        vmEnableRawMode();
    }

    vmCommitLastIoError(io_error);
    return makeVoid();
}

static bool vmTraceStdoutEnabled(void) {
    static int trace_mode = -1;
    if (trace_mode < 0) {
        const char *env = getenv("REA_TRACE_STDOUT");
        if (!env || !*env) {
            env = getenv("PSCAL_TRACE_STDOUT");
        }
        trace_mode = (env && *env && env[0] != '0') ? 1 : 0;
    }
    return trace_mode == 1;
}

static void vmTraceDescribeValue(const Value *val) {
    if (!val) {
        fprintf(stderr, "  [TRACE stdout] <null value>\n");
        return;
    }
    switch (VALUE_TYPE(*val)) {
        case TYPE_STRING:
            fprintf(stderr, "  [TRACE stdout] string: \"%.*s\"\n",
                    80, AS_STRING(*val) ? AS_STRING(*val) : "");
            break;
        case TYPE_CHAR:
            fprintf(stderr, "  [TRACE stdout] char: %d\n", AS_CHAR(*val));
            break;
        case TYPE_BOOLEAN:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_WORD:
        case TYPE_BYTE:
            fprintf(stderr, "  [TRACE stdout] int: %lld\n", (long long)AS_INTEGER(*val));
            break;
        case TYPE_REAL:
        case TYPE_FLOAT:
            fprintf(stderr, "  [TRACE stdout] real: %Lf\n", (long double)AS_REAL(*val));
            break;
        default:
            fprintf(stderr, "  [TRACE stdout] value type %d\n", VALUE_TYPE(*val));
            break;
    }
}

// One write/writeln call is one locked stdio transaction, so concurrent
// threads (Aether `par` branches, Rea/Pascal threads) never interleave inside
// a line. The stdio locks are recursive, so the fputs/fwrite/fprintf calls
// made while the lock is held still take it themselves without deadlocking.
// On POSIX the unlock is also a cancellation cleanup handler: a worker
// thread cancelled inside the write (iOS in-process background jobs are
// stopped with pthread_cancel) must not leave the stream locked for good.
#if !defined(_WIN32) || defined(__CYGWIN__)
static void vmWriteUnlockStream(void *stream) {
    funlockfile((FILE *)stream);
}

/* read_line() -> Text: the next line of stdin without its line break, or ""
 * at end of input. read_all_stdin() -> Text: the rest of stdin, each line
 * ending in "\n". Aether's replacements for readln (D60); both go through
 * readln's own interruptible line reader, so ^C and the iOS session stdin
 * behave the same. Lines have no length limit (they were split at 1 MiB). */
static bool vmReadStdinLine(VM *vm, VmLineBuf *line) {
    bool ok = vmReadLineInterruptible(vm, stdin, line);
    vmCommitLastIoError(ok || feof(stdin) ? 0 : 1);
    return ok;
}

static Value vmBuiltinReadLine(VM *vm, int arg_count, Value *args) {
    (void)args;
    fflush(stdout);
    if (arg_count != 0) {
        runtimeError(vm, "read_line expects no arguments.");
        return makeUnicodeString("");
    }
    VmLineBuf line = {0};
    vmPrepareCanonicalInput();
    bool ok = vmReadStdinLine(vm, &line);
    vmEnableRawMode();
    Value v = ok ? makeUnicodeString(line.data) : makeUnicodeString("");
    free(line.data);
    return v;
}

static Value vmBuiltinReadAllStdin(VM *vm, int arg_count, Value *args) {
    (void)args;
    fflush(stdout);
    if (arg_count != 0) {
        runtimeError(vm, "read_all_stdin expects no arguments.");
        return makeUnicodeString("");
    }
    char *acc = NULL;
    size_t len = 0, cap = 0;
    VmLineBuf line = {0};
    // The terminal is set up once for the whole read, not once per line:
    // the per-line tcsetattr/tcflush (and a 1 MiB buffer per line) made
    // reading 500k short lines take 13 s.
    vmPrepareCanonicalInput();
    while (vmReadStdinLine(vm, &line)) {
        size_t n = strlen(line.data);
        if (len + n + 2 > cap) {
            size_t ncap = cap ? cap : 4096;
            while (ncap < len + n + 2) ncap *= 2;
            char *nb = (char *)realloc(acc, ncap);
            if (!nb) {
                free(line.data);
                free(acc);
                vmEnableRawMode();
                runtimeError(vm, "read_all_stdin: out of memory.");
                return makeUnicodeString("");
            }
            acc = nb;
            cap = ncap;
        }
        memcpy(acc + len, line.data, n);
        len += n;
        acc[len++] = '\n';
        acc[len] = '\0';
        if (vm && (vm->abort_requested || vm->exit_requested)) break;
    }
    vmEnableRawMode();
    free(line.data);
    Value v = makeUnicodeStringLen(acc ? acc : "", len);
    free(acc);
    return v;
}
#endif

Value vmBuiltinWrite(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1) {
        runtimeError(vm, "Write expects at least a newline flag.");
        return makeVoid();
    }

    bool newline = false;
    bool suppress_spacing = (gSuppressWriteSpacing != 0);
    bool suppress_spacing_flag = false;
    last_io_error = 0;
    Value flag = args[0];
    if (isRealType(VALUE_TYPE(flag))) {
        newline = (AS_REAL(flag) != 0.0);
    } else if (IS_INTLIKE(flag)) {
        long long raw = AS_INTEGER(flag);
        newline = (raw & VM_WRITE_FLAG_NEWLINE) != 0;
        suppress_spacing_flag = ((raw & VM_WRITE_FLAG_SUPPRESS_SPACING) != 0);
        suppress_spacing = suppress_spacing || suppress_spacing_flag;
    } else if (VALUE_TYPE(flag) == TYPE_BOOLEAN) {
        newline = VAL_INT(flag) != 0;
    } else if (VALUE_TYPE(flag) == TYPE_CHAR) {
        newline = AS_CHAR(flag) != 0;
    }

    FILE* output_stream = stdout;
    int start_index = 1;
    const Value* file_value = NULL;
    bool binary_file = false;
    VarType binary_element_type = TYPE_VOID;
    size_t binary_element_size = 0;

    if (arg_count > 1) {
        const Value* first = &args[1];
        if (VALUE_TYPE(*first) == TYPE_POINTER && AS_POINTER(*first)) first = (const Value*)AS_POINTER(*first);
        if (VALUE_TYPE(*first) == TYPE_FILE) {
            if (!AS_FILE(*first)) {
                runtimeError(vm, "File not open for writing.");
                return makeVoid();
            }
            output_stream = AS_FILE(*first);
            start_index = 2;
            file_value = first;
            if (FILE_ELEMENT_TYPE(*file_value) != TYPE_VOID && FILE_ELEMENT_TYPE(*file_value) != TYPE_UNKNOWN) {
                bool has_typed_metadata = FILE_RECORD_SIZE_EXPLICIT(*file_value) || FILE_ELEMENT_TYPE_DEF(*file_value) != NULL;
                if (has_typed_metadata) {
                    long long size_bytes = 0;
                    if (builtinSizeForVarType(FILE_ELEMENT_TYPE(*file_value), &size_bytes) && size_bytes > 0 &&
                        (size_t)size_bytes <= sizeof(long double)) {
                        binary_file = true;
                        binary_element_type = FILE_ELEMENT_TYPE(*file_value);
                        binary_element_size = (size_t)size_bytes;
                    }
                }
            }
        }
    }

    if (binary_file) {
        suppress_spacing = true;
    }

    int print_arg_count = arg_count - start_index;
    if (print_arg_count > MAX_WRITE_ARGS_VM) {
        runtimeError(vm, "VM Error: Too many arguments for WRITE/WRITELN (max %d).", MAX_WRITE_ARGS_VM);
        return makeVoid();
    }

    bool trace_stdout = vmTraceStdoutEnabled() && (output_stream == stdout);
    if (trace_stdout) {
        fprintf(stderr, "[TRACE stdout] write call: newline=%d args=%d\n", newline ? 1 : 0, print_arg_count);
    }
    // Held from the colour prefix through the newline, the colour reset and
    // the file flush: every byte this call writes lands as one unit.
#if defined(_WIN32) && !defined(__CYGWIN__)
    _lock_file(output_stream);
#else
    flockfile(output_stream);
    pthread_cleanup_push(vmWriteUnlockStream, output_stream);
#endif
    bool color_was_applied = false;
    if (output_stream == stdout) {
        color_was_applied = applyCurrentTextAttributes(output_stream);
    }

    Value prev = makeVoid();
    bool has_prev = false;
    for (int i = start_index; i < arg_count; i++) {
        Value val = args[i];
        if (binary_file) {
            int write_error = 0;
            if (!writeBinaryElement(output_stream, &val, binary_element_type, binary_element_size, &write_error)) {
                last_io_error = write_error != 0 ? write_error : 1;
                break;
            }
            if (trace_stdout) {
                vmTraceDescribeValue(&val);
            }
            prev = val;
            has_prev = true;
            continue;
        }
        if (!suppress_spacing && has_prev) {
            bool add_space = true;
            const char *no_space_after = "=,.;:?!-)]}>)\"'";
            if (VALUE_TYPE(prev) == TYPE_STRING && AS_STRING(prev)) {
                size_t len = strlen(AS_STRING(prev));
                if (len == 0) {
                    add_space = false;
                } else {
                    char last = AS_STRING(prev)[len - 1];
                    if (isspace((unsigned char)last) || strchr(no_space_after, last)) {
                        add_space = false;
                    }
                }
            } else if (VALUE_TYPE(prev) == TYPE_CHAR) {
                char last = (char)AS_CHAR(prev);
                if (isspace((unsigned char)last) || strchr(no_space_after, last)) {
                    add_space = false;
                }
            }
            if (VALUE_TYPE(val) == TYPE_STRING && AS_STRING(val)) {
                size_t len_cur = strlen(AS_STRING(val));
                if (len_cur > 0) {
                    char first = AS_STRING(val)[0];
                    if (isspace((unsigned char)first) || strchr(",.;:)]}!?)", first)) {
                        add_space = false;
                    }
                }
            } else if (VALUE_TYPE(val) == TYPE_CHAR) {
                char first = (char)AS_CHAR(val);
                if (isspace((unsigned char)first) || strchr(",.;:)]}!?", first)) {
                    add_space = false;
                }
            }
            if (add_space) {
                fputc(' ', output_stream);
            }
        }
        if (trace_stdout) {
            vmTraceDescribeValue(&val);
        }
        if (suppress_spacing_flag && VALUE_TYPE(val) == TYPE_BOOLEAN) {
            fputs(VAL_INT(val) ? "1" : "0", output_stream);
        } else if (VALUE_TYPE(val) == TYPE_STRING) {
            const char *text = AS_STRING(val) ? AS_STRING(val) : "";
            writePascalText(output_stream, text, strlen(text));
        } else if (VALUE_TYPE(val) == TYPE_CHAR) {
            char utf8[5];
            size_t utf8_len = encodePascalCharUtf8(AS_CHAR(val), utf8);
            fwrite(utf8, 1, utf8_len, output_stream);
        } else if (output_stream != stdout && (VALUE_TYPE(val) == TYPE_RECORD || VALUE_TYPE(val) == TYPE_ARRAY)) {
            if (!writeStructuredValue(output_stream, &val)) {
                runtimeError(vm, "Cannot write structured value to file.");
                last_io_error = 1;
                break;
            }
        } else {
            printValueToStream(val, output_stream);
        }
        prev = val;
        has_prev = true;
    }

    if (newline && !binary_file) {
        fprintf(output_stream, "\n");
    }

    if (color_was_applied) {
        resetTextAttributes(output_stream);
    }

    if (output_stream != stdout) {
        fflush(output_stream);
    }
#if defined(_WIN32) && !defined(__CYGWIN__)
    _unlock_file(output_stream);
#else
    pthread_cleanup_pop(1);
#endif

    return makeVoid();
}


Value vmBuiltinIoresult(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "IOResult requires 0 arguments."); return makeInt(0); }
    int err = last_io_error;
    last_io_error = 0;
    return makeInt(err);
}

// --- VM BUILT-IN IMPLEMENTATIONS: RANDOM ---

Value vmBuiltinRandomize(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "Randomize requires 0 arguments."); return makeVoid(); }
    unsigned int base = randEntropy();
    atomic_store(&gRandBaseSeed, base);
    /* Reseed the caller right away, mixed with its own thread index, so two
     * par branches that each call Randomize() still diverge even though they
     * read the same base. */
    rand_seed = randMixSeed(base, randThreadIndex());
    rand_seed_ready = 1;
    return makeVoid();
}

/* One draw in [0, PSCAL_RAND_MAX]. This is the Park-Miller "minimal standard"
 * generator macOS's rand_r uses (FreeBSD's do_rand on a 64-bit context), so
 * a run without Randomize prints what it printed on macOS on every host.
 * libc rand_r differs (glibc's is another generator, and Windows' RAND_MAX
 * is 32767), so an unseeded program printed different numbers on Linux. */
#define PSCAL_RAND_MAX 0x7fffffff
static unsigned int pscalRandDraw(unsigned int *ctx) {
    int64_t v = (int64_t)*ctx;
    if (v == 0) {
        v = 123459876;
    }
    int64_t hi = v / 127773;
    int64_t lo = v % 127773;
    int64_t x = 16807 * lo - 2836 * hi;
    if (x < 0) {
        x += 0x7fffffff;
    }
    *ctx = (unsigned int)x;
    return (unsigned int)(x % ((int64_t)PSCAL_RAND_MAX + 1));
}

/* A uniformly distributed integer in [0, n), n >= 1. One draw covers
 * RAND_MAX + 1 values; draws from the incomplete block at its top are
 * rejected, so every result is equally likely (plain `% n` favoured the low
 * values). For the usual small n that rejection almost never happens, so a
 * run without Randomize still reproduces the numbers it always did. An n
 * beyond one draw's range takes 64 bits from several draws (Random(n) could
 * never return 2^31 or more). */
static unsigned long long randUniformBelow(unsigned long long n) {
    const unsigned long long draw_range = (unsigned long long)PSCAL_RAND_MAX + 1ULL;
    if (n <= draw_range) {
        unsigned long long limit = draw_range - (draw_range % n);
        for (;;) {
            unsigned long long r = (unsigned long long)pscalRandDraw(&rand_seed);
            if (r < limit) {
                return r % n;
            }
        }
    }
    int bits_per_draw = 0;
    for (unsigned long long m = (unsigned long long)PSCAL_RAND_MAX; m; m >>= 1) {
        bits_per_draw++;
    }
    // 2^64 mod n: draws below it belong to the incomplete top block.
    unsigned long long threshold = (0ULL - n) % n;
    for (;;) {
        unsigned long long r = 0;
        for (int bits = 0; bits < 64; bits += bits_per_draw) {
            r = (r << bits_per_draw) | (unsigned long long)pscalRandDraw(&rand_seed);
        }
        if (r >= threshold) {
            return r % n;
        }
    }
}

Value vmBuiltinRandom(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) {
        randEnsureSeeded();
        return makeReal((double)pscalRandDraw(&rand_seed) / ((double)PSCAL_RAND_MAX + 1.0));
    }
    if (arg_count == 1 && IS_INTLIKE(args[0])) {
        long long n = AS_INTEGER(args[0]);
        if (n <= 0) { runtimeError(vm, "Random argument must be > 0."); return makeInt(0); }
        randEnsureSeeded();
        return pscalIntResultNarrow((long long)randUniformBelow((unsigned long long)n));
    }
    runtimeError(vm, "Random requires 0 arguments, or 1 integer argument.");
    return makeVoid();
}

// --- VM BUILT-IN IMPLEMENTATIONS: DOS/OS ---

Value vmBuiltinDosGetenv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "dosGetenv expects 1 string argument.");
        return makeString("");
    }
    const char* val = getenv(AS_STRING(args[0]));
    if (!val) val = "";
    return makeString(val);
}

// Expose getenv without the DOS_ prefix for portability
Value vmBuiltinGetenv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "getenv expects 1 string argument.");
        return makeString("");
    }
    const char* val = getenv(AS_STRING(args[0]));
    if (!val) val = "";
    return makeString(val);
}

Value vmBuiltinGetenvint(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !isPascalStringType(VALUE_TYPE(args[0])) ||
        !IS_INTLIKE(args[1])) {
        runtimeError(vm, "getEnvInt expects (string, integer).");
        return makeInt(0);
    }
    const char* name = AS_STRING(args[0]);
    long long defVal = AS_INTEGER(args[1]);
    const char* val = getenv(name);
    if (!val || *val == '\0') return pscalIntResultLike1(args[1], defVal);
    char* endptr;
    long long parsed = strtoll(val, &endptr, 10);
    if (endptr == val || *endptr != '\0') return pscalIntResultLike1(args[1], defVal);
    return pscalIntResultLike1(args[1], parsed);
}

/* Val's scanners return Pascal's Code: 0 on success, otherwise the 1-based
 * position of the first character that cannot belong to the number (one past
 * the end when the text stops early, so "" is an error at 1 and "-" at 2).
 * Leading blanks and tabs are skipped; anything after the number, trailing
 * blanks included, is an error. The syntax is checked here rather than read
 * off strtoll/strtod's errno and endptr: those differ between libcs (glibc
 * reports "" as a successful 0 where Apple's strtoll sets EINVAL), and Val's
 * result must not depend on the platform. */
static size_t valSkipBlanks(const char* s) {
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}

static long long valScanInt(const char* s, long long* out) {
    size_t i = valSkipBlanks(s);
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') {
        negative = (s[i] == '-');
        i++;
    }
    if (!isdigit((unsigned char)s[i])) return (long long)i + 1;
    unsigned long long limit = negative ? (unsigned long long)LLONG_MAX + 1ULL
                                        : (unsigned long long)LLONG_MAX;
    unsigned long long magnitude = 0;
    for (; isdigit((unsigned char)s[i]); i++) {
        unsigned long long digit = (unsigned long long)(s[i] - '0');
        // Overflow: the digit that takes the value out of range is the bad one.
        if (magnitude > (limit - digit) / 10) return (long long)i + 1;
        magnitude = magnitude * 10 + digit;
    }
    if (s[i] != '\0') return (long long)i + 1;
    if (!negative) *out = (long long)magnitude;
    else if (magnitude == (unsigned long long)LLONG_MAX + 1ULL) *out = LLONG_MIN;
    else *out = -(long long)magnitude;
    return 0;
}

static long long valScanReal(const char* s, double* out) {
    size_t start = valSkipBlanks(s);
    size_t i = start;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') {
        negative = (s[i] == '-');
        i++;
    }
    // inf, infinity and nan, which Val has always accepted through strtod.
    static const char* const words[] = {"infinity", "inf", "nan"};
    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        size_t len = strlen(words[w]);
        if (strncasecmp(s + i, words[w], len) == 0 && s[i + len] == '\0') {
            double r = (words[w][0] == 'n' || words[w][0] == 'N') ? (double)NAN : (double)INFINITY;
            *out = negative ? -r : r;
            return 0;
        }
    }
    size_t digits = 0;
    while (isdigit((unsigned char)s[i])) { i++; digits++; }
    if (s[i] == '.') {
        i++;
        while (isdigit((unsigned char)s[i])) { i++; digits++; }
    }
    if (digits == 0) return (long long)i + 1;
    if (s[i] == 'e' || s[i] == 'E') {
        i++;
        if (s[i] == '+' || s[i] == '-') i++;
        if (!isdigit((unsigned char)s[i])) return (long long)i + 1;
        while (isdigit((unsigned char)s[i])) i++;
    }
    if (s[i] != '\0') return (long long)i + 1;
    // The text is a well-formed decimal, so strtod reads all of it. Underflow
    // (ERANGE with a tiny or zero result) is a value, not an error; a result
    // too large for a double has no bad character, so it reports one past the end.
    double r = strtod(s + start, NULL);
    if (isinf(r)) return (long long)i + 1;
    *out = r;
    return 0;
}

// Parse string to numeric value with Turbo/Free Pascal Val semantics: on
// error the destination is left unchanged and Code is the bad position.
Value vmBuiltinVal(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "Val expects 3 arguments.");
        return makeVoid();
    }

    Value src = args[0];
    Value dstPtr = args[1];
    Value codePtr = args[2];
    bool srcIsChar = VALUE_TYPE(src) == TYPE_CHAR;
    if ((!isPascalStringType(VALUE_TYPE(src)) && !srcIsChar) ||
        VALUE_TYPE(dstPtr) != TYPE_POINTER || VALUE_TYPE(codePtr) != TYPE_POINTER) {
        runtimeError(vm, "Val expects (string, var numeric, var integer).");
        return makeVoid();
    }

    Value* dst = (Value*)AS_POINTER(dstPtr);
    Value* code = (Value*)AS_POINTER(codePtr);
    // A Char is a one-character string (Pascal passes Val('-', ...) a Char
    // literal); a character outside ASCII is never part of a number.
    char charText[2] = {0, 0};
    const char* s;
    if (srcIsChar) {
        int ch = AS_CHAR(src);
        charText[0] = (ch > 0 && ch < 128) ? (char)ch : '?';
        s = charText;
    } else {
        s = AS_STRING(src) ? AS_STRING(src) : "";
    }

    long long status;
    if (isRealType(VALUE_TYPE(*dst))) {
        // Every real type, Extended included (it used to take the integer
        // branch, where SET_INT_VALUE on a real stores nothing, and still
        // report Code 0).
        double r = 0.0;
        status = valScanReal(s, &r);
        if (status == 0) SET_REAL_VALUE(dst, r);
    } else if (IS_INTLIKE(*dst)) {
        long long n = 0;
        status = valScanInt(s, &n);
        if (status == 0) {
            // The number must fit the variable: Val('99999999999', i, code)
            // with an Integer i stored 1215752191 and Code 0. Out of range
            // is reported one past the end, the variable left unchanged.
            long long lo;
            unsigned long long hi;
            builtinIntTypeRange(VALUE_TYPE(*dst), &lo, &hi);
            bool fits = (n < 0) ? (lo < 0 && n >= lo) : ((unsigned long long)n <= hi);
            if (fits) {
                SET_INT_VALUE(dst, n);
            } else {
                status = (long long)strlen(s) + 1;
            }
        }
    } else {
        runtimeError(vm, "Val expects a numeric variable, got %s.", varTypeToString(VALUE_TYPE(*dst)));
        return makeVoid();
    }
    *code = makeInt(status);
    return makeVoid();
}

Value vmBuiltinValreal(VM* vm, int arg_count, Value* args) {
    return vmBuiltinVal(vm, arg_count, args);
}

Value vmBuiltinVMVersion(VM* vm, int arg_count, Value* args) {
    (void)vm; (void)args;
    return arg_count == 0 ? makeInt(pscal_vm_version()) : makeInt(-1);
}

Value vmBuiltinBytecodeVersion(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0 || !vm || !vm->chunk) return makeInt(-1);
    return makeInt(vm->chunk->version);
}

Value vmBuiltinDosExec(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !isPascalStringType(VALUE_TYPE(args[0])) || !isPascalStringType(VALUE_TYPE(args[1]))) {
        runtimeError(vm, "dosExec expects 2 string arguments.");
        return makeInt(-1);
    }
    const char* path = AS_STRING(args[0]);
    const char* cmdline = AS_STRING(args[1]);

    int result = -1;
#ifdef PSCAL_TARGET_IOS
    runtimeError(vm, "dosExec is unavailable on iOS builds.");
#else
    size_t len = strlen(path) + strlen(cmdline) + 2;
    char* cmd = malloc(len);
    if (!cmd) {
        runtimeError(vm, "dosExec memory allocation failed.");
        return makeInt(-1);
    }
    snprintf(cmd, len, "%s %s", path, cmdline);

    wordexp_t p;
    // WRDE_NOCMD prevents command substitution $(...) which is a security risk.
    // This effectively blocks shell injection attacks while allowing argument parsing.
    int ret = wordexp(cmd, &p, WRDE_NOCMD);
    if (ret == 0) {
        if (p.we_wordc > 0) {
            pid_t pid = fork();
            if (pid == 0) {
                // Child process
                execvp(p.we_wordv[0], p.we_wordv);
                // If execvp fails, exit with error
                _exit(127);
            } else if (pid > 0) {
                // Parent process
                int status;
                if (waitpid(pid, &status, 0) != -1) {
                    if (WIFEXITED(status)) {
                        result = WEXITSTATUS(status);
                    }
                    // The child may have been an interactive, job-control
                    // shell that seized the controlling terminal for its
                    // own process group and exited without giving it back
                    // (many shells never restore it). Reclaim it here so
                    // this process's own next terminal read doesn't stop
                    // or fail; a no-op if the child never touched it.
                    pscalRuntimeReclaimForegroundTerminal(STDIN_FILENO);
                }
            }
        }
        wordfree(&p);
    } else {
        // wordexp failed, likely due to bad characters (WRDE_BADCHAR) or other issues.
        // Treat as execution failure to prevent injection.
        result = -1;
    }
    free(cmd);
#endif
    return makeInt(result);
}

static int dosMkdirParents(const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    char *tmp = strdup(path);
    if (!tmp) {
        errno = ENOMEM;
        return -1;
    }
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    char *p = tmp;
    /* Skip leading slash so we don't try to mkdir("/") */
    if (*p == '/') {
        p++;
    }
    int rc = 0;
    for (; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
#if defined(PSCAL_TARGET_IOS)
        if (pscalPathVirtualized_mkdir(tmp, 0777) != 0 && errno != EEXIST) {
#else
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
#endif
            rc = -1;
            break;
        }
        *p = '/';
    }
    if (rc == 0) {
#if defined(PSCAL_TARGET_IOS)
        if (pscalPathVirtualized_mkdir(tmp, 0777) != 0 && errno != EEXIST) {
#else
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
#endif
            rc = -1;
        }
    }
    free(tmp);
    return rc;
}

Value vmBuiltinDosMkdir(VM* vm, int arg_count, Value* args) {
    bool parents = false;
    int first_path_idx = 0;
    if (arg_count <= 0) {
        runtimeError(vm, "dosMkdir expects at least one path.");
        return makeInt(EINVAL);
    }
    /* Accept optional flags like -p, ignoring others for now. */
    if (isPascalStringType(VALUE_TYPE(args[0])) && AS_STRING(args[0])[0] == '-') {
        const char *opt = AS_STRING(args[0]);
        if (strcmp(opt, "-p") == 0) {
            parents = true;
        } else {
            runtimeError(vm, "dosMkdir: unsupported option '%s'", opt);
            return makeInt(EINVAL);
        }
        first_path_idx = 1;
    }
    int last_err = 0;
    bool any = false;
    for (int i = first_path_idx; i < arg_count; i++) {
        if (!isPascalStringType(VALUE_TYPE(args[i]))) {
            runtimeError(vm, "dosMkdir: path %d is not a string", i - first_path_idx + 1);
            return makeInt(EINVAL);
        }
        const char *path = AS_STRING(args[i]);
        any = true;
        int rc = -1;
        if (parents) {
            rc = dosMkdirParents(path);
        } else {
#if defined(PSCAL_TARGET_IOS)
            rc = pscalPathVirtualized_mkdir(path, 0777);
#else
            rc = mkdir(path, 0777);
#endif
        }
        if (rc != 0 && errno != EEXIST) {
            last_err = errno ? errno : EIO;
        }
    }
    if (!any) {
        runtimeError(vm, "dosMkdir expects at least one path.");
        return makeInt(EINVAL);
    }
    return makeInt(last_err);
}

Value vmBuiltinDosRmdir(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "dosRmdir expects 1 string argument.");
        return makeInt(errno);
    }
    int rc = -1;
#if defined(PSCAL_TARGET_IOS)
    rc = pscalPathVirtualized_rmdir(AS_STRING(args[0]));
#else
    rc = rmdir(AS_STRING(args[0]));
#endif
    return makeInt(rc == 0 ? 0 : errno);
}

Value vmBuiltinDosFindfirst(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "dosFindfirst expects 1 string argument.");
        return makeString("");
    }
    if (dos_dir) { closedir(dos_dir); dos_dir = NULL; }
    dos_dir = opendir(AS_STRING(args[0]));
    if (!dos_dir) return makeString("");
    struct dirent* ent;
    while ((ent = readdir(dos_dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            return makeString(ent->d_name);
        }
    }
    closedir(dos_dir); dos_dir = NULL;
    return makeString("");
}

Value vmBuiltinDosFindnext(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "dosFindnext expects 0 arguments.");
        return makeString("");
    }
    if (!dos_dir) return makeString("");
    struct dirent* ent;
    while ((ent = readdir(dos_dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            return makeString(ent->d_name);
        }
    }
    closedir(dos_dir); dos_dir = NULL;
    return makeString("");
}

Value vmBuiltinDosGetfattr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "dosGetfattr expects 1 string argument.");
        return makeInt(0);
    }
    struct stat st;
    if (stat(AS_STRING(args[0]), &st) != 0) {
        return makeInt(0);
    }
    int attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= 16;
    if (!(st.st_mode & S_IWUSR)) attr |= 1;
    return makeInt(attr);
}

/* Store a date/time component into a VAR argument. An integer variable keeps
 * its own type: retyping each one to Word first (SET_VALUE_TYPE) dropped an
 * Int64's heap box without freeing it and changed the variable's type. */
static void dosStoreTimeField(Value* arg, long long value) {
    Value* dst = (VALUE_TYPE(*arg) == TYPE_POINTER) ? (Value*)AS_POINTER(*arg) : NULL;
    if (!dst) {
        return;
    }
    if (!IS_INTLIKE(*dst)) {
        freeValue(dst);
        SET_VALUE_TYPE(dst, TYPE_WORD);
    }
    SET_INT_VALUE(dst, value);
}

Value vmBuiltinDosGetdate(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "dosGetdate expects 4 var arguments.");
        return makeVoid();
    }
    time_t t = time(NULL);
    struct tm tm_info;
#if defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_info, &t);
#else
    if (!localtime_r(&t, &tm_info)) {
        struct tm *tmp = localtime(&t);
        if (tmp) tm_info = *tmp; else memset(&tm_info, 0, sizeof(tm_info));
    }
#endif
    dosStoreTimeField(&args[0], tm_info.tm_year + 1900);
    dosStoreTimeField(&args[1], tm_info.tm_mon + 1);
    dosStoreTimeField(&args[2], tm_info.tm_mday);
    dosStoreTimeField(&args[3], tm_info.tm_wday);
    return makeVoid();
}

Value vmBuiltinDosGettime(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "dosGettime expects 4 var arguments.");
        return makeVoid();
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
#if defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_info, &tv.tv_sec);
#else
    if (!localtime_r(&tv.tv_sec, &tm_info)) {
        struct tm *tmp = localtime(&tv.tv_sec);
        if (tmp) tm_info = *tmp; else memset(&tm_info, 0, sizeof(tm_info));
    }
#endif
    dosStoreTimeField(&args[0], tm_info.tm_hour);
    dosStoreTimeField(&args[1], tm_info.tm_min);
    dosStoreTimeField(&args[2], tm_info.tm_sec);
    dosStoreTimeField(&args[3], (long long)(tv.tv_usec / 10000));
    return makeVoid();
}

Value vmBuiltinScreencols(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ScreenCols expects 0 arguments.");
        return makeInt(80);
    }
    int rows, cols;
    if (getTerminalSize(&rows, &cols) == 0) {
        return makeInt(cols);
    }
    return makeInt(80); // Default on error
}

Value vmBuiltinScreenrows(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ScreenRows expects 0 arguments.");
        return makeInt(24);
    }
    int rows, cols;
    if (getTerminalSize(&rows, &cols) == 0) {
        return makeInt(rows);
    }
    return makeInt(24); // Default on error
}

// --- VM-NATIVE MEMORY STREAM FUNCTIONS ---
Value vmBuiltinMstreamcreate(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "MStreamCreate expects no arguments.");
        return makeVoid();
    }
    MStream *ms = createMStream();
    if (!ms) {
        runtimeError(vm, "Memory allocation error for MStream structure in MStreamCreate.");
        return makeVoid();
    }
    return makeMStream(ms);
}

Value vmBuiltinMstreamloadfromfile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "MStreamLoadFromFile expects 2 arguments (MStreamVar, Filename).");
        return makeBoolean(0);
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (VALUE_TYPE(args[0]) != TYPE_POINTER) {
        runtimeError(vm, "MStreamLoadFromFile: First argument must be a VAR MStream.");
        return makeBoolean(0);
    }
    Value* ms_value_ptr = (Value*)AS_POINTER(args[0]);
    if (!ms_value_ptr || VALUE_TYPE(*ms_value_ptr) != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamLoadFromFile: First argument is not a valid MStream variable.");
        return makeBoolean(0);
    }
    MStream* ms = AS_MSTREAM(*ms_value_ptr);
    if (!ms) {
        runtimeError(vm, "MStreamLoadFromFile: MStream variable not initialized.");
        return makeBoolean(0);
    }

    // Argument 1 is the filename string
    if (!isPascalStringType(VALUE_TYPE(args[1])) || AS_STRING(args[1]) == NULL) {
        runtimeError(vm, "MStreamLoadFromFile: Second argument must be a string filename.");
        return makeBoolean(0); // No need to free args[1] here, vm stack manages
    }
    const char* filename = AS_STRING(args[1]);

    FILE* f = fopen(filename, "rb");
    if (!f) {
        runtimeError(vm, "MStreamLoadFromFile: Cannot open file '%s' for reading.", filename);
        return makeBoolean(0);
    }

    // Read to end of file rather than trusting ftell for the size: on a pipe
    // or FIFO (MStreamLoadFromFile(ms, '/dev/stdin')) ftell is -1, which made
    // this malloc(0) and then fread SIZE_MAX bytes into it. The file size,
    // when there is one, only sizes the first allocation. MStream sizes are
    // ints, so a file of 2 GiB or more is refused instead of truncated.
    size_t capacity = 64 * 1024;
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t end = ftello(f);
        if (end > 0 && end < (off_t)INT_MAX) {
            capacity = (size_t)end + 1;
        }
        if (fseeko(f, 0, SEEK_SET) != 0) {
            clearerr(f);
        }
    } else {
        clearerr(f);
    }
    unsigned char* buffer = malloc(capacity);
    size_t size = 0;
    bool too_large = false;
    while (buffer) {
        if (size + 1 >= capacity) {
            if (capacity >= (size_t)INT_MAX) {
                too_large = true;
                break;
            }
            size_t new_capacity = capacity * 2;
            if (new_capacity > (size_t)INT_MAX) new_capacity = (size_t)INT_MAX;
            unsigned char* grown = realloc(buffer, new_capacity);
            if (!grown) {
                free(buffer);
                buffer = NULL;
                break;
            }
            buffer = grown;
            capacity = new_capacity;
        }
        size_t n = fread(buffer + size, 1, capacity - size - 1, f);
        size += n;
        if (n == 0 || feof(f)) {
            break;
        }
        if (size + 1 >= capacity) {
            // Filled exactly: check for end of file before growing.
            int c = fgetc(f);
            if (c == EOF) {
                break;
            }
            ungetc(c, f);
        }
    }
    if (!buffer) {
        fclose(f);
        runtimeError(vm, "MStreamLoadFromFile: Memory allocation error for file buffer.");
        return makeBoolean(0);
    }
    if (too_large || ferror(f)) {
        fprintf(stderr, too_large ? "MStreamLoadFromFile: file is too large (2 GiB or more).\n"
                                  : "MStreamLoadFromFile: short read or read error.\n");
        free(buffer);
        fclose(f);
        return makeBoolean(0);
    }
    buffer[size] = '\0'; // Null-terminate the buffer
    fclose(f);

    // Free existing buffer in MStream if any
    if (ms->buffer) free(ms->buffer);

    ms->buffer = buffer;
    ms->size = (int)size;
    ms->capacity = (int)capacity;

    return makeBoolean(1);
}

Value vmBuiltinMstreamsavetofile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "MStreamSaveToFile expects 2 arguments (MStreamVar, Filename).");
        return makeVoid();
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (VALUE_TYPE(args[0]) != TYPE_POINTER) {
        runtimeError(vm, "MStreamSaveToFile: First argument must be a VAR MStream.");
        return makeVoid();
    }
    Value* ms_value_ptr = (Value*)AS_POINTER(args[0]);
    if (!ms_value_ptr || VALUE_TYPE(*ms_value_ptr) != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamSaveToFile: First argument is not a valid MStream variable.");
        return makeVoid();
    }
    MStream* ms = AS_MSTREAM(*ms_value_ptr);
    if (!ms) {
        runtimeError(vm, "MStreamSaveToFile: MStream variable not initialized.");
        return makeVoid();
    }

    // Argument 1 is the filename string
    if (!isPascalStringType(VALUE_TYPE(args[1])) || AS_STRING(args[1]) == NULL) {
        runtimeError(vm, "MStreamSaveToFile: Second argument must be a string filename.");
        return makeVoid();
    }
    const char* filename = AS_STRING(args[1]);

    FILE* f = fopen(filename, "wb");
    if (!f) {
        runtimeError(vm, "MStreamSaveToFile: Cannot open file '%s' for writing.", filename);
        return makeVoid();
    }

    if (ms->buffer && ms->size > 0) {
        fwrite(ms->buffer, 1, ms->size, f);
    }
    fclose(f);

    return makeVoid();
}

Value vmBuiltinMstreamfree(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "MStreamFree expects 1 argument (MStreamVar).");
        return makeVoid();
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (VALUE_TYPE(args[0]) != TYPE_POINTER) {
        runtimeError(vm, "MStreamFree: First argument must be a VAR MStream.");
        return makeVoid();
    }
    Value* ms_value_ptr = (Value*)AS_POINTER(args[0]);
    if (!ms_value_ptr || VALUE_TYPE(*ms_value_ptr) != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamFree: First argument is not a valid MStream variable.");
        return makeVoid();
    }
    MStream* ms = AS_MSTREAM(*ms_value_ptr);

    if (ms) { // Only free if MStream struct itself exists
        releaseMStream(ms);
        // VM 2.0 Phase 4i checkpoint 3d: mstream IS bits now -- this call
        // both clears the pointer and updates the tag, replacing the old
        // "AS_MSTREAM(*ms_value_ptr) = NULL;" raw union write.
        pscalValueSetHeapPtrBits(ms_value_ptr, NULL);
    }
    // If ms was NULL, it's a no-op, which is fine.

    return makeVoid();
}

Value vmBuiltinMstreambuffer(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "MStreamBuffer expects 1 argument (MStream).");
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_MEMORYSTREAM || AS_MSTREAM(args[0]) == NULL) {
        runtimeError(vm, "MStreamBuffer: Argument is not a valid MStream.");
        return makeVoid();
    }
    MStream* mstream = AS_MSTREAM(args[0]);
    const char* buffer_content = mstream->buffer ? (char*)mstream->buffer : "";
    return makeString(buffer_content);
}

Value vmBuiltinMstreamFromString(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "MStreamFromString expects 1 argument (string).");
        return makeMStream(NULL);
    }

    if (!builtinValueIsStringLike(&args[0])) {
        runtimeError(vm, "MStreamFromString requires a string argument.");
        return makeMStream(NULL);
    }

    const char* payload = builtinValueToCString(&args[0]);
    if (!payload) {
        payload = "";
    }

    size_t len = strlen(payload);
    size_t capacity = len + 1;

    MStream* ms = createMStream();
    if (!ms) {
        runtimeError(vm, "MStreamFromString failed to allocate stream.");
        return makeMStream(NULL);
    }

    if (capacity > 0) {
        unsigned char* buffer = (unsigned char*)malloc(capacity);
        if (!buffer) {
            releaseMStream(ms);
            runtimeError(vm, "MStreamFromString failed to allocate buffer.");
            return makeMStream(NULL);
        }
        if (len > 0) {
            memcpy(buffer, payload, len);
        }
        buffer[len] = '\0';
        ms->buffer = buffer;
        ms->capacity = (int)capacity;
        ms->size = (int)len;
    }

    return makeMStream(ms);
}

Value vmBuiltinMstreamAppendByte(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "MStreamAppendByte expects (mstream, byte).");
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_MEMORYSTREAM || AS_MSTREAM(args[0]) == NULL) {
        runtimeError(vm, "MStreamAppendByte: first argument must be a valid mstream.");
        return makeVoid();
    }
    int byte = 0;
    if (!IS_INTLIKE(args[1])) {
        runtimeError(vm, "MStreamAppendByte: second argument must be an integer.");
        return makeVoid();
    }
    byte = AS_INTEGER(args[1]) & 0xFF;

    MStream* ms = AS_MSTREAM(args[0]);
    /* Ensure capacity */
    if (ms->capacity < ms->size + 2) { /* +1 for new byte, +1 for terminator */
        int newcap = (ms->capacity > 0) ? ms->capacity * 2 : 64;
        if (newcap < ms->size + 2) newcap = ms->size + 2;
        unsigned char* newbuf = realloc(ms->buffer, (size_t)newcap);
        if (!newbuf) {
            runtimeError(vm, "MStreamAppendByte: out of memory.");
            return makeVoid();
        }
        ms->buffer = newbuf;
        ms->capacity = newcap;
    }
    ms->buffer[ms->size] = (unsigned char)byte;
    ms->size += 1;
    ms->buffer[ms->size] = '\0'; /* maintain convenience NUL */
    return makeVoid();
}

Value vmBuiltinReal(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Real() expects 1 argument.");
        return makeReal(0.0);
    }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) {
        return makeReal((double)AS_INTEGER(arg));
    }
    if (VALUE_TYPE(arg) == TYPE_CHAR) {
        return makeReal((double)AS_CHAR(arg));
    }
    if (isRealType(VALUE_TYPE(arg))) {
        return makeReal(AS_REAL(arg));
    }
    runtimeError(vm, "Real() argument must be an Integer, Ordinal, or Real type. Got %s.", varTypeToString(VALUE_TYPE(arg)));
    return makeReal(0.0);
}


Value vmBuiltinInttostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "IntToStr requires 1 argument."); return makeString(""); }
    Value arg = args[0];
    long long value_to_convert = 0;
    if (IS_INTLIKE(arg)) {
        value_to_convert = AS_INTEGER(arg);
    } else if (VALUE_TYPE(arg) == TYPE_CHAR) {
        value_to_convert = (long long)AS_CHAR(arg);
    } else {
        runtimeError(vm, "IntToStr requires an integer-compatible argument."); return makeString("");
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%lld", value_to_convert);
    return makeString(buffer);
}

Value vmBuiltinStr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || VALUE_TYPE(args[1]) != TYPE_POINTER) {
        runtimeError(vm, "Str expects (value, var string).");
        return makeVoid();
    }
    Value val = args[0];
    Value* dest = (Value*)AS_POINTER(args[1]);
    if (!dest) {
        runtimeError(vm, "Str received a nil pointer.");
        return makeVoid();
    }

    char* new_buf = NULL;
    if (isPascalStringType(VALUE_TYPE(val))) {
        const char* src = AS_STRING(val) ? AS_STRING(val) : "";
        new_buf = strdup(src);
    } else if (isRealType(VALUE_TYPE(val))) {
        // The text Write gives the same real with no width (printValueToStream),
        // sized to fit: a 64-byte buffer cut Str(1e300) to its first 63 digits,
        // a number a factor 10^246 too small.
        if (frontendIsAether()) {
            char repr[64];
            pscalFormatRealRepr((double)AS_REAL(val), repr, sizeof(repr)); /* D74 */
            new_buf = strdup(repr);
        } else if (VALUE_TYPE(val) == TYPE_LONG_DOUBLE) {
            new_buf = builtinAsprintf("%Lf", AS_REAL(val));
        } else {
            new_buf = builtinAsprintf("%f", (double)AS_REAL(val));
        }
    } else {
        char buffer[64];
        switch (VALUE_TYPE(val)) {
            case TYPE_CHAR:
                snprintf(buffer, sizeof(buffer), "%c", AS_CHAR(val));
                break;
            case TYPE_WIDECHAR: {
                char utf8[5];
                encodeUtf8Codepoint((uint32_t)AS_CHAR(val), utf8);
                snprintf(buffer, sizeof(buffer), "%s", utf8);
                break;
            }
            case TYPE_BOOLEAN:
                snprintf(buffer, sizeof(buffer), "%s", VAL_INT(val) ? "TRUE" : "FALSE");
                break;
            default:
                if (IS_INTLIKE(val)) {
                    snprintf(buffer, sizeof(buffer), "%lld", AS_INTEGER(val));
                } else {
                    runtimeError(vm, "Str expects a numeric, char, or formatted string argument.");
                    return makeVoid();
                }
                break;
        }
        new_buf = strdup(buffer);
    }
    if (!new_buf) {
        runtimeError(vm, "Str: memory allocation failed.");
        return makeVoid();
    }
    VarType dest_type = isPascalStringType(VALUE_TYPE(*dest)) ? VALUE_TYPE(*dest) : TYPE_STRING;
    freeValue(dest);
    SET_VALUE_TYPE(dest, dest_type);
    pscalStringEnsureObj(dest); // freeValue left dest->s_val NULL
    AS_STRING(*dest) = new_buf;
    STRING_MAX_LENGTH(*dest) = -1;
    return makeVoid();
}

Value vmBuiltinLength(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Length expects 1 argument.");
        return makeInt(0);
    }

    Value arg = args[0];

    if (VALUE_TYPE(arg) == TYPE_POINTER) {
        if (!AS_POINTER(arg)) {
            runtimeError(vm, "Length() cannot dereference a nil pointer argument.");
            return makeInt(0);
        }
        Value* pointed = (Value*)AS_POINTER(arg);
        if (isPascalStringType(VALUE_TYPE(*pointed))) {
            if (VALUE_TYPE(*pointed) == TYPE_UNICODE_STRING) {
                size_t len = AS_STRING(*pointed) ? strlen(AS_STRING(*pointed)) : 0;
                return makeInt((long long)utf8CodepointCount(AS_STRING(*pointed), len));
            }
            return makeInt(AS_STRING(*pointed) ? (long long)strlen(AS_STRING(*pointed)) : 0);
        }
    }

    if (VALUE_TYPE(arg) == TYPE_UNICODE_STRING) {
        size_t len = AS_STRING(arg) ? strlen(AS_STRING(arg)) : 0;
        return makeInt((long long)utf8CodepointCount(AS_STRING(arg), len));
    }

    if (VALUE_TYPE(arg) == TYPE_STRING) {
        return makeInt(AS_STRING(arg) ? (long long)strlen(AS_STRING(arg)) : 0);
    }

    if (VALUE_TYPE(arg) == TYPE_CHAR || VALUE_TYPE(arg) == TYPE_WIDECHAR) {
        return makeInt(1);
    }

    ArrayBoundsResult bounds = resolveFirstDimBounds(&arg);
    if (bounds.hasBounds) {
        long long len = (long long)bounds.upper - (long long)bounds.lower + 1;
        if (len < 0) {
            len = 0;
        }
        return makeInt(len);
    }
    if (bounds.hitNilPointer) {
        runtimeError(vm, "Length() cannot dereference a nil array reference.");
        return makeInt(0);
    }

    runtimeError(vm, "Length expects a string or array argument.");
    return makeInt(0);
}

Value vmBuiltinSizeof(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "SizeOf expects 1 argument.");
        return makeInt64(0);
    }

    Value arg = args[0];

    const char *type_name = NULL;
    if (builtinValueIsStringLike(&arg)) {
        type_name = builtinValueToCString(&arg);
    }

    if (type_name && *type_name) {
        long long bytes = 0;
        if (computeSizeFromTypeName(type_name, &bytes)) {
            return makeInt64(bytes);
        }
        runtimeError(vm, "SizeOf: unknown type '%s'.", type_name);
        return makeInt64(0);
    }

    long long bytes = 0;
    if (computeValueSizeBytes(&arg, &bytes)) {
        return makeInt64(bytes);
    }

    runtimeError(vm, "SizeOf unsupported for type '%s'.", varTypeToString(VALUE_TYPE(arg)));
    return makeInt64(0);
}


Value vmBuiltinAbs(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "abs expects 1 argument."); return makeInt(0); }
    if (IS_INTLIKE(args[0])) {
        long long v = AS_INTEGER(args[0]);
        if (v >= 0 || VALUE_TYPE(args[0]) == TYPE_UINT64) return pscalIntResultLike1(args[0], v);
        if (v == LLONG_MIN) { // -v does not fit; llabs() is undefined here and wrapped
            runtimeError(vm, "Runtime Error: Integer overflow.");
            return makeInt(0);
        }
        return pscalIntResultLike1(args[0], -v);
    }
    if (isRealType(VALUE_TYPE(args[0]))) return makeReal(fabsl(AS_REAL(args[0])));
    runtimeError(vm, "abs expects a numeric argument.");
    return makeInt(0);
}

Value vmBuiltinRound(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Round expects 1 argument."); return makeInt(0); }
    if (isRealType(VALUE_TYPE(args[0]))) return vmRealToIntResult(vm, "round", roundl(AS_REAL(args[0])));
    if (IS_INTLIKE(args[0])) return pscalIntResultLike1(args[0], AS_INTEGER(args[0]));
    runtimeError(vm, "Round expects a numeric argument.");
    return makeInt(0);
}

Value vmBuiltinOdd(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Odd expects 1 integer-compatible argument.");
        return makeBoolean(false);
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "Odd expects an integer-compatible argument.");
        return makeBoolean(false);
    }
    return makeBoolean((AS_INTEGER(args[0]) & 1LL) != 0);
}

Value vmBuiltinHalt(VM* vm, int arg_count, Value* args) {
    long long code = 0;
    if (arg_count == 0) {
        // No exit code supplied, default to 0.
    } else if (arg_count == 1 && IS_INTLIKE(args[0])) {
        code = AS_INTEGER(args[0]);
    } else {
        runtimeError(vm, "Halt expects 0 or 1 integer argument.");
    }
    exit(vmExitWithCleanup((int)code));
    return makeVoid(); // Unreachable
}

Value vmBuiltinDelay(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "Delay requires an integer argument.");
        return makeVoid();
    }
    long long ms = AS_INTEGER(args[0]);
    if (ms > 0) {
        const long long slice_ms = 200;
        long long remaining = ms;
        while (remaining > 0) {
            if (pscalRuntimeConsumeSigint()) {
                if (vm) {
                    vm->abort_requested = true;
                    vm->exit_requested = true;
                }
                break;
            }
            if (pscalRuntimeConsumeSigtstp()) {
#if defined(PSCAL_TARGET_IOS)
                if (!vmShouldUseCooperativeSuspend() && vmRequestHardSuspendCurrentVproc()) {
                    vmWaitIfStoppedWithCheckpoint();
                    continue;
                }
#endif
                if (vm) {
                    vm->abort_requested = false;
                    vm->exit_requested = true;
                    vm->suspend_unwind_requested = true;
                }
                shellRuntimeSetLastStatus(128 + SIGTSTP);
                break;
            }
            if (vm && (vm->abort_requested || vm->exit_requested)) {
                break;
            }
            long long step = remaining > slice_ms ? slice_ms : remaining;
            usleep((useconds_t)step * 1000);
            remaining -= step;
        }
    }
    return makeVoid();
}

static bool parseThreadIdValue(const Value* value, int* outId) {
    if (!value || !outId) {
        return false;
    }
    if (VALUE_TYPE(*value) == TYPE_THREAD || IS_INTLIKE(*value)) {
        long long raw = asI64(*value);
        // No VM in scope here to check a real committed count against --
        // this is just a coarse plausibility filter (reject negative/absurd
        // values); every actual array-indexing use site downstream checks
        // against that specific VM's threadsCommittedCount for real memory
        // safety (VM 2.0 Phase 5a checkpoint 5a-ii).
        if (raw <= 0 || (size_t)raw >= pscalVmThreadsCeiling()) {
            return false;
        }
        *outId = (int)raw;
        return true;
    }
    return false;
}

static bool parseBooleanValue(const Value* value, bool* outBool) {
    if (!value || !outBool) {
        return false;
    }
    if (VALUE_TYPE(*value) == TYPE_BOOLEAN) {
        *outBool = VAL_INT(*value) != 0;
        return true;
    }
    if (IS_INTLIKE(*value)) {
        *outBool = asI64(*value) != 0;
        return true;
    }
    return false;
}

typedef struct {
    char name[THREAD_NAME_MAX];
    bool submitOnly;
} ThreadRequestOptions;

static void initThreadRequestOptions(ThreadRequestOptions* options) {
    if (!options) {
        return;
    }
    options->name[0] = '\0';
    options->submitOnly = false;
}

static bool parseThreadRequestOptionsValue(const Value* value, ThreadRequestOptions* options) {
    if (!value || !options || VALUE_TYPE(*value) != TYPE_RECORD) {
        return false;
    }
    bool recognized = false;
    for (FieldValue* field = AS_RECORD(*value); field; field = field->next) {
        if (!field->name) {
            continue;
        }
        const Value *fieldValue = fieldValueStorageConst(field);
        if (strcasecmp(field->name, "name") == 0) {
            recognized = true;
            const char* requested = builtinValueToCString(fieldValue);
            if (requested) {
                strncpy(options->name, requested, sizeof(options->name) - 1);
                options->name[sizeof(options->name) - 1] = '\0';
            }
        } else if (strcasecmp(field->name, "submitonly") == 0 ||
                   strcasecmp(field->name, "submit_only") == 0 ||
                   strcasecmp(field->name, "queueonly") == 0 ||
                   strcasecmp(field->name, "queue_only") == 0 ||
                   strcasecmp(field->name, "queue") == 0) {
            recognized = true;
            bool flag = false;
            if (parseBooleanValue(fieldValue, &flag)) {
                options->submitOnly = flag;
            }
        }
    }
    return recognized;
}

static bool appendThreadField(FieldValue** head, FieldValue** tail, const char* name, Value value) {
    if (!head || !tail || !name) {
        freeValue(&value);
        return false;
    }
    FieldValue* field = (FieldValue*)calloc(1, sizeof(FieldValue));
    if (!field) {
        freeValue(&value);
        return false;
    }
    field->name = pscalInternName(name);
    field->value = value;
    field->storage = &field->value;
    field->type_def = NULL;
    field->declared_type = VALUE_TYPE(value);
    field->slot_index = (*tail) ? ((*tail)->slot_index + 1) : 0;
    field->owns_storage = true;
    field->next = NULL;
    if (!*head) {
        *head = field;
    } else {
        (*tail)->next = field;
    }
    *tail = field;
    return true;
}

static long long timevalToMicros(const struct timeval* tv) {
    if (!tv) {
        return 0;
    }
    return (long long)tv->tv_sec * 1000000LL + (long long)tv->tv_usec;
}

static Value makeTimespecRecord(const struct timespec* ts) {
    FieldValue* head = NULL;
    FieldValue* tail = NULL;
    const bool has_value = ts != NULL;
    if (!appendThreadField(&head, &tail, "valid", makeBoolean(has_value ? 1 : 0))) {
        freeFieldValue(head);
        return makeRecord(NULL);
    }
    if (has_value) {
        if (!appendThreadField(&head, &tail, "seconds", makeInt64((long long)ts->tv_sec)) ||
            !appendThreadField(&head, &tail, "nanoseconds", makeInt64((long long)ts->tv_nsec))) {
            freeFieldValue(head);
            return makeRecord(NULL);
        }
    }
    return makeRecord(head);
}

static Value makeMetricsSampleRecord(const ThreadMetricsSample* sample) {
    FieldValue* head = NULL;
    FieldValue* tail = NULL;
    const bool valid = sample && sample->valid;
    if (!appendThreadField(&head, &tail, "valid", makeBoolean(valid ? 1 : 0))) {
        freeFieldValue(head);
        return makeRecord(NULL);
    }
    if (valid) {
        if (!appendThreadField(&head, &tail, "cpu_seconds", makeInt64((long long)sample->cpuTime.tv_sec)) ||
            !appendThreadField(&head, &tail, "cpu_nanoseconds", makeInt64((long long)sample->cpuTime.tv_nsec)) ||
            !appendThreadField(&head, &tail, "rss_bytes", makeInt64((long long)sample->rssBytes)) ||
            !appendThreadField(&head, &tail, "user_micros", makeInt64(timevalToMicros(&sample->usage.ru_utime))) ||
            !appendThreadField(&head, &tail, "system_micros", makeInt64(timevalToMicros(&sample->usage.ru_stime)))) {
            freeFieldValue(head);
            return makeRecord(NULL);
        }
    }
    return makeRecord(head);
}

static Value makeMetricsRecord(const ThreadMetrics* metrics) {
    FieldValue* head = NULL;
    FieldValue* tail = NULL;
    if (!appendThreadField(&head, &tail, "start", makeMetricsSampleRecord(metrics ? &metrics->start : NULL)) ||
        !appendThreadField(&head, &tail, "end", makeMetricsSampleRecord(metrics ? &metrics->end : NULL))) {
        freeFieldValue(head);
        return makeRecord(NULL);
    }
    return makeRecord(head);
}

static Value makeThreadStateRecord(int threadId, const Thread* thread) {
    FieldValue* head = NULL;
    FieldValue* tail = NULL;
    const char* thread_name = (thread && thread->name[0] != '\0') ? thread->name : "";
    bool include_pool = thread &&
                        (thread->poolWorker ||
                         (thread_name && *thread_name && strstr(thread_name, "pool") != NULL));
    if (!appendThreadField(&head, &tail, "id", makeInt(threadId)) ||
        !appendThreadField(&head, &tail, "name", makeString(thread_name))) {
        freeFieldValue(head);
        return makeRecord(NULL);
    }

    bool active = thread && atomic_load(&thread->active);
    const bool in_pool = thread && atomic_load(&thread->inPool);
    bool reported_idle = thread &&
        (thread->idle ||
         thread->readyForReuse ||
         (!atomic_load(&thread->active) && !thread->awaitingReuse && thread->currentJob == NULL));
    const bool should_exit = thread && thread->shouldExit;
    const bool awaiting_reuse = thread && thread->awaitingReuse;
    const bool ready_for_reuse = thread && thread->readyForReuse;
    const bool status_ready = thread && thread->statusReady;
    bool status_flag = thread && thread->statusFlag;
    const bool status_consumed = thread && thread->statusConsumed;
    const bool result_ready = thread && thread->resultReady;
    const bool result_consumed = thread && thread->resultConsumed;
    const bool paused = thread ? atomic_load(&thread->paused) : false;
    const bool cancel_requested = thread ? atomic_load(&thread->cancelRequested) : false;
    const bool kill_requested = thread ? atomic_load(&thread->killRequested) : false;

    if ((frontendIsPascal() || frontendIsRea()) && include_pool) {
        active = false;
        reported_idle = true;
        status_flag = false;
    }

    if (!appendThreadField(&head, &tail, "active", makeBoolean(active ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "in_pool", makeBoolean(in_pool ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "idle", makeBoolean(reported_idle ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "should_exit", makeBoolean(should_exit ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "awaiting_reuse", makeBoolean(awaiting_reuse ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "ready_for_reuse", makeBoolean(ready_for_reuse ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "status_ready", makeBoolean(status_ready ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "status_success", makeBoolean(status_flag ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "status_consumed", makeBoolean(status_consumed ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "result_ready", makeBoolean(result_ready ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "result_consumed", makeBoolean(result_consumed ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "paused", makeBoolean(paused ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "cancel_requested", makeBoolean(cancel_requested ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "kill_requested", makeBoolean(kill_requested ? 1 : 0)) ||
        !appendThreadField(&head, &tail, "pool_generation", makeInt(thread ? thread->poolGeneration : 0))) {
        freeFieldValue(head);
        return makeRecord(NULL);
    }

    if (!appendThreadField(&head, &tail, "queued_at", makeTimespecRecord(thread ? &thread->queuedAt : NULL)) ||
        !appendThreadField(&head, &tail, "started_at", makeTimespecRecord(thread ? &thread->startedAt : NULL)) ||
        !appendThreadField(&head, &tail, "finished_at", makeTimespecRecord(thread ? &thread->finishedAt : NULL)) ||
        !appendThreadField(&head, &tail, "metrics", makeMetricsRecord(thread ? &thread->metrics : NULL))) {
        freeFieldValue(head);
        return makeRecord(NULL);
    }

    return makeRecord(head);
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} JsonBuffer;

static bool jsonBufferReserve(JsonBuffer *buffer, size_t additional) {
    if (!buffer) {
        return false;
    }
    size_t required = buffer->length + additional + 1;
    if (required <= buffer->capacity) {
        return true;
    }
    size_t new_capacity = buffer->capacity ? buffer->capacity : 256;
    while (new_capacity < required) {
        new_capacity *= 2;
    }
    char *new_data = (char *)realloc(buffer->data, new_capacity);
    if (!new_data) {
        return false;
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return true;
}

static bool jsonBufferAppendFormat(JsonBuffer *buffer, const char *fmt, ...) {
    if (!buffer || !fmt) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    if (!jsonBufferReserve(buffer, (size_t)needed)) {
        va_end(args);
        return false;
    }
    vsnprintf(buffer->data + buffer->length,
              buffer->capacity - buffer->length,
              fmt, args);
    buffer->length += (size_t)needed;
    va_end(args);
    return true;
}

static bool jsonAppendEscapedString(JsonBuffer *buffer, const char *text) {
    if (!jsonBufferAppendFormat(buffer, "\"")) {
        return false;
    }
    const unsigned char *ptr = (const unsigned char *)(text ? text : "");
    while (*ptr) {
        unsigned char ch = *ptr++;
        switch (ch) {
            case '\\':
                if (!jsonBufferAppendFormat(buffer, "\\\\")) {
                    return false;
                }
                break;
            case '"':
                if (!jsonBufferAppendFormat(buffer, "\\\"")) {
                    return false;
                }
                break;
            case '\n':
                if (!jsonBufferAppendFormat(buffer, "\\n")) {
                    return false;
                }
                break;
            case '\r':
                if (!jsonBufferAppendFormat(buffer, "\\r")) {
                    return false;
                }
                break;
            case '\t':
                if (!jsonBufferAppendFormat(buffer, "\\t")) {
                    return false;
                }
                break;
            default:
                if (ch < 0x20) {
                    if (!jsonBufferAppendFormat(buffer, "\\u%04x", ch)) {
                        return false;
                    }
                } else {
                    if (!jsonBufferReserve(buffer, 1)) {
                        return false;
                    }
                    buffer->data[buffer->length++] = (char)ch;
                    buffer->data[buffer->length] = '\0';
                }
                break;
        }
    }
    return jsonBufferAppendFormat(buffer, "\"");
}

static bool jsonAppendValue(JsonBuffer *buffer, const Value *value);

static bool jsonAppendArrayRecursive(JsonBuffer *buffer, const Value *array,
                                     int dimension, int *indices) {
    if (!jsonBufferAppendFormat(buffer, "[")) {
        return false;
    }
    int lower = ARRAY_LOWER_BOUNDS(*array)[dimension];
    int upper = ARRAY_UPPER_BOUNDS(*array)[dimension];
    for (int idx = lower; idx <= upper; ++idx) {
        if (idx > lower && !jsonBufferAppendFormat(buffer, ", ")) {
            return false;
        }
        indices[dimension] = idx;
        if (dimension + 1 >= ARRAY_DIMENSIONS(*array)) {
            int offset = computeFlatOffset((Value *)array, indices);
            if (arrayUsesPackedBytes(array)) {
                if (!AS_ARRAY_RAW(*array)) {
                    return false;
                }
                Value temp = makeByte(AS_ARRAY_RAW(*array)[offset]);
                if (!jsonAppendValue(buffer, &temp)) {
                    return false;
                }
            } else {
                if (!jsonAppendValue(buffer, &AS_ARRAY(*array)[offset])) {
                    return false;
                }
            }
        } else {
            if (!jsonAppendArrayRecursive(buffer, array, dimension + 1, indices)) {
                return false;
            }
        }
    }
    return jsonBufferAppendFormat(buffer, "]");
}

static bool jsonAppendArray(JsonBuffer *buffer, const Value *array) {
    if (!array || ARRAY_DIMENSIONS(*array) <= 0 ||
        (!AS_ARRAY(*array) && !arrayUsesPackedBytes(array)) ||
        !ARRAY_LOWER_BOUNDS(*array) || !ARRAY_UPPER_BOUNDS(*array)) {
        return jsonBufferAppendFormat(buffer, "[]");
    }
    int *indices = (int *)malloc(sizeof(int) * ARRAY_DIMENSIONS(*array));
    if (!indices) {
        return false;
    }
    bool ok = jsonAppendArrayRecursive(buffer, array, 0, indices);
    free(indices);
    return ok;
}

static bool jsonAppendRecord(JsonBuffer *buffer, const FieldValue *field) {
    if (!jsonBufferAppendFormat(buffer, "{")) {
        return false;
    }
    bool first = true;
    while (field) {
        if (!first && !jsonBufferAppendFormat(buffer, ", ")) {
            return false;
        }
        if (!jsonAppendEscapedString(buffer, field->name ? field->name : "?")) {
            return false;
        }
        if (!jsonBufferAppendFormat(buffer, ": ")) {
            return false;
        }
        if (!jsonAppendValue(buffer, fieldValueStorageConst(field))) {
            return false;
        }
        first = false;
        field = field->next;
    }
    return jsonBufferAppendFormat(buffer, "}");
}

static bool jsonAppendValue(JsonBuffer *buffer, const Value *value) {
    if (!value) {
        return jsonBufferAppendFormat(buffer, "null");
    }
    switch (VALUE_TYPE(*value)) {
        case TYPE_BOOLEAN:
            return jsonBufferAppendFormat(buffer, "%s",
                                         VAL_INT(*value) ? "true" : "false");
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
            return jsonBufferAppendFormat(buffer, "%lld", VAL_INT(*value));
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
            return jsonBufferAppendFormat(buffer, "%llu", VAL_UINT(*value));
        case TYPE_FLOAT:
            return jsonBufferAppendFormat(buffer, "%g", VAL_REAL32(*value));
        case TYPE_DOUBLE:
            return jsonBufferAppendFormat(buffer, "%g", VAL_REAL64(*value));
        case TYPE_LONG_DOUBLE:
            return jsonBufferAppendFormat(buffer, "%Lg", VAL_REAL_LD(*value));
        case TYPE_STRING:
            return jsonAppendEscapedString(buffer, AS_STRING(*value) ? AS_STRING(*value) : "");
        case TYPE_RECORD:
            return jsonAppendRecord(buffer, AS_RECORD(*value));
        case TYPE_ARRAY:
            return jsonAppendArray(buffer, value);
        case TYPE_NIL:
        case TYPE_VOID:
            return jsonBufferAppendFormat(buffer, "null");
        default:
            return jsonAppendEscapedString(buffer, varTypeToString(VALUE_TYPE(*value)));
    }
}

static void jsonBufferFree(JsonBuffer *buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static Value threadSpawnOrSubmitCommon(VM* vm, int arg_count, Value* args, bool poolSubmit, const char* opName) {
    if (!vm) {
        return makeInt(-1);
    }
    if (arg_count < 1) {
        runtimeError(vm, "%s expects a builtin identifier followed by optional arguments.", opName);
        return makeInt(-1);
    }

    Value target = args[0];
    int builtin_id = -1;
    const char* builtin_name = NULL;
    if (isPascalStringType(VALUE_TYPE(target)) || VALUE_TYPE(target) == TYPE_POINTER) {
        const char* source_name = builtinValueToCString(&target);
        if (!source_name || !*source_name) {
            runtimeError(vm, "%s requires a builtin name or id.", opName);
            return makeInt(-1);
        }
        /* The front end's own spelling first (Aether's "dns_lookup" names
         * the engine's "dnslookup"), then the engine name as written. */
        const char* engine_name = resolveBuiltinNameAlias(source_name);
        builtin_id = engine_name ? getVmBuiltinID(engine_name) : -1;
        if (builtin_id < 0) {
            builtin_id = getVmBuiltinID(source_name);
        }
        builtin_name = getVmBuiltinNameById(builtin_id);
    } else if (IS_INTLIKE(target)) {
        builtin_id = (int)asI64(target);
        builtin_name = getVmBuiltinNameById(builtin_id);
    } else {
        runtimeError(vm, "%s requires a builtin name (string) or id (integer).", opName);
        return makeInt(-1);
    }

    if (builtin_id < 0 || !builtin_name) {
        runtimeError(vm, "%s received an unknown builtin identifier.", opName);
        return makeInt(-1);
    }
        if (!threadBuiltinIsAllowlisted(builtin_id)) {
            runtimeError(vm, "Builtin '%s' is not approved for threaded execution.", builtin_name);
            if (shellRuntimeSetLastStatusSticky) {
                shellRuntimeSetLastStatusSticky(1);
                if (frontendIsShell() && vm) {
                    vm->abort_requested = false;
                    vm->exit_requested = false;
                }
            } else if (shellRuntimeSetLastStatus) {
                shellRuntimeSetLastStatus(1);
                if (frontendIsShell() && vm) {
                    vm->abort_requested = false;
                    vm->exit_requested = false;
                }
            }
            return makeInt(-1);
        }

    /* vmApplyFxPolicy gates a builtin at the CALL_BUILTIN dispatch, but a
     * spawned or pooled builtin job runs its handler directly on the worker,
     * so check the target here: --deny net must stop a dnslookup or
     * httprequest handed to a thread as surely as a direct call. */
    EffectMask target_mask = getVmBuiltinEffectMaskById(builtin_id);
    EffectMask denied_mask = pscalFxEffectiveDeniedMask();
    if ((target_mask & denied_mask) && !pscalFxBuiltinIsDenyExempt(builtin_name)) {
        runtimeError(vm, "VM Error: builtin '%s' denied by --deny/PSCAL_VM_DENY policy (effect mask 0x%x intersects denied 0x%x).",
                     builtin_name, (unsigned)target_mask, (unsigned)denied_mask);
        return makeInt(-1);
    }

    int options_index = -1;
    ThreadRequestOptions options;
    initThreadRequestOptions(&options);
    if (poolSubmit) {
        options.submitOnly = true;
    }
    if (arg_count > 1) {
        const Value* maybe_options = &args[arg_count - 1];
        if (VALUE_TYPE(*maybe_options) == TYPE_RECORD) {
            ThreadRequestOptions parsed = options;
            if (parseThreadRequestOptionsValue(maybe_options, &parsed)) {
                options_index = arg_count - 1;
                options = parsed;
            }
        }
    }

    int builtin_argc = (options_index >= 0 ? options_index : arg_count) - 1;
    if (builtin_argc < 0) {
        builtin_argc = 0;
    }
    const Value* builtin_args = (builtin_argc > 0) ? &args[1] : NULL;

    VM* thread_vm = vm;
    if (vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    const char* thread_name = (options.name[0] != '\0') ? options.name : NULL;
    int thread_id = vmSpawnBuiltinThread(thread_vm, builtin_id, builtin_name, builtin_argc, builtin_args,
                                        options.submitOnly, thread_name);
    if (thread_id < 0) {
        runtimeError(vm, "%s failed to start builtin '%s'.", opName, builtin_name);
        return makeInt(-1);
    }

    if (options.name[0] != '\0') {
        if (!vmThreadAssignName(thread_vm, thread_id, options.name) && thread_vm != vm) {
            vmThreadAssignName(vm, thread_id, options.name);
        }
    }

    Value thread_value = makeInt(thread_id);
    SET_VALUE_TYPE(&thread_value, TYPE_THREAD);
    // SET_VALUE_TYPE resets .bits to a zero placeholder for the new type
    // (it can't know the real value belongs with the type change) --
    // re-tag with the actual thread_id now that the type is correct.
    SET_INT_VALUE(&thread_value, thread_id);
    return thread_value;
}

Value vmBuiltinWaitForThread(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeInt(-1);
    }
    if (arg_count != 1) {
        runtimeError(vm, "WaitForThread expects exactly 1 argument (thread id).");
        return makeInt(-1);
    }

    Value tidVal = args[0];
    if (!(VALUE_TYPE(tidVal) == TYPE_THREAD || IS_INTLIKE(tidVal))) {
        runtimeError(vm, "WaitForThread argument must be a thread id.");
        return makeInt(-1);
    }

    int id = (int)asI64(tidVal);
    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    // Every thread lives in the root VM's pool (thread_vm), whichever VM
    // spawned it; vmJoinThreadById waits as `vm`.
    bool joined = vmJoinThreadById(vm, id);
    if (!joined) {
        bool aborted = (thread_vm && (thread_vm->abort_requested || thread_vm->exit_requested)) ||
                       (vm && (vm->abort_requested || vm->exit_requested));
        if (aborted) {
            return makeInt(-1);
        }
        runtimeError(vm, "WaitForThread received invalid thread id %d.", id);
        return makeInt(-1);
    }

    bool status_flag = true;
    if (vmThreadTakeResult(thread_vm, id, NULL, false, &status_flag, true)) {
        return makeInt(status_flag ? 0 : 1);
    }

    return makeInt(0);
}

Value vmBuiltinThreadSpawnBuiltin(VM* vm, int arg_count, Value* args) {
    return threadSpawnOrSubmitCommon(vm, arg_count, args, false, "ThreadSpawnBuiltin");
}

Value vmBuiltinThreadPoolSubmit(VM* vm, int arg_count, Value* args) {
    return threadSpawnOrSubmitCommon(vm, arg_count, args, true, "ThreadPoolSubmit");
}

Value vmBuiltinThreadGetResult(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeNil();
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "ThreadGetResult expects a thread id and optional consumeStatus flag.");
        return makeNil();
    }

    int thread_id = -1;
    if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "ThreadGetResult argument must be a valid thread id.");
        return makeNil();
    }
    if ((size_t)thread_id >= pscalVmThreadsCeiling()) {
        runtimeError(vm, "ThreadGetResult received thread id %d out of range.", thread_id);
        return makeNil();
    }

    bool consume_status = false;
    if (arg_count == 2) {
        if (!parseBooleanValue(&args[1], &consume_status)) {
            runtimeError(vm, "ThreadGetResult consume flag must be boolean or integer.");
            return makeNil();
        }
    }

    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    if (vmThreadIsRunning(thread_vm, thread_id)) {
        runtimeError(vm, "Thread %d is still running; join it before retrieving the result.", thread_id);
        return makeNil();
    }

    bool status = false;
    Value result = makeNil();
    if (thread_vm && vmThreadTakeResult(thread_vm, thread_id, &result, true, &status, consume_status)) {
        return result;
    }

    if (thread_vm && thread_vm != vm) {
        if (vmThreadIsRunning(vm, thread_id)) {
            runtimeError(vm,
                         "Thread %d is still running; join it before retrieving the result.",
                         thread_id);
            return makeNil();
        }
        if (vmThreadTakeResult(vm, thread_id, &result, true, &status, consume_status)) {
            return result;
        }
    }

    if ((vm && (vm->abort_requested || vm->exit_requested)) ||
        (thread_vm && (thread_vm->abort_requested || thread_vm->exit_requested))) {
        return makeNil();
    }

    runtimeError(vm, "Thread %d has no stored result.", thread_id);
    return makeNil();
}

Value vmBuiltinThreadGetStatus(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeBoolean(false);
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "ThreadGetStatus expects a thread id and optional dropResult flag.");
        return makeBoolean(false);
    }

    int thread_id = -1;
    if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "ThreadGetStatus argument must be a valid thread id.");
        return makeBoolean(false);
    }
    if ((size_t)thread_id >= pscalVmThreadsCeiling()) {
        runtimeError(vm, "ThreadGetStatus received thread id %d out of range.", thread_id);
        return makeBoolean(false);
    }

    bool drop_result = false;
    if (arg_count == 2) {
        if (!parseBooleanValue(&args[1], &drop_result)) {
            runtimeError(vm, "ThreadGetStatus drop flag must be boolean or integer.");
            return makeBoolean(false);
        }
    }

    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    Thread* slot = NULL;
    if (thread_vm && thread_id > 0 && (size_t)thread_id < thread_vm->threadsCommittedCount) {
        slot = &thread_vm->threads[thread_id];
        if (vmThreadIsRunning(thread_vm, thread_id)) {
            runtimeError(vm, "Thread %d is still running; join it before querying status.", thread_id);
            return makeBoolean(false);
        }
        if (!slot->statusReady || slot->statusConsumed) {
            if (drop_result && slot->resultReady) {
                bool dummy_status = false;
                Value drop_value = makeNil();
                if (vmThreadTakeResult(thread_vm,
                                       thread_id,
                                       &drop_value,
                                       true,
                                       &dummy_status,
                                       false)) {
                    freeValue(&drop_value);
                }
            }
            runtimeError(vm, "Thread %d has no stored status.", thread_id);
            return makeBoolean(false);
        }
    }

    bool status = false;
    Value dropped = makeNil();
    if (thread_vm &&
        vmThreadTakeResult(thread_vm, thread_id, drop_result ? &dropped : NULL, drop_result, &status, true)) {
        if (drop_result) {
            freeValue(&dropped);
        }
        return makeBoolean(status);
    }

    if (thread_vm && thread_vm != vm) {
        Thread* fallback_slot = NULL;
        if (thread_id > 0 && (size_t)thread_id < vm->threadsCommittedCount) {
            fallback_slot = &vm->threads[thread_id];
            if (vmThreadIsRunning(vm, thread_id)) {
                runtimeError(vm, "Thread %d is still running; join it before querying status.", thread_id);
                if (drop_result) {
                    freeValue(&dropped);
                }
                return makeBoolean(false);
            }
            if (!fallback_slot->statusReady || fallback_slot->statusConsumed) {
                if (drop_result && fallback_slot->resultReady) {
                    bool dummy_status = false;
                    Value drop_value = makeNil();
                    if (vmThreadTakeResult(vm,
                                           thread_id,
                                           &drop_value,
                                           true,
                                           &dummy_status,
                                           false)) {
                        freeValue(&drop_value);
                    }
                }
                runtimeError(vm, "Thread %d has no stored status.", thread_id);
                if (drop_result) {
                    freeValue(&dropped);
                }
                return makeBoolean(false);
            }
        }
        if (vmThreadTakeResult(vm, thread_id, drop_result ? &dropped : NULL, drop_result, &status, true)) {
            if (drop_result) {
                freeValue(&dropped);
            }
            return makeBoolean(status);
        }
    }

    if ((vm && (vm->abort_requested || vm->exit_requested)) ||
        (thread_vm && (thread_vm->abort_requested || thread_vm->exit_requested))) {
        if (drop_result) {
            freeValue(&dropped);
        }
        return makeBoolean(false);
    }

    runtimeError(vm, "Thread %d has no stored status.", thread_id);
    if (drop_result) {
        freeValue(&dropped);
    }
    return makeBoolean(false);
}

Value vmBuiltinThreadSetName(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeBoolean(false);
    }
    if (arg_count != 2) {
        runtimeError(vm, "ThreadSetName expects exactly 2 arguments (thread id, name).");
        return makeBoolean(false);
    }
    int thread_id = -1;
    if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "ThreadSetName requires a valid thread id.");
        return makeBoolean(false);
    }
    const char* requested = builtinValueToCString(&args[1]);
    if (!requested) {
        runtimeError(vm, "ThreadSetName requires a thread name (string).");
        return makeBoolean(false);
    }
    VM* thread_vm = vm->threadOwner ? vm->threadOwner : vm;
    bool renamed = vmThreadAssignName(thread_vm, thread_id, requested);
    if (!renamed && thread_vm != vm) {
        renamed = vmThreadAssignName(vm, thread_id, requested);
    }
    return makeBoolean(renamed ? 1 : 0);
}

Value vmBuiltinThreadLookup(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeInt(-1);
    }
    if (arg_count != 1) {
        runtimeError(vm, "ThreadLookup expects exactly 1 argument (thread name or id).");
        return makeInt(-1);
    }
    VM* thread_vm = vm->threadOwner ? vm->threadOwner : vm;
    int thread_id = -1;
    const char* lookup = builtinValueToCString(&args[0]);
    if (lookup && *lookup) {
        thread_id = vmThreadFindIdByName(thread_vm, lookup);
        if (thread_id < 0 && thread_vm != vm) {
            thread_id = vmThreadFindIdByName(vm, lookup);
        }
    } else if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "ThreadLookup requires a thread name (string) or id (integer).");
        return makeInt(-1);
    }
    if (thread_id <= 0 || (size_t)thread_id >= pscalVmThreadsCeiling()) {
        return makeInt(-1);
    }
    Value result = makeInt(thread_id);
    SET_VALUE_TYPE(&result, TYPE_THREAD);
    // SET_VALUE_TYPE resets .bits to a zero placeholder for the new type --
    // re-tag with the actual thread_id now that the type is correct.
    SET_INT_VALUE(&result, thread_id);
    return result;
}

static Value threadControlOperation(VM* vm, int arg_count, Value* args, const char* opName,
                                    bool (*operation)(VM*, int)) {
    if (!vm) {
        return makeBoolean(false);
    }
    if (arg_count != 1) {
        runtimeError(vm, "%s expects exactly 1 argument (thread id).", opName);
        return makeBoolean(false);
    }
    int thread_id = -1;
    if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "%s requires a valid thread id.", opName);
        return makeBoolean(false);
    }
    VM* thread_vm = vm->threadOwner ? vm->threadOwner : vm;
    bool success = operation(thread_vm, thread_id);
    if (!success && thread_vm != vm) {
        success = operation(vm, thread_id);
    }
    return makeBoolean(success ? 1 : 0);
}

Value vmBuiltinThreadPause(VM* vm, int arg_count, Value* args) {
    return threadControlOperation(vm, arg_count, args, "ThreadPause", vmThreadPause);
}

Value vmBuiltinThreadResume(VM* vm, int arg_count, Value* args) {
    return threadControlOperation(vm, arg_count, args, "ThreadResume", vmThreadResume);
}

Value vmBuiltinThreadCancel(VM* vm, int arg_count, Value* args) {
    return threadControlOperation(vm, arg_count, args, "ThreadCancel", vmThreadCancel);
}

// VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1): TYPE_TASK
// builtins. Every task lives in the root VM's pool (TaskObj.owner), and a
// TaskObj also carries its job's generation: once the task has been awaited
// or released and its slot reused, TaskAwait/TaskDone/TaskCancel on it no
// longer reach the slot's later job (a stale TaskCancel used to throw away
// the next task's result).
Value vmBuiltinTaskSpawn(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeNil();
    }
    if (arg_count < 1) {
        runtimeError(vm, "TaskSpawn requires at least one argument (a procedure or closure).");
        return makeNil();
    }
    uint32_t generation = 0;
    int id = vmHostCreateTaskEntry(vm, args[0], arg_count - 1, args + 1, &generation);
    if (id < 0) {
        return makeNil();
    }
    return makeTask(id, vm->threadOwner ? vm->threadOwner : vm, generation);
}

Value vmBuiltinTaskAwait(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeNil();
    }
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_TASK) {
        runtimeError(vm, "TaskAwait expects a single task argument.");
        return makeNil();
    }
    TaskObj* t = AS_TASK(args[0]);
    if (!t || !t->owner) {
        runtimeError(vm, "TaskAwait received an invalid task.");
        return makeNil();
    }
    int id = t->threadId;

    // vmThreadTakeResult(..., takeValue=true, takeStatus=true) alone is the
    // complete "await" operation: its own wait loop already blocks (on
    // resultCond) until the job's statusReady fires, then extracts the
    // value/status and -- because both takeValue and takeStatus are true --
    // triggers the release-worker-to-pool gate itself. Do NOT call
    // vmJoinThreadById first: that performs its OWN separate consume-and-
    // release cycle (with a NULL result) and marks the slot readyForReuse
    // before this call ever runs, so by the time this call's wait loop
    // checks `!thread->active && !thread->awaitingReuse` the answer can
    // already be "nothing to wait for" -- confirmed by an actual "Task N
    // has no stored result" failure on a plain void-procedure task during
    // this checkpoint's own smoke test.
    bool status = false;
    Value result = makeNil();
    if (vmTaskTakeResult(vm, id, t->generation, &result, &status)) {
        if (!status) {
            // statusFlag is false for every "didn't succeed" outcome --
            // canceled, killed, or (since the ordinal-vs-nil comparison fix)
            // a genuine runtime-error abort inside the task. The abort
            // itself already printed its own diagnostic from the worker
            // thread at the moment it happened; this just makes sure the
            // awaiting caller ALSO gets a signal instead of a plain nil
            // indistinguishable from a legitimately-nil-returning task.
            runtimeWarning(vm, "Task %d did not complete successfully (canceled, killed, or aborted -- see any earlier error above).", id);
        }
        return result;
    }

    VM* owner = t->owner;
    if ((vm->abort_requested || vm->exit_requested) ||
        (owner->abort_requested || owner->exit_requested)) {
        return makeNil();
    }
    // A non-fatal warning, not runtimeError: reachable via a legitimate
    // race (TaskCancel racing the worker thread's own startup, before
    // Thread.active is even set) that this checkpoint chose not to paper
    // over with extra synchronization -- see vmBuiltinTaskCancel's comment.
    // Aborting the whole program for "the task never got a chance to store
    // anything" would be too harsh for an outcome a caller can reasonably
    // hit just by canceling promptly after spawn.
    runtimeWarning(vm, "Task %d has no stored result (already consumed, or canceled before it started).", id);
    return makeNil();
}

Value vmBuiltinTaskDone(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeBoolean(false);
    }
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_TASK) {
        runtimeError(vm, "TaskDone expects a single task argument.");
        return makeBoolean(false);
    }
    TaskObj* t = AS_TASK(args[0]);
    if (!t || !t->owner) {
        runtimeError(vm, "TaskDone received an invalid task.");
        return makeBoolean(false);
    }
    return makeBoolean(vmTaskIsDone(vm, t->threadId, t->generation));
}

// Cooperative, like vmThreadCancel itself: sets an atomic flag the worker's
// interpreter loop and job-pickup path check at safe points, not preemptive.
// Calling this immediately after TaskSpawn (before the worker thread has
// even reached its job-pickup code and set Thread.active) is a real,
// reachable race, not a hypothetical -- confirmed by this checkpoint's own
// stress fixtures -- and its outcome is simply "the job never really
// started," surfaced as TaskAwait returning nil with a warning rather than
// a stored result (see vmBuiltinTaskAwait's comment). This is considered
// correct, not a bug to fix: matches vmThreadCancel's existing contract for
// plain THREAD_CREATE handles today.
Value vmBuiltinTaskCancel(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeBoolean(false);
    }
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_TASK) {
        runtimeError(vm, "TaskCancel expects a single task argument.");
        return makeBoolean(false);
    }
    TaskObj* t = AS_TASK(args[0]);
    if (!t || !t->owner) {
        runtimeError(vm, "TaskCancel received an invalid task.");
        return makeBoolean(false);
    }
    return makeBoolean(vmTaskCancel(vm, t->threadId, t->generation));
}

// VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2):
// TYPE_CHANNEL builtins. Unlike TaskObj, a ChannelObj carries no owning VM
// -- see its comment in core/types.h -- so these builtins pass `vm` (the
// CALLING VM) straight through to vmChannelSend/Receive only so their
// blocking wait loops can poll cancellation/interrupt state, not to
// address the channel itself.
#define VM_CHANNEL_MAX_CAPACITY (1LL << 24)

Value vmBuiltinChannelCreate(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeNil();
    }
    if (arg_count != 1 || !isIntlikeType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "ChannelCreate expects a single integer capacity argument.");
        return makeNil();
    }
    long long capacity = asI64(args[0]);
    if (capacity < 1) {
        runtimeError(vm, "ChannelCreate requires capacity >= 1.");
        return makeNil();
    }
    // The buffer is allocated and initialised up front, so an absurd
    // capacity is refused here rather than exhausting memory (1e9 slots is
    // 16 GB) or overflowing the size computation.
    if (capacity > VM_CHANNEL_MAX_CAPACITY) {
        runtimeError(vm, "ChannelCreate capacity %lld exceeds the maximum of %lld.",
                     capacity, (long long)VM_CHANNEL_MAX_CAPACITY);
        return makeNil();
    }
    return makeChannel((size_t)capacity);
}

// Blocking; a true procedure (no meaningful return value on success).
// Raises a runtime error on a closed channel -- matching Go's "send on
// closed channel panics" convention, not a silently-tolerated no-op, since
// sending after close is a programming error, not a normal runtime state a
// caller polls for. A canceled/interrupted wait is NOT an error (same
// soft-outcome tolerance TaskCancel racing TaskAwait already has, see
// vmBuiltinTaskAwait's comment above): the value is simply not sent and
// the caller gets no result.
Value vmBuiltinChannelSend(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeVoid();
    }
    if (arg_count != 2 || VALUE_TYPE(args[0]) != TYPE_CHANNEL) {
        runtimeError(vm, "ChannelSend expects (channel, value).");
        return makeVoid();
    }
    ChannelObj* ch = AS_CHANNEL(args[0]);
    if (!ch) {
        runtimeError(vm, "ChannelSend received an invalid channel.");
        return makeVoid();
    }
    VmChannelStatus status = vmChannelSend(vm, ch, makeCopyOfValue(&args[1]));
    if (status == VM_CHANNEL_CLOSED) {
        runtimeError(vm, "ChannelSend: channel is closed.");
    }
    return makeVoid();
}

// Blocking; returns the received value, or nil if the channel is closed
// and drained, or the wait was canceled/interrupted -- same nil-overload
// tolerance TaskAwait already accepts for its own degenerate cases.
// Callers who need to disambiguate a legitimately-sent nil from "nothing
// more will ever arrive" can follow up with ChannelIsClosed (race-free:
// closed is monotonic, so a true result there stays true), or use
// ChannelTryReceive's fully-disambiguated record return instead.
Value vmBuiltinChannelReceive(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeNil();
    }
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_CHANNEL) {
        runtimeError(vm, "ChannelReceive expects a single channel argument.");
        return makeNil();
    }
    ChannelObj* ch = AS_CHANNEL(args[0]);
    if (!ch) {
        runtimeError(vm, "ChannelReceive received an invalid channel.");
        return makeNil();
    }
    Value result = makeNil();
    vmChannelReceive(vm, ch, &result);
    return result;
}

// Non-blocking. Returns 1 if sent, 0 if the buffer is currently full
// (channel still open -- try again later). Raises the same runtime error
// as ChannelSend on a closed channel, for the same reason.
Value vmBuiltinChannelTrySend(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeInt(0);
    }
    if (arg_count != 2 || VALUE_TYPE(args[0]) != TYPE_CHANNEL) {
        runtimeError(vm, "ChannelTrySend expects (channel, value).");
        return makeInt(0);
    }
    ChannelObj* ch = AS_CHANNEL(args[0]);
    if (!ch) {
        runtimeError(vm, "ChannelTrySend received an invalid channel.");
        return makeInt(0);
    }
    VmChannelStatus status = vmChannelTrySend(ch, makeCopyOfValue(&args[1]));
    if (status == VM_CHANNEL_CLOSED) {
        runtimeError(vm, "ChannelTrySend: channel is closed.");
        return makeInt(0);
    }
    return makeInt(status == VM_CHANNEL_OK ? 1 : 0);
}

// Non-blocking procedure: ChannelTryReceive(channel, var status, var
// outValue). Writes an integer status into `status` (1 = got a value, now
// in `outValue`; 0 = buffer momentarily empty, channel still open, try
// again later; -1 = closed and drained, nothing more will ever arrive) and
// the received value (nil if none) into `outValue` -- both genuine `var`
// output parameters, since a channel's payload can be ANY Value type
// (unlike HttpTryAwait's `out: mstream`, which works only because MStream
// is itself a heap handle mutated in place). Two earlier designs were
// tried and abandoned: a {ok, closed, value} record return doesn't work
// because Pascal's compiler requires a compile-time-known record shape for
// `.field` access, which a generic builtin's dynamically-shaped return
// can't provide; and a single `var outValue` with status as this call's
// ordinary return value doesn't work either, because the var-param
// compilation table below only applies when the call is compiled as a
// bare statement (return value discarded) -- exactly Val's/Str's own
// calling shape -- not inside an assignment/expression (confirmed by
// testing both, not assumed). Reusing Val's exact (input, var-out1,
// var-out2) shape, called as a bare statement
// (`ChannelTryReceive(ch, status, outValue);`), is what actually works;
// see compiler.c's per-builtin-name param-index table's
// "channeltryreceive" entry, which compiles both var arguments to
// TYPE_POINTER (the address of the caller's variable) instead of a
// by-value copy.
Value vmBuiltinChannelTryReceive(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeVoid();
    }
    if (arg_count != 3 || VALUE_TYPE(args[0]) != TYPE_CHANNEL ||
        VALUE_TYPE(args[1]) != TYPE_POINTER || VALUE_TYPE(args[2]) != TYPE_POINTER) {
        runtimeError(vm, "ChannelTryReceive expects (channel, var status, var outValue).");
        return makeVoid();
    }
    ChannelObj* ch = AS_CHANNEL(args[0]);
    if (!ch) {
        runtimeError(vm, "ChannelTryReceive received an invalid channel.");
        return makeVoid();
    }
    Value* outStatus = (Value*)AS_POINTER(args[1]);
    Value* outValue = (Value*)AS_POINTER(args[2]);
    Value received = makeNil();
    VmChannelStatus status = vmChannelTryReceive(ch, &received);
    int statusCode = (status == VM_CHANNEL_OK) ? 1 : (status == VM_CHANNEL_CLOSED ? -1 : 0);
    if (outStatus) {
        freeValue(outStatus);
        *outStatus = makeInt(statusCode);
    }
    if (outValue) {
        freeValue(outValue); // release whatever the caller's variable held before this call
        *outValue = received;
    } else {
        freeValue(&received);
    }
    return makeVoid();
}

// A true procedure. Idempotent: closing an already-closed channel is a
// harmless no-op, not an error -- deliberately NOT Go's "close of closed
// channel panics" convention, to avoid a whole new error case in this
// first checkpoint (vmChannelClose already sets `closed` unconditionally
// and re-broadcasts, which is safe either way).
Value vmBuiltinChannelClose(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeVoid();
    }
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_CHANNEL) {
        runtimeError(vm, "ChannelClose expects a single channel argument.");
        return makeVoid();
    }
    ChannelObj* ch = AS_CHANNEL(args[0]);
    if (!ch) {
        runtimeError(vm, "ChannelClose received an invalid channel.");
        return makeVoid();
    }
    vmChannelClose(ch);
    return makeVoid();
}

Value vmBuiltinChannelIsClosed(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeBoolean(true);
    }
    if (arg_count != 1 || VALUE_TYPE(args[0]) != TYPE_CHANNEL) {
        runtimeError(vm, "ChannelIsClosed expects a single channel argument.");
        return makeBoolean(true);
    }
    ChannelObj* ch = AS_CHANNEL(args[0]);
    return makeBoolean(vmChannelIsClosed(ch));
}

Value vmBuiltinThreadStats(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (!vm) {
        return makeEmptyArray(TYPE_RECORD, NULL);
    }
    if (arg_count != 0) {
        runtimeError(vm, "ThreadStats expects no arguments.");
        return makeEmptyArray(TYPE_RECORD, NULL);
    }

    VM* thread_vm = vm->threadOwner ? vm->threadOwner : vm;
    pthread_mutex_lock(&thread_vm->threadRegistryLock);
    int active_count = 0;
    for (size_t i = 1; i < thread_vm->threadsCommittedCount; ++i) {
        Thread *thread = &thread_vm->threads[i];
        const char *name = thread->name;
        bool include = thread->poolWorker || (name && *name && strstr(name, "pool") != NULL);
        if (atomic_load(&thread->inPool) && include && !thread->readyForReuse) {
            active_count++;
        }
    }
    if (active_count <= 0) {
        pthread_mutex_unlock(&thread_vm->threadRegistryLock);
        return makeEmptyArray(TYPE_RECORD, NULL);
    }

    int lower_bounds[1] = {0};
    int upper_bounds[1] = {active_count - 1};
    Value result = makeArrayND(1, lower_bounds, upper_bounds, TYPE_RECORD, NULL);
    int index = 0;
    for (size_t i = 1; i < thread_vm->threadsCommittedCount && index < active_count; ++i) {
        Thread* thread = &thread_vm->threads[i];
        const char *name = thread->name;
        bool include = thread->poolWorker || (name && *name && strstr(name, "pool") != NULL);
        if (!atomic_load(&thread->inPool) || thread->readyForReuse || !include) {
            continue;
        }
        Value entry = makeThreadStateRecord((int)i, thread);
        freeValue(&AS_ARRAY(result)[index]);
        AS_ARRAY(result)[index] = entry;
        index++;
    }
    pthread_mutex_unlock(&thread_vm->threadRegistryLock);
    return result;
}

Value vmBuiltinThreadStatsJson(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (!vm) {
        return makeStringLen("", 0);
    }
    if (arg_count != 0) {
        runtimeError(vm, "ThreadStatsJson expects no arguments.");
        return makeStringLen("", 0);
    }

    VM *thread_vm = vm->threadOwner ? vm->threadOwner : vm;
    pthread_mutex_lock(&thread_vm->threadRegistryLock);

    JsonBuffer buffer = {0, 0, 0};
    bool ok = jsonBufferAppendFormat(&buffer, "[");
    int emitted = 0;
    for (size_t i = 1; i < thread_vm->threadsCommittedCount && ok; ++i) {
        Thread *thread = &thread_vm->threads[i];
        const char *thread_name = thread->name;
        bool include = thread->poolWorker || (thread_name && *thread_name && strstr(thread_name, "pool") != NULL);
        if (!atomic_load(&thread->inPool) || thread->readyForReuse || !include) {
            continue;
        }
        if (emitted > 0) {
            ok = jsonBufferAppendFormat(&buffer, ", ");
        }
        if (!ok) {
            break;
        }
        const char *name = (thread->name[0] != '\0') ? thread->name : "";
        bool reported_idle = thread->idle ||
                             thread->readyForReuse ||
                             (!atomic_load(&thread->active) && !thread->awaitingReuse && thread->currentJob == NULL);
        bool active = atomic_load(&thread->active);
        bool status_flag = thread->statusFlag;
        if ((frontendIsPascal() || frontendIsRea()) && include) {
            active = false;
            reported_idle = true;
            status_flag = false;
        }
        ok = jsonBufferAppendFormat(&buffer, "{\"id\": %d, \"name\": ", (int)i);
        if (ok) {
            ok = jsonAppendEscapedString(&buffer, name);
        }
        if (ok) {
            ok = jsonBufferAppendFormat(&buffer,
                                        ", \"active\": %s, \"idle\": %s, \"status_success\": %s, \"ready_for_reuse\": %s, \"pool_generation\": %d}",
                                        active ? "true" : "false",
                                        reported_idle ? "true" : "false",
                                        status_flag ? "true" : "false",
                                        thread->readyForReuse ? "true" : "false",
                                        thread->poolGeneration);
        }
        emitted++;
    }
    pthread_mutex_unlock(&thread_vm->threadRegistryLock);

    if (ok) {
        ok = jsonBufferAppendFormat(&buffer, "]");
    }

    Value result;
    if (!ok) {
        jsonBufferFree(&buffer);
        result = makeStringLen("[]", 2);
    } else {
        result = makeStringLen(buffer.data ? buffer.data : "", buffer.length);
        jsonBufferFree(&buffer);
    }
    return result;
}

int getBuiltinIDForCompiler(const char *name) {
    return getVmBuiltinID(name);
}

typedef struct {
    char *name;
    BuiltinRoutineType type;
} RegisteredBuiltin;

static RegisteredBuiltin* builtin_registry = NULL;
static int builtin_registry_count = 0;
static int builtin_registry_capacity = 0;
static pthread_once_t builtin_registration_once = PTHREAD_ONCE_INIT;

/*
 * Thread-safe builtin allowlist.
 *
 * Only builtins that are re-entrant and do not mutate global VM state may run
 * on worker threads. Audit new candidates carefully (no shared static buffers,
 * no implicit interaction with the interpreter state) before adding them to
 * this table.
 */
static pthread_once_t gThreadBuiltinAllowlistOnce = PTHREAD_ONCE_INIT;
static bool *gThreadBuiltinAllowlist = NULL;
static size_t gThreadBuiltinAllowlistCount = 0;
static const char *const kThreadBuiltinAllowlistNames[] = {
    "delay",
    "httprequest",
    "httprequesttofile",
    "httprequestasync",
    "httprequestasynctofile",
    "httptryawait",
    "httpawait",
    "httpisdone",
    "httpcancel",
    "httpgetasyncprogress",
    "httpgetasynctotal",
    "httpgetlastheaders",
    "httpgetheader",
    "httpclearheaders",
    "httpsetheader",
    "httpsetoption",
    "httperrorcode",
    "httplasterror",
    "apireceive",
    "apisend",
    "dnslookup"
};

static void initThreadBuiltinAllowlist(void) {
    gThreadBuiltinAllowlistCount = num_vm_builtins;
    if (gThreadBuiltinAllowlistCount == 0) {
        return;
    }

    gThreadBuiltinAllowlist = calloc(gThreadBuiltinAllowlistCount, sizeof(bool));
    if (!gThreadBuiltinAllowlist) {
        gThreadBuiltinAllowlistCount = 0;
        return;
    }

    const size_t count = sizeof(kThreadBuiltinAllowlistNames) / sizeof(kThreadBuiltinAllowlistNames[0]);
    for (size_t i = 0; i < count; ++i) {
        int id = getVmBuiltinID(kThreadBuiltinAllowlistNames[i]);
        if (id >= 0 && (size_t)id < gThreadBuiltinAllowlistCount) {
            gThreadBuiltinAllowlist[id] = true;
        }
    }
}

static bool threadBuiltinIsAllowlisted(int id) {
    if (id < 0) {
        return false;
    }

    pthread_once(&gThreadBuiltinAllowlistOnce, initThreadBuiltinAllowlist);
    if (!gThreadBuiltinAllowlist || gThreadBuiltinAllowlistCount == 0) {
        return false;
    }

    if ((size_t)id < gThreadBuiltinAllowlistCount) {
        return gThreadBuiltinAllowlist[id];
    }
    return false;
}

typedef struct {
    Symbol symbol;
    BuiltinRoutineType builtinType;
    int registryIndex;
} BuiltinTypeEntry;

static HashTable *builtinTypeHash = NULL;

static void destroyBuiltinTypeHashUnlocked(void) {
    if (!builtinTypeHash) {
        return;
    }
    freeHashTable(builtinTypeHash);
    builtinTypeHash = NULL;
}

static BuiltinRoutineType builtinRoutineTypeFromDecl(ASTNodeType declType) {
    return (declType == AST_FUNCTION_DECL)
               ? BUILTIN_TYPE_FUNCTION
               : BUILTIN_TYPE_PROCEDURE;
}

static void registerBuiltinFunctionLinear(const char *name, BuiltinRoutineType type) {
    if (!name) {
        return;
    }

    for (int i = 0; i < builtin_registry_count; ++i) {
        if (strcasecmp(name, builtin_registry[i].name) == 0) {
            builtin_registry[i].type = type;
            return;
        }
    }

    if (builtin_registry_count >= builtin_registry_capacity) {
        int new_capacity = builtin_registry_capacity < 64 ? 64 : builtin_registry_capacity * 2;
        RegisteredBuiltin* new_registry = realloc(builtin_registry, sizeof(RegisteredBuiltin) * new_capacity);
        if (!new_registry) {
            return;
        }
        builtin_registry = new_registry;
        builtin_registry_capacity = new_capacity;
    }

    char *dup_name = strdup(name);
    if (!dup_name) {
        return;
    }

    builtin_registry[builtin_registry_count].name = dup_name;
    builtin_registry[builtin_registry_count].type = type;
    builtin_registry_count++;
}

static BuiltinTypeEntry *lookupBuiltinTypeEntryUnlocked(const char *canonical_name) {
    if (!builtinTypeHash || !canonical_name || !*canonical_name) {
        return NULL;
    }

    Symbol *sym = hashTableLookup(builtinTypeHash, canonical_name);
    if (!sym) {
        return NULL;
    }
    return (BuiltinTypeEntry *)sym;
}

static bool ensureBuiltinTypeHashUnlocked(void) {
    if (builtinTypeHash) {
        return true;
    }

    builtinTypeHash = createHashTable();
    if (!builtinTypeHash) {
        return false;
    }

    for (int i = 0; i < builtin_registry_count; ++i) {
        char canonical[MAX_SYMBOL_LENGTH];
        if (!canonicalizeBuiltinName(builtin_registry[i].name, canonical, sizeof(canonical))) {
            continue;
        }

        BuiltinTypeEntry *entry = calloc(1, sizeof(*entry));
        if (!entry) {
            destroyBuiltinTypeHashUnlocked();
            return false;
        }

        entry->symbol.name = strdup(canonical);
        if (!entry->symbol.name) {
            free(entry);
            destroyBuiltinTypeHashUnlocked();
            return false;
        }
        entry->symbol.is_alias = true;
        entry->builtinType = builtin_registry[i].type;
        entry->registryIndex = i;
        hashTableInsert(builtinTypeHash, &entry->symbol);
    }

    return true;
}

static void registerBuiltinFunctionUnlocked(const char *name,
        ASTNodeType declType,
        const char* unit_context_name_param_for_addproc) {
    (void)unit_context_name_param_for_addproc;
    if (!name) {
        return;
    }

    BuiltinRoutineType builtinType = builtinRoutineTypeFromDecl(declType);

    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    if (!ensureBuiltinTypeHashUnlocked()) {
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    BuiltinTypeEntry *existing = lookupBuiltinTypeEntryUnlocked(canonical);
    if (existing) {
        existing->builtinType = builtinType;
        if (existing->registryIndex >= 0 && existing->registryIndex < builtin_registry_count) {
            builtin_registry[existing->registryIndex].type = builtinType;
        }
        return;
    }

    if (builtin_registry_count >= builtin_registry_capacity) {
        int new_capacity = builtin_registry_capacity < 64 ? 64 : builtin_registry_capacity * 2;
        RegisteredBuiltin* new_registry = realloc(builtin_registry, sizeof(RegisteredBuiltin) * new_capacity);
        if (!new_registry) {
            return;
        }
        builtin_registry = new_registry;
        builtin_registry_capacity = new_capacity;
    }

    BuiltinTypeEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        destroyBuiltinTypeHashUnlocked();
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    entry->symbol.name = strdup(canonical);
    if (!entry->symbol.name) {
        free(entry);
        destroyBuiltinTypeHashUnlocked();
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    char *dup_name = strdup(name);
    if (!dup_name) {
        free(entry->symbol.name);
        free(entry);
        return;
    }

    int index = builtin_registry_count;
    builtin_registry[index].name = dup_name;
    builtin_registry[index].type = builtinType;
    builtin_registry_count++;

    entry->symbol.is_alias = true;
    entry->builtinType = builtinType;
    entry->registryIndex = index;
    hashTableInsert(builtinTypeHash, &entry->symbol);
}

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc) {
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    registerBuiltinFunctionUnlocked(name, declType, unit_context_name_param_for_addproc);
    pthread_mutex_unlock(&builtin_registry_mutex);
}

int isBuiltin(const char *name) {
    if (!name) {
        return 0;
    }

    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);

    int result = 0;
    char canonical[MAX_SYMBOL_LENGTH];
    // While the hash exists it holds every registered name, so a miss there
    // is final; the linear scan (a strcasecmp per registered builtin, on
    // every miss) is only for when the hash could not be built.
    bool hashed = canonicalizeBuiltinName(name, canonical, sizeof(canonical)) && ensureBuiltinTypeHashUnlocked();
    if (hashed) {
        result = lookupBuiltinTypeEntryUnlocked(canonical) != NULL;
    }

    if (!result && !hashed) {
        for (int i = 0; i < builtin_registry_count; ++i) {
            if (strcasecmp(name, builtin_registry[i].name) == 0) {
                result = 1;
                break;
            }
        }
    }

    pthread_mutex_unlock(&builtin_registry_mutex);

    if (!result) {
        result = getVmBuiltinID(name) != -1;
    }

    return result;
}

BuiltinRoutineType getBuiltinType(const char *name) {
    if (!name) {
        return BUILTIN_TYPE_NONE;
    }

    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);

    BuiltinRoutineType result = BUILTIN_TYPE_NONE;
    char canonical[MAX_SYMBOL_LENGTH];
    // See isBuiltin: the linear scan only stands in for a missing hash.
    bool hashed = canonicalizeBuiltinName(name, canonical, sizeof(canonical)) && ensureBuiltinTypeHashUnlocked();
    if (hashed) {
        BuiltinTypeEntry *entry = lookupBuiltinTypeEntryUnlocked(canonical);
        if (entry) {
            result = entry->builtinType;
        }
    }

    if (result == BUILTIN_TYPE_NONE && !hashed) {
        for (int i = 0; i < builtin_registry_count; ++i) {
            if (strcasecmp(name, builtin_registry[i].name) == 0) {
                result = builtin_registry[i].type;
                break;
            }
        }
    }

    pthread_mutex_unlock(&builtin_registry_mutex);

    return result;
}

static void populateBuiltinRegistry(void) {
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    /*
     * Core numeric conversion helpers.  These mirror the small "C-like"
     * casting helpers exposed by several front ends (Rea, CLike, etc.).
     * Historically each front end registered these manually which made the
     * setup fragile—forgetting to do so caused the compiler to fall back to
     * emitting indirect calls and the VM would later report missing globals
     * such as "float".  Register them here so every front end shares the
     * same canonical metadata and the compiler can always resolve their
     * routine types.
     */
    registerBuiltinFunctionUnlocked("int",    AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("double", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("float",  AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("char",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("bool",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("byte",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("toint",    AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("todouble", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tofloat",  AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tochar",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tobool",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tobyte",   AST_FUNCTION_DECL, NULL);

    /* General built-in functions and procedures */
    // Rea/CLike: object allocation helper
    registerBuiltinFunctionUnlocked("newobj", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Abs", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("apiReceive", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("apiSend", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpSession", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpClose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpSetHeader", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpClearHeaders", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpSetOption", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequest", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequestToFile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequestAsync", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequestAsyncToFile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpIsDone", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpTryAwait", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpCancel", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetAsyncProgress", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetAsyncTotal", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpAwait", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpLastError", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetLastHeaders", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpErrorCode", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetHeader", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("DnsLookup", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketAccept", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketBind", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketBindAddr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketClose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketConnect", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketCreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketLastError", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketListen", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketPoll", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketReceive", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketSend", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketSetBlocking", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketPeerAddr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Append", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcCos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcSin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcTan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcTan2", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("atan2", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Assign", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("AssignFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Beep", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Byte", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Ceil", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Chr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Close", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("CloseFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ClrEol", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Copy", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Cos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Cosh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Cotan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("CursorOff", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("CursorOn", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Dec", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Delay", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("DelLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Dispose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosExec", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosFindfirst", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosFindnext", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGetenv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGetfattr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosMkdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosRmdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGetdate", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGettime", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("EOF", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("exec", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Exit", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Exp", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("findFirst", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("findNext", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Floor", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getDate", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("getEnv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getEnvInt", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getFAttr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getTime", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Halt", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HideCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("High", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HighVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Inc", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("InsLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("IntToStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("InvertColors", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("IOResult", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("KeyPressed", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Length", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SetLength", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("SizeOf", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Ln", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Log10", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Low", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Max", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Min", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("clamp", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("mkDir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamCreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamFree", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamFromString", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamLoadFromFile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamSaveToFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamAppendByte", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamBuffer", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("New", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("NormalColors", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Ord", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Odd", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ParamCount", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ParamStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("PopScreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Pos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Power", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("pow", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("PushScreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("QuitRequested", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Random", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Randomize", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ReadKey", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Real", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Format", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("FormatFloat", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("RealToStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Rename", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Erase", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Reset", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("RestoreCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Rewrite", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("rmDir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Round", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SaveCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ScreenCols", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ScreenRows", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ShowCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sinh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sqr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sqrt", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Str", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Succ", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Tan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Tanh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("GotoXY", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BoldText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIBoldText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BlinkText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIBlinkText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("UnderlineText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIUnderlineText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("LowVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BILowVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("NormVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BINormVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ClrScr", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIClrScr", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TermBackground", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextBackground", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextBackgroundE", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextColor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextColorE", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Trunc", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("UpCase", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BytecodeVersion", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Val", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ValReal", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("VMVersion", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Window", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Write", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("WhereX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIWhereX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("WhereY", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIWhereY", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("printf", AST_FUNCTION_DECL, NULL); // special-case handled by compiler
    registerBuiltinFunctionUnlocked("CreateThread", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("WaitForThread", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadSpawnBuiltin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadGetResult", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadGetStatus", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadPoolSubmit", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadSetName", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadLookup", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadPause", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadResume", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadCancel", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadStats", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadStatsJson", AST_FUNCTION_DECL, NULL);
    // VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1)
    registerBuiltinFunctionUnlocked("TaskSpawn", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("TaskAwait", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("TaskDone", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("TaskCancel", AST_FUNCTION_DECL, NULL);
    // VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2)
    registerBuiltinFunctionUnlocked("ChannelCreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ChannelSend", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ChannelReceive", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ChannelTrySend", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ChannelTryReceive", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ChannelClose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ChannelIsClosed", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BlockRead", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BlockWrite", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("FileSize", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("mutex", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("rcmutex", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("lock", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("unlock", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("destroy", AST_PROCEDURE_DECL, NULL);

    // Additional registrations to ensure CLike builtins are classified correctly
    registerBuiltinFunctionUnlocked("Fopen", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Fclose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Fprintf", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Fflush", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Read", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ReadLn", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("DeLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("JsonGet", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ToUpper", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Trim", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("toupper", AST_FUNCTION_DECL, NULL);

    /* Allow externally linked modules to add more builtins. */
    registerExtendedBuiltins();
    /* CLike-style cast helper synonyms to avoid keyword collisions */
    registerVmBuiltin("toint",    vmBuiltinToInt,    BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("todouble", vmBuiltinToDouble, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("tofloat",  vmBuiltinToFloat,  BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("tochar",   vmBuiltinToChar,   BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("tobool",   vmBuiltinToBool,   BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("tobyte",   vmBuiltinToByte,   BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("mstreamfromstring", vmBuiltinMstreamFromString, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("stringofchar", vmBuiltinStringOfChar, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("flush", vmBuiltinFflush, BUILTIN_TYPE_PROCEDURE, "Flush", FX_IO);
    registerVmBuiltin("aether_read_line", vmBuiltinReadLine, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    registerVmBuiltin("aether_read_all_stdin", vmBuiltinReadAllStdin, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    pthread_mutex_unlock(&builtin_registry_mutex);
}
void registerAllBuiltins(void) {
    pthread_once(&builtin_registration_once, populateBuiltinRegistry);
}
