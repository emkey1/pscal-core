/* Auto-generated include: word expansion and substitution logic. Included from shell.c. */
#if defined(PSCAL_TARGET_IOS)
#include "smallclue/smallclue.h"
#undef stdin
#undef stdout
#undef stderr
#define stdin shellVprocStdin()
#define stdout shellVprocStdout()
#define stderr shellVprocStderr()
#endif
#if !defined(PSCAL_TAB_TITLE_SUPPORT)
#if defined(PSCAL_TARGET_IOS) || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
#define PSCAL_TAB_TITLE_SUPPORT 1
#endif
#endif

static char *shellExpandWord(const char *text,
                             uint8_t flags,
                             const char *meta,
                             size_t meta_len,
                             bool **out_quoted_map,
                             size_t *out_quoted_len,
                             bool *out_array_zero);
static bool shellArithmeticEvaluate(const char *expr, long long *out_value, char *error, size_t error_size);

static char *shellNormalizeDollarCommandInline(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len && command[i + 1] == '\n') {
            i++;
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static char *shellNormalizeBacktickCommandInline(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len) {
            char next = command[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
            if (next == '\\' || next == '`' || next == '$') {
                out[j++] = next;
                i++;
                continue;
            }
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static bool shellParseInlineDollarCommand(const char *text,
                                         size_t start,
                                         size_t text_len,
                                         size_t *out_span,
                                         char **out_command) {
    if (!text || start + 1 >= text_len || text[start] != '$' || text[start + 1] != '(') {
        return false;
    }
    size_t stop = shellScanDollarParen(text, text_len, start + 2);
    if (stop == SHELL_SCAN_UNTERMINATED) {
        return false;
    }
    size_t i = stop - 1;
    size_t span = (i + 1) - start;
    if (out_span) {
        *out_span = span;
    }
    if (out_command) {
        size_t inner_start = start + 2;
        size_t inner_len = i - inner_start;
        *out_command = shellNormalizeDollarCommandInline(text + inner_start, inner_len);
        if (!*out_command) {
            return false;
        }
    }
    return true;
}

static bool shellParseInlineBacktickCommand(const char *text,
                                            size_t start,
                                            size_t text_len,
                                            size_t *out_span,
                                            char **out_command) {
    if (!text || start >= text_len || text[start] != '`') {
        return false;
    }
    size_t i = start + 1;
    while (i < text_len) {
        char c = text[i];
        if (c == '`') {
            break;
        }
        if (c == '\\' && i + 1 < text_len) {
            i += 2;
            continue;
        }
        i++;
    }
    if (i >= text_len || text[i] != '`') {
        return false;
    }
    size_t span = (i + 1) - start;
    if (out_span) {
        *out_span = span;
    }
    if (out_command) {
        size_t inner_len = (i - start) - 1;
        *out_command = shellNormalizeBacktickCommandInline(text + start + 1, inner_len);
        if (!*out_command) {
            return false;
        }
    }
    return true;
}

/* Not encoded in a word: only shellExpandHereDocument passes it. */
#define SHELL_WORD_FLAG_HERE_DOCUMENT (1 << 6)

/* Marks a value so a builtin can tell `declare a=(x)` from `declare a="(x)"`. */
#define SHELL_ARRAY_LITERAL_MARK '\x1f'

static void shellSubshellResetState(pid_t dollar_pid);

#include "shell_parameter.inc"

static char *shellExpandHereDocument(const char *body, bool quoted) {
    if (quoted) {
        return body ? strdup(body) : strdup("");
    }
    char *expanded = shellExpandWord(body,
                                     SHELL_WORD_FLAG_HAS_ARITHMETIC | SHELL_WORD_FLAG_HERE_DOCUMENT,
                                     NULL, 0, NULL, NULL, NULL);
    shellFlattenArraySeparators(expanded);
    return expanded;
}

/* <(cmd) and >(cmd). A host gives the command a pipe and the word a /dev/fd path. iOS
 * has no process to run it in and no such path to open, so a file stands in: <(cmd)
 * runs now and >(cmd) reads the file when the command that named it is done. */
typedef struct {
    int fd;
    pid_t pid;
    char *path;
    char *command;
    VM *owner;
} ShellProcSub;

static PSCAL_THREAD_LOCAL ShellProcSub *gShellProcSubs = NULL;
static PSCAL_THREAD_LOCAL size_t gShellProcSubCount = 0;

static bool shellProcSubRemember(ShellProcSub entry) {
    ShellProcSub *grown = (ShellProcSub *)realloc(gShellProcSubs, (gShellProcSubCount + 1) * sizeof(ShellProcSub));
    if (!grown) {
        return false;
    }
    gShellProcSubs = grown;
    gShellProcSubs[gShellProcSubCount++] = entry;
    return true;
}

static int shellProcSubRun(const char *source, const char *label) {
    ShellRunOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.no_cache = 1;
    opts.quiet = true;
    opts.suppress_warnings = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    const char *frontend = shellRuntimeGetArg0();
    opts.frontend_path = frontend ? frontend : "exsh";
    opts.first_line = gShellCurrentCommandLine;
    bool exit_requested = false;
    return shellRunSource(source, label, &opts, &exit_requested);
}

static char *shellProcessSubstitution(char direction, const char *command) {
    fflush(stdout);
    fflush(stderr);
#if defined(PSCAL_TARGET_IOS)
    const char *dir = shellSafeGetenv("TMPDIR");
    char path[PATH_MAX];
    static atomic_uint counter;
    snprintf(path, sizeof(path), "%s/.exsh-procsub-%d-%u", (dir && *dir) ? dir : "/tmp", (int)getpid(),
             atomic_fetch_add(&counter, 1));
    size_t size = strlen(command) + strlen(path) + 32;
    char *script = (char *)malloc(size);
    if (!script) {
        return NULL;
    }
    ShellProcSub entry = {.fd = -1, .pid = -1, .path = strdup(path), .command = NULL, .owner = gShellCurrentVm};
    if (direction == '<') {
        snprintf(script, size, "{ %s\n} > '%s'", command, path);
        int saved_status = gShellRuntime.last_status;
        shellProcSubRun(script, "<process-substitution>");
        shellUpdateStatus(saved_status);
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            close(fd);
        }
        snprintf(script, size, "{ %s\n} < '%s'", command, path);
        entry.command = script;
        script = NULL;
    }
    free(script);
    shellProcSubRemember(entry);
    return strdup(path);
#else
    int fds[2];
    if (pipe(fds) != 0) {
        shellReportRecoverableError(gShellCurrentVm, true, "process substitution: %s", strerror(errno));
        return NULL;
    }
    /* Out of the numbers a script names: `exec 3< <(cmd)` opens this path onto 3, and what goes
     * when the command is done must not be 3. Not close-on-exec, as what the command starts opens
     * the path too. */
    for (int i = 0; i < 2; ++i) {
        int high = fcntl(fds[i], F_DUPFD, SHELL_COMMAND_SUB_READ_FD_MIN);
        if (high >= 0) {
            close(fds[i]);
            fds[i] = high;
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        shellReportRecoverableError(gShellCurrentVm, true, "process substitution: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        for (size_t i = 0; i < gShellProcSubCount; ++i) {
            if (gShellProcSubs[i].fd >= 0) {
                close(gShellProcSubs[i].fd);
            }
        }
        gShellProcSubCount = 0;
        /* The shell ignores it for itself. A closed reader must end this child. */
        signal(SIGPIPE, SIG_DFL);
        dup2(direction == '<' ? fds[1] : fds[0], direction == '<' ? STDOUT_FILENO : STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        shellSubshellResetState(shellRuntimeDollarPid());
        int status = shellProcSubRun(command, "<process-substitution>");
        fflush(stdout);
        fflush(stderr);
        _exit(status);
    }
    int keep = direction == '<' ? fds[0] : fds[1];
    close(direction == '<' ? fds[1] : fds[0]);
    ShellProcSub entry = {.fd = keep, .pid = pid, .path = NULL, .command = NULL, .owner = gShellCurrentVm};
    shellProcSubRemember(entry);
    char path[64];
    snprintf(path, sizeof(path), "/dev/fd/%d", keep);
    return strdup(path);
#endif
}

/* The command that named them is done. What it opened it holds on its own. */
static void shellProcessSubstitutionsFinish(VM *vm) {
#if defined(PSCAL_TARGET_IOS)
    /* A stage on a thread may not have opened its file yet. */
    if (gShellRuntime.pipeline.active && gShellRuntime.pipeline.stage_count > 1) {
        return;
    }
#endif
    size_t kept = 0;
    for (size_t i = 0; i < gShellProcSubCount; ++i) {
        ShellProcSub *entry = &gShellProcSubs[i];
        if (entry->owner != vm) {
            gShellProcSubs[kept++] = *entry;
            continue;
        }
        if (entry->fd >= 0) {
            close(entry->fd);
        }
        if (entry->pid > 0) {
            int status = 0;
            (void)waitpid(entry->pid, &status, WNOHANG);
        }
        if (entry->command) {
            int saved_status = gShellRuntime.last_status;
            shellProcSubRun(entry->command, "<process-substitution>");
            shellUpdateStatus(saved_status);
            free(entry->command);
        }
        if (entry->path) {
            unlink(entry->path);
            free(entry->path);
        }
    }
    gShellProcSubCount = kept;
}

static char *shellExpandParameterEx(const char *input,
                                    bool in_double,
                                    size_t *out_consumed,
                                    bool *out_is_array_expansion,
                                    size_t *out_array_count,
                                    bool **out_map,
                                    size_t *out_map_len) {
    if (out_is_array_expansion) {
        *out_is_array_expansion = false;
    }
    if (out_array_count) {
        *out_array_count = 0;
    }
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (out_map) {
        *out_map = NULL;
    }
    if (out_map_len) {
        *out_map_len = 0;
    }
    if (!input || !*input) {
        return NULL;
    }
    if (*input == '{') {
        size_t stop = shellScanBraceParam(input, strlen(input), 1, in_double);
        if (stop == SHELL_SCAN_UNTERMINATED) {
            return NULL;
        }
        if (out_consumed) {
            *out_consumed = stop;
        }
        return shellExpandBraceParameter(input + 1, stop - 2, in_double, out_is_array_expansion, out_array_count,
                                         out_map, out_map_len);
    }

    if (*input == '$') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", (int)shellRuntimeDollarPid());
        return strdup(buffer);
    }

    if (*input == '?') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
        return strdup(buffer);
    }

    if (*input == '-') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return shellRuntimeOptionFlags();
    }

    if (*input == '!') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        pid_t pid = gShellRuntime.last_bg_pid;
        if (pid <= 0) {
            if (gShellRuntime.nounset_enabled && !gShellNounsetSuppressed) {
                shellReportRecoverableError(gShellCurrentVm, true, "$!: unbound variable");
                shellExpansionFail();
            }
            return strdup("");
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", (int)pid);
        return strdup(buffer);
    }

    if (*input == '#') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gParamCount);
        return strdup(buffer);
    }

    if (*input == '*' || *input == '@') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        size_t array_count = 0;
        if (*input == '*' && in_double) {
            return shellJoinWithIfs(gParamValues, gParamCount > 0 ? (size_t)gParamCount : 0);
        }
        /* Unquoted, $* is $@. */
        char *joined = shellJoinPositionalParameters(true, &array_count);
        if (joined) {
            if (out_is_array_expansion) {
                *out_is_array_expansion = true;
            }
            if (out_array_count) {
                *out_array_count = array_count;
            }
        }
        return joined;
    }

    if (*input == '0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        if (gShellArg0) {
            return strdup(gShellArg0);
        }
        return strdup("exsh");
    }

    /* One digit: $10 is $1 and then a 0. */
    size_t name_len = 0;
    if (isdigit((unsigned char)*input)) {
        name_len = 1;
    } else if (!consumeShellIdentifier(input, strlen(input), &name_len, false)) {
        return NULL;
    }
    if (out_consumed) {
        *out_consumed = name_len;
    }
    bool is_set = false;
    char *value = shellLookupParameterValueInternal(input, name_len, &is_set);
    if (!is_set && gShellRuntime.nounset_enabled && !gShellNounsetSuppressed) {
        /* `$1` by that name, and `${1}` by its number, as bash has it. */
        shellReportRecoverableError(gShellCurrentVm, true, "%s%.*s: unbound variable",
                                    isdigit((unsigned char)*input) ? "$" : "", (int)name_len, input);
        shellExpansionFail();
    }
    return value;
}

static char *shellExpandParameter(const char *input,
                                  size_t *out_consumed,
                                  bool *out_is_array_expansion,
                                  size_t *out_array_count) {
    return shellExpandParameterEx(input, false, out_consumed, out_is_array_expansion, out_array_count, NULL, NULL);
}

static void shellMarkArithmeticError(void) {
    shellUpdateStatus(1);
    gShellArithmeticErrorPending = true;
}

#include "shell_arithmetic.inc"

static char *shellExpandWord(const char *text,
                             uint8_t flags,
                             const char *meta,
                             size_t meta_len,
                             bool **out_quoted_map,
                             size_t *out_quoted_len,
                             bool *out_array_zero) {
    if (out_quoted_map) {
        *out_quoted_map = NULL;
    }
    if (out_quoted_len) {
        *out_quoted_len = 0;
    }
    if (out_array_zero) {
        *out_array_zero = false;
    }
    if (!text) {
        return strdup("");
    }

    bool track_quotes = out_quoted_map && out_quoted_len;
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    size_t quoted_cap = 0;

    ShellMetaSubstitution *subs = NULL;
    size_t sub_count = 0;
    if (!shellParseCommandMetadata(meta, meta_len, &subs, &sub_count)) {
        subs = NULL;
        sub_count = 0;
    }

    size_t text_len = strlen(text);
    size_t length = 0;
    size_t capacity = text_len + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        shellFreeMetaSubstitutions(subs, sub_count);
        return NULL;
    }
    buffer[0] = '\0';

    bool base_single = (flags & SHELL_WORD_FLAG_SINGLE_QUOTED) != 0;
    bool base_double = (flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    bool in_single_segment = false;
    bool in_double_segment = false;
    bool saw_single_marker = false;
    bool saw_double_marker = false;
    size_t sub_index = 0;
    bool here_document = (flags & SHELL_WORD_FLAG_HERE_DOCUMENT) != 0;
    bool assignment_word = (flags & SHELL_WORD_FLAG_ASSIGNMENT) != 0;
    const char *first_eq = assignment_word ? strchr(text, '=') : NULL;

    bool saw_empty_array_expansion = false;

    for (size_t i = 0; i < text_len;) {
        char c = text[i];
        if (c == SHELL_QUOTE_MARK_BYTE && i + 1 < text_len) {
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, true, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, text[i + 1]);
            i += 2;
            continue;
        }
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
            i++;
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
            i++;
            continue;
        }

        bool effective_single = in_single_segment || (!saw_single_marker && base_single);
        bool effective_double = in_double_segment || (!saw_double_marker && base_double);
        bool quoted_flag = effective_single || effective_double;

        if (effective_single) {
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, true, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }

        if (c == '~' && !quoted_flag && !here_document &&
            (i == 0 || (first_eq && text + i > first_eq && (text[i - 1] == ':' || text + i - 1 == first_eq)))) {
            size_t consumed = 0;
            char *home = shellTildeExpansion(text + i, text_len - i, first_eq != NULL, &consumed);
            if (home) {
                /* Whatever it holds is not split or matched again. */
                if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, true,
                                                  strlen(home))) {
                    free(home);
                    goto expand_error;
                }
                shellBufferAppendString(&buffer, &length, &capacity, home);
                free(home);
                i += consumed;
                continue;
            }
        }

        if ((c == '<' || c == '>') && i + 1 < text_len && text[i + 1] == '(' && !quoted_flag && !here_document &&
            !gShellProcSubDisabled) {
            size_t stop = shellScanDollarParen(text, text_len, i + 2);
            if (stop != SHELL_SCAN_UNTERMINATED) {
                char *command = shellNormalizeDollarCommandInline(text + i + 2, stop - i - 3);
                char *path = command ? shellProcessSubstitution(c, command) : NULL;
                free(command);
                if (path) {
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, true,
                                                      strlen(path))) {
                        free(path);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, path);
                    free(path);
                }
                i = stop;
                continue;
            }
        }

        if (c == '$' && i + 2 < text_len && text[i + 1] == '(' && text[i + 2] == '(') {
            size_t stop = shellScanArithmetic(text, text_len, i + 3);
            if (stop != SHELL_SCAN_UNTERMINATED) {
                size_t expr_len = stop - 2 - (i + 3);
                char *expr = (char *)malloc(expr_len + 1);
                if (!expr) {
                    goto expand_error;
                }
                memcpy(expr, text + i + 3, expr_len);
                expr[expr_len] = '\0';
                long long value = 0;
                char error[256];
                if (!shellArithmeticEvaluate(expr, &value, error, sizeof(error))) {
                    if (!gShellExpansionFailed) {
                        shellReportRecoverableError(gShellCurrentVm, true, "%s: %s", expr, error);
                        shellExpansionFail();
                    }
                    free(expr);
                    i = text_len;
                    continue;
                }
                free(expr);
                char result[32];
                snprintf(result, sizeof(result), "%lld", value);
                if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, quoted_flag,
                                                  strlen(result))) {
                    goto expand_error;
                }
                shellBufferAppendString(&buffer, &length, &capacity, result);
                i = stop;
                continue;
            }
        }

        bool handled = false;
        if (sub_index < sub_count) {
            ShellMetaSubstitution *sub = &subs[sub_index];
            size_t span = sub->span_length;
            if (sub->style == SHELL_META_SUBSTITUTION_DOLLAR && c == '$' && i + 1 < text_len && text[i + 1] == '(') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        size_t out_len = strlen(output);
                        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                          quoted_flag, out_len)) {
                            free(output);
                            goto expand_error;
                        }
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            } else if (sub->style == SHELL_META_SUBSTITUTION_BACKTICK && c == '`') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        size_t out_len = strlen(output);
                        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                          quoted_flag, out_len)) {
                            free(output);
                            goto expand_error;
                        }
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            }
        }
        if (handled) {
            continue;
        }

        if (sub_count == 0 && c == '$' && i + 1 < text_len && text[i + 1] == '(') {
            size_t span = 0;
            char *command = NULL;
            if (shellParseInlineDollarCommand(text, i, text_len, &span, &command)) {
                char *output = shellRunCommandSubstitution(command);
                free(command);
                if (output) {
                    size_t out_len = strlen(output);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(output);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, output);
                    free(output);
                }
                i += span;
                continue;
            }
        }
        if (sub_count == 0 && c == '`') {
            size_t span = 0;
            char *command = NULL;
            if (shellParseInlineBacktickCommand(text, i, text_len, &span, &command)) {
                char *output = shellRunCommandSubstitution(command);
                free(command);
                if (output) {
                    size_t out_len = strlen(output);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(output);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, output);
                    free(output);
                }
                i += span;
                continue;
            }
        }

        bool treat_as_double = effective_double || here_document;
        if (c == '\\') {
            if (i + 1 < text_len) {
                char next = text[i + 1];
                if (here_document && next == '\n') {
                    i += 2;
                    continue;
                }
                if (!treat_as_double || next == '$' || (next == '"' && !here_document) || next == '\\' ||
                    next == '`' || next == '\n') {
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, 1)) {
                        goto expand_error;
                    }
                    shellBufferAppendChar(&buffer, &length, &capacity, next);
                    i += 2;
                    continue;
                }
            }
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                              quoted_flag, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }

        if (c == '$') {
            size_t consumed = 0;
            bool array_like = false;
            size_t array_count = 0;
            bool *inner_map = NULL;
            size_t inner_map_len = 0;
            char *expanded = shellExpandParameterEx(text + i + 1,
                                                    quoted_flag || here_document,
                                                    &consumed,
                                                    &array_like,
                                                    &array_count,
                                                    track_quotes ? &inner_map : NULL,
                                                    track_quotes ? &inner_map_len : NULL);
            if (expanded) {
                size_t out_len = strlen(expanded);
                if (array_like && array_count == 0) {
                    saw_empty_array_expansion = true;
                }
                /* ${x:-"a b"}: the quotes inside the word count. */
                bool appended = true;
                if (inner_map && inner_map_len == out_len) {
                    for (size_t k = 0; k < out_len && appended; ++k) {
                        appended = shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                                quoted_flag || inner_map[k], 1);
                    }
                } else {
                    appended = shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                            quoted_flag, out_len);
                }
                free(inner_map);
                if (!appended) {
                    free(expanded);
                    goto expand_error;
                }
                shellBufferAppendString(&buffer, &length, &capacity, expanded);
                free(expanded);
                i += consumed + 1;
                if (gShellExpansionFailed) {
                    i = text_len;
                }
                continue;
            }
        }

        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                          quoted_flag, 1)) {
            goto expand_error;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    shellFreeMetaSubstitutions(subs, sub_count);
    if (track_quotes) {
        *out_quoted_map = quoted_map;
        *out_quoted_len = quoted_len;
    } else {
        free(quoted_map);
    }
    if (out_array_zero && saw_empty_array_expansion && length == 0) {
        *out_array_zero = true;
    }
    return buffer;

expand_error:
    shellFreeMetaSubstitutions(subs, sub_count);
    free(buffer);
    free(quoted_map);
    return NULL;
}

static void shellFreeRedirections(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        free(cmd->redirs[i].path);
        free(cmd->redirs[i].here_doc);
        free(cmd->redirs[i].here_string_literal);
        free(cmd->redirs[i].here_string);
        free(cmd->redirs[i].fd_var);
    }
    free(cmd->redirs);
    cmd->redirs = NULL;
    cmd->redir_count = 0;
}

static void shellFreeCommand(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        free(cmd->assignments[i].text);
    }
    free(cmd->assignments);
    cmd->assignments = NULL;
    cmd->assignment_count = 0;
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    cmd->argv = NULL;
    cmd->argc = 0;
    shellFreeRedirections(cmd);
#if defined(PSCAL_TARGET_IOS)
    if (cmd->body_state) {
        shellBodyStateFree(cmd->body_state);
        cmd->body_state = NULL;
    }
    vprocEnvRelease(cmd->program_env);
    cmd->program_env = NULL;
    free(cmd->program_path);
    cmd->program_path = NULL;
#else
    free(cmd->exec_arg0);
    cmd->exec_arg0 = NULL;
#endif
}

static bool shellParseBool(const char *value, bool *out_flag) {
    if (!value || !out_flag) {
        return false;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        *out_flag = true;
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0) {
        *out_flag = false;
        return true;
    }
    return false;
}

static bool shellStringEqualsIgnoreCase(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    return strcasecmp(lhs, rhs) == 0;
}

static bool shellTryParseIntegerLiteral(const char *text, long long *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellLooksLikeFloatLiteral(const char *text) {
    if (!text) {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') {
            return true;
        }
    }
    if (shellStringEqualsIgnoreCase(text, "inf") ||
        shellStringEqualsIgnoreCase(text, "+inf") ||
        shellStringEqualsIgnoreCase(text, "-inf") ||
        shellStringEqualsIgnoreCase(text, "infinity") ||
        shellStringEqualsIgnoreCase(text, "+infinity") ||
        shellStringEqualsIgnoreCase(text, "-infinity") ||
        shellStringEqualsIgnoreCase(text, "nan") ||
        shellStringEqualsIgnoreCase(text, "+nan") ||
        shellStringEqualsIgnoreCase(text, "-nan")) {
        return true;
    }
    return false;
}

static bool shellTryParseFloatLiteral(const char *text, double *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static Value shellConvertBuiltinArgument(const char *text) {
    if (!text) {
        return makeString("");
    }

    enum {
        SHELL_ARG_MODE_AUTO,
        SHELL_ARG_MODE_STRING,
        SHELL_ARG_MODE_BOOL,
        SHELL_ARG_MODE_INT,
        SHELL_ARG_MODE_FLOAT,
        SHELL_ARG_MODE_NIL
    } mode = SHELL_ARG_MODE_AUTO;

    const char *payload = text;

    if (strncasecmp(payload, "str:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "string:", 7) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 7;
    } else if (strncasecmp(payload, "raw:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "bool:", 5) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 5;
    } else if (strncasecmp(payload, "boolean:", 8) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 8;
    } else if (strncasecmp(payload, "int:", 4) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 4;
    } else if (strncasecmp(payload, "integer:", 8) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 8;
    } else if (strncasecmp(payload, "float:", 6) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 6;
    } else if (strncasecmp(payload, "double:", 7) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 7;
    } else if (strncasecmp(payload, "real:", 5) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 5;
    } else if (strncasecmp(payload, "nil:", 4) == 0) {
        mode = SHELL_ARG_MODE_NIL;
        payload += 4;
    }

    if (mode == SHELL_ARG_MODE_STRING) {
        return makeString(payload ? payload : "");
    }

    if (mode == SHELL_ARG_MODE_NIL) {
        return makeNil();
    }

    if (mode == SHELL_ARG_MODE_BOOL || mode == SHELL_ARG_MODE_AUTO) {
        bool flag = false;
        if (shellParseBool(payload, &flag)) {
            return makeBoolean(flag ? 1 : 0);
        }
        if (mode == SHELL_ARG_MODE_BOOL) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_INT || mode == SHELL_ARG_MODE_AUTO) {
        long long int_value = 0;
        if (shellTryParseIntegerLiteral(payload, &int_value)) {
            return makeInt(int_value);
        }
        if (mode == SHELL_ARG_MODE_INT) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_FLOAT || mode == SHELL_ARG_MODE_AUTO) {
        if (mode == SHELL_ARG_MODE_FLOAT || shellLooksLikeFloatLiteral(payload)) {
            double dbl_value = 0.0;
            if (shellTryParseFloatLiteral(payload, &dbl_value)) {
                return makeDouble(dbl_value);
            }
            if (mode == SHELL_ARG_MODE_FLOAT) {
                return makeString(payload ? payload : "");
            }
        }
    }

    if (mode == SHELL_ARG_MODE_AUTO && payload && *payload) {
        if (shellStringEqualsIgnoreCase(payload, "nil") ||
            shellStringEqualsIgnoreCase(payload, "null")) {
            return makeNil();
        }
    }

    return makeString(payload ? payload : "");
}

static void shellUpdateStatus(int status) {
    if (gShellStatusSticky && status == 0) {
        gShellStatusSticky = false;
        return;
    }
    shellRuntimeSetLastStatus(status);
}

/* The script stops here: the VM at the next instruction, the loops it is in at once. */
static void shellEndWithRequest(void) {
    shellRuntimeRequestExit();
    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = true;
        gShellCurrentVm->current_builtin_name = "signal";
    }
    if (gShellLoopStackSize > 0) {
        gShellRuntime.break_requested = true;
        gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
        shellLoopRequestBreakLevels((int)gShellLoopStackSize);
    }
}

/* The script goes on as it was, with the status it had, unless the trap ended it. */
static void shellRunSignalTrapCommand(int signo) {
    bool saved_sticky = gShellStatusSticky;
    shellRuntimeRunSignalTrap(signo);
    if ((gShellReturnPending || gShellDiscardPending) && !gShellExitRequested) {
        /* `return` in the text: the VM stops for the function it broke into, not for the shell. */
        return;
    }
    if (gShellExitRequested || (gShellCurrentVm && gShellCurrentVm->exit_requested)) {
        shellEndWithRequest();
    } else {
        gShellStatusSticky = saved_sticky;
    }
}

static bool shellRuntimeSignalIsForShell(int signo);

/*
 * POSIX specifies that foreground commands should see the shell's inherited
 * signal dispositions, except that asynchronous lists without job control must
 * inherit SIG_IGN for SIGINT and SIGQUIT, and that traps fire only after the
 * foreground job or wait completes.  We record pending signals in an
 * async-signal-safe manner and reconcile them once we're back in the main
 * interpreter loop so the runtime can unwind cleanly before honouring traps.
 */
static void shellHandleSignalCommon(int signo, bool from_status) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }

    ShellTrapAction trap_action = shellRuntimeGetSignalTrapAction(signo);
    if (trap_action == SHELL_TRAP_ACTION_IGNORE) {
        return;
    }

    bool command_trap = (trap_action == SHELL_TRAP_ACTION_COMMAND);

    if (!command_trap) {
        shellUpdateStatus(128 + signo);
    }

    if (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) {
        bool stop_request = (signo == SIGTSTP) && gShellRuntime.job_control_enabled && shellJobControlSupportsStops();
        bool dispatched = false;
        if (!from_status && (signo == SIGINT || stop_request)) {
            dispatched = shellJobControlDispatchForegroundSignal(signo);
        }
        /* With a trap the script goes on after it, unless the trap ends it. */
        if (gShellCurrentVm && !stop_request && !command_trap) {
            gShellCurrentVm->exit_requested = true;
            gShellCurrentVm->current_builtin_name = "signal";
        }

        if (!gShellRuntime.job_control_enabled && !stop_request && !command_trap) {
            shellRuntimeRequestExit();
        }

        if (gShellLoopStackSize > 0 && !stop_request && !command_trap) {
            gShellRuntime.break_requested = true;
            gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
            shellLoopRequestBreakLevels((int)gShellLoopStackSize);
        }

        bool propagate_default =
            (gShellExitOnSignalFlag && (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) &&
             !gShellRuntime.trap_enabled && !command_trap && !dispatched && !from_status);
#if defined(PSCAL_TARGET_IOS)
        if (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) {
            /* iOS job control is virtualized through vproc; never forward
             * default signal disposition to the host process. */
            propagate_default = false;
        }
#endif

        if (propagate_default) {
            struct sigaction action;
            memset(&action, 0, sizeof(action));
            sigemptyset(&action.sa_mask);
            action.sa_handler = SIG_DFL;
            sigaction(signo, &action, NULL);
            raise(signo);
            if (signo == SIGTSTP) {
                memset(&action, 0, sizeof(action));
                sigemptyset(&action.sa_mask);
                action.sa_handler = shellSignalHandler;
                action.sa_flags |= SA_RESTART;
                sigaction(signo, &action, NULL);
            }
        }

        if (command_trap) {
            if (from_status && shellRuntimeSignalIsForShell(signo)) {
                /* What ran in a substitution died of it. The shell takes it when that is over. */
                shellSignalHandler(signo);
            } else {
                shellRunSignalTrapCommand(signo);
            }
        }
        return;
    }

    /* A command dying of it says nothing about this shell, which has not been sent it. */
    if (command_trap && !from_status) {
        shellRunSignalTrapCommand(signo);
    }
#if defined(PSCAL_TARGET_IOS)
    /* A signal noted with no trap: vprocDeliverTaskSignals said it ends this shell. At a prompt,
     * as in bash, SIGTERM does not. */
    else if (!from_status && (signo != SIGTERM || !shellRuntimeIsInteractive())) {
        shellEndWithRequest();
        if (vprocTaskHalting(shellRuntimeOwnPid())) {
            shellUpdateStatus(0);
        }
    }
#endif
}

static void shellHandlePendingSignal(int signo) {
    shellHandleSignalCommon(signo, false);
}

static void shellHandleStatusSignal(int signo) {
    shellHandleSignalCommon(signo, true);
}

#if defined(PSCAL_TARGET_IOS)
/* On iOS a signal waits with the task until the task asks. Its handler runs here, on the task's
 * thread, however far down the vprocs of its commands that thread is. */
static uint32_t shellRuntimeCaughtSignals(int owner) {
    int fatal = vprocDeliverTaskSignals(owner);
    if (fatal > 0) {
        vprocNoteCaughtSignal(owner, fatal);
    }
    return vprocCaughtSignals(owner);
}
#endif

/* Is what has come for this signal the shell's, around the substitution this thread is in? */
static bool shellRuntimeSignalIsForShell(int signo) {
    uint64_t bit = (uint64_t)1 << (signo % 64);
    return shellRuntimeInSubstitution() && (gShellSubstitutionSignals.held & bit) &&
           !(gShellSubstitutionSignals.own & bit);
}

/* One sent by its own trap comes when that trap is done. Another signal's trap may break in. */
static bool shellRuntimeSignalMustWait(int signo) {
    return (gShellSignalTrapsRunning & ((uint64_t)1 << (signo % 64))) != 0 || shellRuntimeSignalIsForShell(signo);
}

/* What the shell takes afterwards, but what a substitution's process would die of under bash, so the
 * body ends here. Ctrl-C reaches a foreground's whole group. On iOS a signal with no trap waits on
 * the task for someone to act on it. */
static bool shellRuntimeSignalEndsSubstitution(int signo) {
    if (shellRuntimeSignalIsForShell(signo)) {
        return signo == SIGINT || signo == SIGQUIT;
    }
#if defined(PSCAL_TARGET_IOS)
    if (shellRuntimeInSubstitution() && shellRuntimeGetSignalTrapAction(signo) == SHELL_TRAP_ACTION_DEFAULT &&
        signo != SIGINT && signo != SIGQUIT && signo != SIGTSTP) {
        return signo != SIGTERM || !shellRuntimeIsInteractive();
    }
#endif
    return false;
}

static void shellRuntimeEndSubstitution(int signo) {
    if (!gShellExitRequested) {
        shellUpdateStatus(128 + signo);
        shellEndWithRequest();
    }
}

void shellRuntimeProcessPendingSignals(void) {
    bool again = true;
    while (again) {
        again = false;
        /* All that have come, taken together, so that a trap's own commands see only what comes
         * after it started. That may break in, and these run in order. */
        uint64_t taken = 0;
#if defined(PSCAL_TARGET_IOS)
        int owner = shellRuntimeOwnPid();
        uint32_t caught = shellRuntimeCaughtSignals(owner);
        for (int signo = 1; caught != 0 && signo < 32; ++signo) {
            if (!(caught & (1u << signo))) {
                continue;
            }
            if (shellRuntimeSignalEndsSubstitution(signo)) {
                shellRuntimeEndSubstitution(signo);
            } else if (!shellRuntimeSignalMustWait(signo) && vprocTakeCaughtSignal(owner, signo)) {
                taken |= (uint64_t)1 << signo;
            }
        }
#else
        for (int signo = 1; signo < NSIG; ++signo) {
            if (!gShellPendingSignals[signo]) {
                continue;
            }
            if (shellRuntimeSignalEndsSubstitution(signo)) {
                shellRuntimeEndSubstitution(signo);
            } else if (!shellRuntimeSignalMustWait(signo)) {
                gShellPendingSignals[signo] = 0;
                taken |= (uint64_t)1 << (signo % 64);
            }
        }
#endif
        for (int signo = 1; taken != 0 && signo < NSIG; ++signo) {
            uint64_t bit = (uint64_t)1 << (signo % 64);
            if (!(taken & bit)) {
                continue;
            }
            taken &= ~bit;
            shellHandlePendingSignal(signo);
            if (gShellExitRequested) {
                return;
            }
            again = true;
        }
    }
}

/* What `wait` stops for: a signal with a trap, one that ends the shell, or Ctrl-C. */
static int shellRuntimeSignalEndingWait(void) {
#if defined(PSCAL_TARGET_IOS)
    uint32_t caught = shellRuntimeCaughtSignals(shellRuntimeOwnPid());
    for (int signo = 1; caught != 0 && signo < 32; ++signo) {
        if (!(caught & (1u << signo))) {
            continue;
        }
        ShellTrapAction action = shellRuntimeGetSignalTrapAction(signo);
        if (shellRuntimeSignalMustWait(signo) && !shellRuntimeSignalEndsSubstitution(signo)) {
            continue;
        }
        if (action == SHELL_TRAP_ACTION_COMMAND ||
            (action == SHELL_TRAP_ACTION_DEFAULT && signo != SIGQUIT && signo != SIGTSTP)) {
            return signo;
        }
    }
#else
    for (int signo = 1; signo < NSIG; ++signo) {
        if (gShellPendingSignals[signo] && shellRuntimeGetSignalTrapAction(signo) == SHELL_TRAP_ACTION_COMMAND &&
            (!shellRuntimeSignalMustWait(signo) || shellRuntimeSignalEndsSubstitution(signo))) {
            return signo;
        }
    }
#endif
    return 0;
}

static bool shellRuntimeAnySignalTrapped(void) {
    for (int signo = 1; signo < NSIG; ++signo) {
        if (gShellSignalTraps[signo].action == SHELL_TRAP_ACTION_COMMAND) {
            return true;
        }
    }
    return false;
}

void shellRuntimeSetExitOnSignal(bool enabled) {
    gShellExitOnSignalFlag = enabled ? 1 : 0;
}

bool shellRuntimeExitOnSignal(void) {
    return gShellExitOnSignalFlag != 0;
}

void shellRuntimeInitSignals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = shellSignalHandler;
    action.sa_flags |= SA_RESTART;

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGTSTP, &action, NULL);
}

static bool shellHistoryEnsureCapacity(size_t needed) {
    if (gShellHistory.capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellHistory.capacity ? gShellHistory.capacity * 2 : 16;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(char *)) {
        return false;
    }
    char **entries = realloc(gShellHistory.entries, new_capacity * sizeof(char *));
    if (!entries) {
        return false;
    }
    gShellHistory.entries = entries;
    gShellHistory.capacity = new_capacity;
    return true;
}

static bool shellHistoryPersistenceEnabled(void) {
    return gShellRuntime.history_persist_enabled &&
           gShellRuntime.history_path &&
           gShellRuntime.history_path[0] != '\0';
}

static void shellHistoryTrimMemoryLimit(void) {
    if (gShellRuntime.history_size_limit == 0) {
        return;
    }
    if (gShellHistory.count <= gShellRuntime.history_size_limit) {
        return;
    }
    size_t excess = gShellHistory.count - gShellRuntime.history_size_limit;
    for (size_t i = 0; i < excess; ++i) {
        free(gShellHistory.entries[i]);
        gShellHistory.entries[i] = NULL;
    }
    memmove(gShellHistory.entries,
            gShellHistory.entries + excess,
            (gShellHistory.count - excess) * sizeof(char *));
    gShellHistory.count -= excess;
}

static void shellHistoryRewriteFileTail(size_t keep) {
    if (!shellHistoryPersistenceEnabled() || keep == 0) {
        return;
    }
    const char *path = gShellRuntime.history_path;
    if (!path) {
        return;
    }
    char tmp_path[PATH_MAX];
    int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmpXXXXXX", path);
    if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
        return;
    }
    /* mkstemp gives a host descriptor, not a slot. */
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        return;
    }
    size_t start = 0;
    if (gShellHistory.count > keep) {
        start = gShellHistory.count - keep;
    }
    for (size_t i = start; i < gShellHistory.count; ++i) {
        const char *entry = gShellHistory.entries[i];
        if (!entry) {
            continue;
        }
        (void)vprocHostWrite(tmp_fd, entry, strlen(entry));
        (void)vprocHostWrite(tmp_fd, "\n", 1);
    }
    vprocHostFsync(tmp_fd);
    vprocHostClose(tmp_fd);
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
    } else {
        shellHistoryTrimMemoryLimit();
    }
}

static void shellHistoryAppendToFile(const char *line) {
    if (!shellHistoryPersistenceEnabled() || !line || gShellRuntime.history_loading) {
        return;
    }
    int fd = open(gShellRuntime.history_path,
                  O_WRONLY | O_CREAT | O_APPEND
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
                  ,
                  0600);
    if (fd < 0) {
        return;
    }
    size_t len = strlen(line);
    if (len > 0) {
        (void)write(fd, line, len);
    }
    (void)write(fd, "\n", 1);
    fsync(fd);
    close(fd);
    if (gShellRuntime.history_file_size_limit > 0 &&
        gShellHistory.count > gShellRuntime.history_file_size_limit) {
        shellHistoryRewriteFileTail(gShellRuntime.history_file_size_limit);
    }
}

static char *shellHistoryResolvePath(const char *raw_path) {
    if (!raw_path || !*raw_path) {
        return NULL;
    }
    char path_buf[PATH_MAX];
    const char *source = raw_path;
    if (raw_path[0] == '~') {
        const char *home = shellSafeGetenv("HOME");
        if (!home || !*home) {
            return NULL;
        }
        const char *rest = raw_path + 1;
        int written = snprintf(path_buf, sizeof(path_buf), "%s%s",
                               home,
                               (*rest == '/') ? rest : (*rest ? rest : ""));
        if (written < 0 || (size_t)written >= sizeof(path_buf)) {
            return NULL;
        }
        source = path_buf;
    } else {
        size_t len = strlen(raw_path);
        if (len + 1 > sizeof(path_buf)) {
            return NULL;
        }
        memcpy(path_buf, raw_path, len + 1);
        source = path_buf;
    }
#if defined(PSCAL_TARGET_IOS)
    char expanded[PATH_MAX];
    if (pathTruncateExpand(source, expanded, sizeof(expanded))) {
        return strdup(expanded);
    }
#endif
    return strdup(source);
}

static size_t shellHistoryParseSizeEnv(const char *name, size_t fallback) {
    if (!name) {
        return fallback;
    }
    const char *value = shellSafeGetenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0) {
        return fallback;
    }
    return (size_t)parsed;
}

void shellHistoryLoadFromFile(void) {
    if (!shellHistoryPersistenceEnabled() || gShellRuntime.history_loaded_from_file) {
        return;
    }
    const char *path = gShellRuntime.history_path;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    gShellRuntime.history_loading = true;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread = 0;
    while ((nread = getline(&line, &cap, fp)) != -1) {
        if (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            line[nread - 1] = '\0';
        }
        shellRuntimeRecordHistory(line);
        shellHistoryTrimMemoryLimit();
    }
    free(line);
    fclose(fp);
    gShellRuntime.history_loading = false;
    gShellRuntime.history_loaded_from_file = true;
}

void shellHistoryConfigurePersistenceFromEnv(void) {
    const char *histfile = shellSafeGetenv("HISTFILE");
    if (!histfile || !*histfile) {
        gShellRuntime.history_persist_enabled = false;
        return;
    }
    char *resolved = shellHistoryResolvePath(histfile);
    if (!resolved) {
        gShellRuntime.history_persist_enabled = false;
        return;
    }
    free(gShellRuntime.history_path);
    gShellRuntime.history_path = resolved;
    gShellRuntime.history_persist_enabled = true;
    gShellRuntime.history_append_mode = true;
    gShellRuntime.history_size_limit = shellHistoryParseSizeEnv("HISTSIZE", 1000);
    gShellRuntime.history_file_size_limit =
        shellHistoryParseSizeEnv("HISTFILESIZE",
                                 gShellRuntime.history_size_limit ? gShellRuntime.history_size_limit : 0);
}

void shellRuntimeRecordHistory(const char *line) {
    if (!line) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        --len;
    }
    if (len == 0) {
        return;
    }
    bool has_content = false;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != ' ' && line[i] != '\t') {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return;
    }
    if (!shellHistoryEnsureCapacity(gShellHistory.count + 1)) {
        return;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    gShellHistory.entries[gShellHistory.count++] = copy;
    shellHistoryTrimMemoryLimit();
    if (shellRuntimeIsInteractive()) {
        shellHistoryAppendToFile(copy);
    }
}

size_t shellRuntimeHistoryCount(void) {
    return gShellHistory.count;
}

bool shellRuntimeHistoryGetEntry(size_t reverse_index, char **out_line) {
    if (!out_line) {
        return false;
    }
    *out_line = NULL;
    if (reverse_index >= gShellHistory.count) {
        return false;
    }
    const char *entry = gShellHistory.entries[gShellHistory.count - reverse_index - 1];
    if (!entry) {
        return false;
    }
    char *copy = strdup(entry);
    if (!copy) {
        return false;
    }
    *out_line = copy;
    return true;
}

void shellRuntimeSetArg0(const char *name) {
    char *copy = NULL;
    if (name && *name) {
        copy = strdup(name);
        if (!copy) {
            return;
        }
    }
    free(gShellArg0);
    gShellArg0 = copy;
}

const char *shellRuntimeGetArg0(void) {
    return gShellArg0;
}

int shellRuntimeCurrentCommandLine(void) {
    return gShellCurrentCommandLine;
}

int shellRuntimeCurrentCommandColumn(void) {
    return gShellCurrentCommandColumn;
}

void shellRuntimeInitJobControl(void) {
    shellEnsureJobControl();
}

void shellRuntimeRestoreForeground(void) {
    shellEnsureJobControl();
    shellJobControlRestoreForeground();
}

typedef enum {
    SHELL_HISTORY_EXPAND_OK,
    SHELL_HISTORY_EXPAND_NOT_FOUND,
    SHELL_HISTORY_EXPAND_INVALID
} ShellHistoryExpandResult;

static bool shellIsHistoryTerminator(char c) {
    switch (c) {
        case '\0':
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case ';':
        case '&':
        case '|':
        case '(':
        case ')':
        case '<':
        case '>':
            return true;
        default:
            return false;
    }
}

static ShellHistoryExpandResult shellExpandHistoryDesignatorAt(const char *input,
                                                               size_t *out_consumed,
                                                               char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || input[0] != '!') {
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *cursor = input + 1;
    if (*cursor == '\0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *designator_start = NULL;
    size_t designator_len = 0;
    const char *entry = NULL;

    if (*cursor == '$' || *cursor == '*' || *cursor == '^') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
        entry = shellHistoryEntryByIndex(-1);
        if (!entry) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_NOT_FOUND;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    long numeric_index = 0;
    bool has_index = false;
    const char *search_token_start = NULL;
    size_t search_token_len = 0;
    bool search_substring = false;
    bool search_regex = false;

    if (*cursor == '!') {
        numeric_index = -1;
        has_index = true;
        cursor++;
    } else if (*cursor == '-') {
        char *endptr = NULL;
        long value = strtol(cursor + 1, &endptr, 10);
        if (endptr == cursor + 1) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor + 1 - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = -value;
        cursor = endptr;
        has_index = true;
    } else if (isdigit((unsigned char)*cursor)) {
        char *endptr = NULL;
        long value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = value;
        cursor = endptr;
        has_index = true;
    } else if (*cursor == '?') {
        cursor++;
        const char *start = cursor;
        const char *closing = strchr(cursor, '?');
        if (!closing) {
            if (out_consumed) {
                *out_consumed = strlen(input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(closing - start);
        if (search_token_len >= 2 && start[0] == '/' && start[search_token_len - 1] == '/') {
            search_regex = true;
            search_token_start = start + 1;
            search_token_len -= 2;
            if (search_token_len == 0) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        }
        cursor = closing + 1;
        search_substring = true;
    } else {
        const char *start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor) &&
               *cursor != ':' && *cursor != '$' && *cursor != '^' && *cursor != '*') {
            cursor++;
        }
        if (cursor == start) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(cursor - start);
    }

    if (*cursor == '$' || *cursor == '^' || *cursor == '*') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
    } else if (*cursor == ':') {
        cursor++;
        designator_start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor)) {
            cursor++;
        }
        designator_len = (size_t)(cursor - designator_start);
        if (designator_len == 0) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
    }

    if (has_index) {
        entry = shellHistoryEntryByIndex(numeric_index);
    } else if (search_substring) {
        if (search_regex) {
            bool invalid = false;
            entry = shellHistoryFindByRegex(search_token_start, search_token_len, &invalid);
            if (!entry && invalid) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        } else {
            entry = shellHistoryFindBySubstring(search_token_start, search_token_len);
        }
    } else {
        entry = shellHistoryFindByPrefix(search_token_start, search_token_len);
    }

    if (!entry) {
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return SHELL_HISTORY_EXPAND_NOT_FOUND;
    }

    if (out_consumed) {
        *out_consumed = (size_t)(cursor - input);
    }

    if (designator_start && designator_len > 0) {
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    *out_line = strdup(entry);
    if (!*out_line) {
        return SHELL_HISTORY_EXPAND_INVALID;
    }
    return SHELL_HISTORY_EXPAND_OK;
}

/* Bash's own exceptions: $!, ${!name} and [!...] are not history references, and nor is a `!`
 * with nothing to name after it, so `! cmd` and `!` alone reach the parser. */
static bool shellBangIsShellSyntax(const char *input, size_t at) {
    char next = input[at + 1];
    if (next == '\0' || next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '=') {
        return true;
    }
    if (at > 0 && input[at - 1] == '$') {
        return true;
    }
    if (at > 1 && input[at - 1] == '{' && input[at - 2] == '$' && strchr(input + at + 1, '}')) {
        return true;
    }
    return at > 0 && input[at - 1] == '[' && strchr(input + at + 1, ']');
}

bool shellRuntimeExpandHistoryReference(const char *input,
                                        char **out_line,
                                        bool *out_did_expand,
                                        char **out_error_token) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_did_expand) {
        *out_did_expand = false;
    }
    if (out_error_token) {
        *out_error_token = NULL;
    }
    if (!input || !out_line) {
        return false;
    }

    size_t capacity = strlen(input) + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return false;
    }
    buffer[0] = '\0';
    size_t length = 0;

    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; input[i];) {
        char c = input[i];
        if (c == '\\' && !in_single) {
            if (input[i + 1] == '!') {
                shellBufferAppendChar(&buffer, &length, &capacity, '!');
                i += 2;
                continue;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '\'') {
            if (!in_double) {
                in_single = !in_single;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '"') {
            if (!in_single) {
                in_double = !in_double;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '!' && !in_single && !shellBangIsShellSyntax(input, i)) {
            size_t consumed = 0;
            char *replacement = NULL;
            ShellHistoryExpandResult result =
                shellExpandHistoryDesignatorAt(input + i, &consumed, &replacement);
            if (result != SHELL_HISTORY_EXPAND_OK) {
                if (out_error_token) {
                    size_t error_len = consumed > 0 ? consumed : 1;
                    char *token = strndup(input + i, error_len);
                    *out_error_token = token;
                }
                free(replacement);
                free(buffer);
                return false;
            }
            if (replacement) {
                shellBufferAppendString(&buffer, &length, &capacity, replacement);
                free(replacement);
            }
            if (out_did_expand) {
                *out_did_expand = true;
            }
            i += consumed;
            continue;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    *out_line = buffer;
    return true;
}

#if defined(PSCAL_TARGET_IOS)
static VmBuiltinFn shellIOSFrontendHandler(const char *name) {
    if (strcasecmp(name, "clike") == 0) {
        return vmBuiltinShellClike;
    } else if (strcasecmp(name, "pascal") == 0) {
        return vmBuiltinShellPascal;
    } else if (strcasecmp(name, "rea") == 0) {
        return vmBuiltinShellRea;
    } else if (strcasecmp(name, "aether") == 0) {
        return vmBuiltinShellAether;
    } else if (strcasecmp(name, "pscalvm") == 0) {
        return vmBuiltinShellPscalVm;
    } else if (strcasecmp(name, "pscaljson2bc") == 0) {
        return vmBuiltinShellPscalJson2bc;
    } else if (strcasecmp(name, "exsh") == 0 || strcasecmp(name, "sh") == 0) {
        return vmBuiltinShellExshTool;
    }
    return NULL;
}

static bool shellDispatchIOSFrontendBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    char lookup_buf[PATH_MAX];
    const char *name = shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf));
    VmBuiltinFn handler = shellIOSFrontendHandler(name);
    if (!handler) {
        return false;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    Value *args = NULL;
    if (arg_count > 0) {
        args = calloc((size_t)arg_count, sizeof(Value));
        if (!args) {
            shellUpdateStatus(1);
            return true;
        }
        for (int i = 0; i < arg_count; ++i) {
            args[i] = makeString(cmd->argv[i + 1]);
        }
    }
    const char *previous_name = shellRuntimeCurrentCommandName();
    shellRuntimeSetCurrentCommandName(name);
    bool previous_background = gShellCurrentBuiltinBackground;
    gShellCurrentBuiltinBackground = cmd->background;
    handler(vm, arg_count, args);
    gShellCurrentBuiltinBackground = previous_background;
    shellRuntimeSetCurrentCommandName(previous_name);
    if (args) {
        for (int i = 0; i < arg_count; ++i) {
            freeValue(&args[i]);
        }
        free(args);
    }
    return true;
}
#endif

static bool shellIsRuntimeBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (shellRuntimeBuiltinDisabled(name)) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    /* Frontends are statically linked on iOS; treat them as builtins even if
     * their compiler ids were not resolved. */
    if (strcasecmp(name, "clike") == 0 ||
        strcasecmp(name, "pascal") == 0 ||
        strcasecmp(name, "rea") == 0 ||
        strcasecmp(name, "pscalvm") == 0 ||
        strcasecmp(name, "pscaljson2bc") == 0 ||
        strcasecmp(name, "exsh") == 0 || strcasecmp(name, "sh") == 0) {
        return true;
    }
#endif
    return shellIsBuiltinName(name);
}

static bool shellInvokeFunction(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    const char *name = cmd->argv[0];
    ShellCompiledFunction *compiled = shellFindCompiledFunction(name);
    if (!compiled) {
        return false;
    }
    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;
    char **function_params = NULL;
    int function_count = (cmd->argc > 1) ? (int)cmd->argc - 1 : 0;
    if (function_count > 0) {
        function_params = (char **)calloc((size_t)function_count, sizeof(char *));
        if (!function_params) {
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        bool ok = true;
        for (int i = 0; i < function_count; ++i) {
            const char *arg = cmd->argv[i + 1] ? cmd->argv[i + 1] : "";
            function_params[i] = strdup(arg);
            if (!function_params[i]) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            for (int i = 0; i < function_count; ++i) {
                free(function_params[i]);
            }
            free(function_params);
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        gParamValues = function_params;
        gParamCount = function_count;
        gShellPositionalOwned = true;
    } else {
        gParamValues = NULL;
        gParamCount = 0;
        gShellPositionalOwned = false;
    }
    bool scope_pushed = shellLocalScopePush();
    if (!scope_pushed) {
        runtimeError(vm, "%s: out of memory", name);
        shellUpdateStatus(1);
        if (gShellPositionalOwned) {
            shellFreeOwnedPositionalParameters();
        } else {
            gParamValues = NULL;
            gParamCount = 0;
        }
        gParamValues = saved_params;
        gParamCount = saved_count;
        gShellPositionalOwned = saved_owned;
        return true;
    }
    static VM function_vm;
    static bool function_vm_initialized = false;
    static bool function_vm_in_use = false;
    VM *invoke_vm = NULL;
    VM *nested_vm = NULL;
    bool using_cached_vm = false;
    bool nested_vm_initialized = false;
    bool frame_pushed = false;
    bool caller_traps_back = false;
    InterpretResult result = INTERPRET_OK;
    size_t redirect_scope_depth = shellRuntimeRedirectScopeDepth();
    size_t loop_depth = gShellLoopStackSize;
    size_t case_depth = gShellCaseStack.count;
    int condition_depth = gShellConditionDepth;
    bool err_trap_at_call = shellTrapIsLive(&gShellErrTrap);
    ShellCallerTraps caller_traps;
    shellCallerTrapsPutAside(&caller_traps, shellFunctionIsTraced(name));
    /* A bare `return` in a function a trap calls is about the function. */
    bool saved_trap_status_valid = gShellTrapStatusValid;
    gShellTrapStatusValid = false;
    /* The pipeline that called the function is still the shell's: its negation and status
     * apply when the call returns, whatever the body's pipelines do. A stage's context may
     * be in use by the stages beside it, and those keep it. */
    ShellPipelineContext *caller_pipeline = &gShellRuntime.pipeline;
    ShellPipelineContext saved_pipeline = *caller_pipeline;
    bool pipeline_saved = caller_pipeline->stage_count <= 1;
    if (pipeline_saved) {
        memset(caller_pipeline, 0, sizeof(*caller_pipeline));
        caller_pipeline->pgid = -1;
    }
    if (!__atomic_test_and_set(&function_vm_in_use, __ATOMIC_ACQUIRE)) {
        if (!function_vm_initialized) {
            initVM(&function_vm);
            function_vm_initialized = true;
        } else {
            vmResetExecutionState(&function_vm);
        }
        using_cached_vm = true;
        invoke_vm = &function_vm;
    } else {
        nested_vm = (VM *)malloc(sizeof(VM));
        if (!nested_vm) {
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            goto invoke_cleanup;
        }
        initVM(nested_vm);
        invoke_vm = nested_vm;
        nested_vm_initialized = true;
    }

    if (!invoke_vm) {
        goto invoke_cleanup;
    }
    invoke_vm->frontendContext = vm ? vm->frontendContext : NULL;

    if (!shellRuntimePushCallFrame(name, cmd->line, cmd->column)) {
        runtimeError(vm, "%s: out of memory", name);
        shellUpdateStatus(1);
        goto invoke_cleanup;
    }
    frame_pushed = true;

    /* Innermost first, in the order FUNCNAME shows them. */
    bool name_pushed = false;
    if (gShellFunctionNameCount == gShellFunctionNameCapacity) {
        size_t capacity = gShellFunctionNameCapacity ? gShellFunctionNameCapacity * 2 : 8;
        char **grown = (char **)realloc(gShellFunctionNames, capacity * sizeof(char *));
        if (grown) {
            gShellFunctionNames = grown;
            gShellFunctionNameCapacity = capacity;
        }
    }
    if (gShellFunctionNameCount < gShellFunctionNameCapacity) {
        memmove(gShellFunctionNames + 1, gShellFunctionNames, gShellFunctionNameCount * sizeof(char *));
        gShellFunctionNames[0] = (char *)name;
        gShellFunctionNameCount++;
        name_pushed = true;
    }

    gShellReturnScopeDepth++;
    result = interpretBytecode(invoke_vm, &compiled->chunk,
                               globalSymbols, constGlobalSymbols, procedure_table, 0);
    shellRuntimeFinishChunk(invoke_vm);
    bool returned = gShellReturnPending;
    if (returned || gShellDiscardPending) {
        /* It may have left a condition of its own. One the call is in still holds:
         * `set -e; if f; then` ignores a failure inside f. */
        gShellConditionDepth = condition_depth;
    }
    gShellReturnPending = false;
    int call_status = (result != INTERPRET_OK && !gShellExitRequested) ? 1 : shellRuntimeLastStatus();
    if (!gShellDiscardPending) {
        shellRuntimeRunReturnTrap(returned, call_status);
    }
    gShellReturnScopeDepth--;
    shellCallerTrapsBack(&caller_traps);
    caller_traps_back = true;
    gShellTrapStatusValid = saved_trap_status_valid;
    /* The caller's ERR trap and errexit answer to the call. */
    shellUpdateStatus(call_status);
    gShellErrTrapSkipOnce = !err_trap_at_call;
    if (name_pushed) {
        gShellFunctionNameCount--;
        memmove(gShellFunctionNames, gShellFunctionNames + 1, gShellFunctionNameCount * sizeof(char *));
    }
    if ((gShellExitRequested || gShellDiscardPending) && vm) {
        /* `exit` ended this chunk. It ends the one that called the function too. */
        vm->exit_requested = true;
    }

invoke_cleanup:
    if (!caller_traps_back) {
        shellCallerTrapsBack(&caller_traps);
        gShellTrapStatusValid = saved_trap_status_valid;
    }
    shellRuntimeUnwindRedirectScopes(redirect_scope_depth);
    shellUnwindChunk(loop_depth, case_depth);
    if (pipeline_saved) {
        if (!shellRuntimeChunkLeftStages()) {
            shellResetPipeline();
        }
        *caller_pipeline = saved_pipeline;
    }
    if (using_cached_vm) {
        vmResetExecutionState(&function_vm);
        __atomic_clear(&function_vm_in_use, __ATOMIC_RELEASE);
    } else if (nested_vm) {
        if (nested_vm_initialized) {
            freeVM(nested_vm);
        }
        free(nested_vm);
    }
    if (frame_pushed) {
        shellRuntimePopCallFrame();
    }
    if (scope_pushed) {
        shellLocalScopePop();
    }
    if (gShellPositionalOwned) {
        shellFreeOwnedPositionalParameters();
    } else {
        gParamValues = NULL;
        gParamCount = 0;
    }
    gParamValues = saved_params;
    gParamCount = saved_count;
    gShellPositionalOwned = saved_owned;
    return true;
}

/* What a subshell does not inherit, or must not touch: the parent's pipeline (its pipe
 * ends are closed here or are not ours), jobs, job control and traps. */
static void shellSubshellResetState(pid_t dollar_pid) {
    gShellRuntime.subshell_parent_pid = dollar_pid;
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    free(ctx->pids);
    free(ctx->merge_stderr);
    free(ctx->pipes);
    memset(ctx, 0, sizeof(*ctx));
    ctx->pgid = -1;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        free(gShellJobs[i].pids);
        free(gShellJobs[i].command);
    }
    free(gShellJobs);
    gShellJobs = NULL;
    gShellJobCount = 0;
    gShellRuntime.finished_job_count = 0;
    gShellRuntime.job_control_enabled = false;
    gShellRuntime.job_control_initialized = true;
    for (int signo = 1; signo < NSIG; ++signo) {
        if (gShellSignalTraps[signo].action == SHELL_TRAP_ACTION_COMMAND) {
            shellTrapClearEntry(&gShellSignalTraps[signo]);
#if !defined(PSCAL_TARGET_IOS)
            signal(signo, SIG_DFL);
#endif
        }
    }
    if (gShellExitTrap.action == SHELL_TRAP_ACTION_COMMAND) {
        shellTrapClearEntry(&gShellExitTrap);
    }
    gShellTextDepth = 0;
    /* set -E and set -T hand these on to a subshell. It lists them either way. */
    gShellErrTrap.dormant = gShellErrTrap.dormant || !gShellRuntime.errtrace_enabled;
    gShellReturnTrap.dormant = gShellReturnTrap.dormant || !gShellRuntime.functrace_enabled;
    if (!gShellRuntime.functrace_enabled) {
        shellTrapClearEntry(&gShellDebugTrap);
    }
#if !defined(PSCAL_TARGET_IOS)
    memset((void *)gShellPendingSignals, 0, sizeof(gShellPendingSignals));
    memset(&gShellSubstitutionSignals, 0, sizeof(gShellSubstitutionSignals));
#endif
    gShellRuntime.in_subshell = true;
#if !defined(PSCAL_TARGET_IOS)
    shellCommandSubForgetFrames();
#endif
#if defined(PSCAL_TARGET_IOS)
    if (!gShellRuntime.has_private_tables) {
        pthread_mutex_lock(&gShellFunctionTableMutex);
        shellCopyDefinitionTables(&gShellRuntime,
                                  gShellFunctionTable,
                                  gShellFunctionTableCount,
                                  gShellAliasTable,
                                  gShellAliasTableCount,
                                  gShellOptionTable);
        pthread_mutex_unlock(&gShellFunctionTableMutex);
    }
#endif
}

#if defined(PSCAL_TARGET_IOS)
static void shellCallStackFree(ShellCallStack *stack) {
    for (size_t i = 0; i < stack->frame_count; ++i) {
        shellRuntimeFreeCallFrame(&stack->frames[i]);
    }
    free(stack->frames);
    for (size_t i = 0; i < stack->source_count; ++i) {
        free(stack->sources[i]);
    }
    free(stack->sources);
    free(stack->function_names);
    free(stack->source_names);
    for (size_t i = 0; i < stack->held_count; ++i) {
        free(stack->held[i]);
    }
    free(stack->held);
    memset(stack, 0, sizeof(*stack));
}

static char *shellCallStackDup(const char *text) {
    return text ? strdup(text) : NULL;
}

static char **shellCallStackHoldNames(ShellCallStack *stack, char **names, size_t count) {
    char **copy = count > 0 ? (char **)calloc(count, sizeof(char *)) : NULL;
    for (size_t i = 0; copy && i < count; ++i) {
        copy[i] = strdup(names[i] ? names[i] : "");
        stack->held[stack->held_count++] = copy[i];
    }
    return copy;
}

static void shellCallStackCapture(ShellCallStack *stack) {
    memset(stack, 0, sizeof(*stack));
    if (gShellCallStackCount > 0) {
        stack->frames = (ShellCallFrame *)calloc(gShellCallStackCount, sizeof(ShellCallFrame));
        for (size_t i = 0; stack->frames && i < gShellCallStackCount; ++i) {
            stack->frames[i].function = shellCallStackDup(gShellCallStack[i].function);
            stack->frames[i].caller_function = shellCallStackDup(gShellCallStack[i].caller_function);
            stack->frames[i].source = shellCallStackDup(gShellCallStack[i].source);
            stack->frames[i].line = gShellCallStack[i].line;
            stack->frames[i].column = gShellCallStack[i].column;
            stack->frame_count = i + 1;
        }
    }
    if (gShellSourceDepth > 0) {
        stack->sources = (char **)calloc(gShellSourceDepth, sizeof(char *));
        for (size_t i = 0; stack->sources && i < gShellSourceDepth; ++i) {
            stack->sources[i] = strdup(gShellSourceStack[i] ? gShellSourceStack[i] : "");
            stack->source_count = i + 1;
        }
    }
    size_t names = gShellFunctionNameCount + gShellSourceNameCount;
    stack->held = names > 0 ? (char **)calloc(names, sizeof(char *)) : NULL;
    if (stack->held) {
        stack->function_names = shellCallStackHoldNames(stack, gShellFunctionNames, gShellFunctionNameCount);
        stack->function_name_count = stack->function_names ? gShellFunctionNameCount : 0;
        stack->source_names = shellCallStackHoldNames(stack, gShellSourceNames, gShellSourceNameCount);
        stack->source_name_count = stack->source_names ? gShellSourceNameCount : 0;
    }
}

/* This thread takes the copy on, and what it had goes in the copy's place. A push grows any of
 * them again, so a count will do as a capacity. */
static void shellCallStackSwap(ShellCallStack *stack) {
    ShellCallStack had = {gShellCallStack, gShellCallStackCount, gShellSourceStack, gShellSourceDepth,
                          gShellFunctionNames, gShellFunctionNameCount, gShellSourceNames, gShellSourceNameCount,
                          stack->held, stack->held_count};
    gShellCallStack = stack->frames;
    gShellCallStackCount = stack->frame_count;
    gShellCallStackCapacity = stack->frame_count;
    gShellSourceStack = stack->sources;
    gShellSourceDepth = stack->source_count;
    gShellSourceCapacity = stack->source_count;
    gShellFunctionNames = stack->function_names;
    gShellFunctionNameCount = stack->function_name_count;
    gShellFunctionNameCapacity = stack->function_name_count;
    gShellSourceNames = stack->source_names;
    gShellSourceNameCount = stack->source_name_count;
    *stack = had;
}

static void shellVariableAttributesFree(ShellVariableAttribute *attributes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(attributes[i].name);
    }
    free(attributes);
}

static void shellBodyStateFree(ShellBodyState *state) {
    if (!state) {
        return;
    }
    if (state->ctx) {
        shellRuntimeDestroyContext(state->ctx);
    }
    vprocEnvRelease(state->env);
    if (state->params) {
        shellFreeParameterArray(state->params, state->param_count);
    }
    shellVariableAttributesFree(state->attributes, state->attribute_count);
    free(state->err_trap);
    free(state->return_trap);
    shellCallStackFree(&state->calls);
    free(state);
}

static ShellBodyState *shellBodyStateCapture(void) {
    ShellBodyState *state = (ShellBodyState *)calloc(1, sizeof(ShellBodyState));
    if (!state) {
        return NULL;
    }
    state->ctx = shellRuntimeCreateContext();
    state->env = vprocEnvCapture();
    if (!state->ctx || !state->env) {
        shellBodyStateFree(state);
        return NULL;
    }
    /* Now, not when the thread gets going: `f & unset -f f` still runs f. */
    if (!state->ctx->has_private_tables) {
        pthread_mutex_lock(&gShellFunctionTableMutex);
        shellCopyDefinitionTables(state->ctx,
                                  gShellFunctionTable,
                                  gShellFunctionTableCount,
                                  gShellAliasTable,
                                  gShellAliasTableCount,
                                  gShellOptionTable);
        pthread_mutex_unlock(&gShellFunctionTableMutex);
    }
    shellRuntimeContextInheritLocals(state->ctx, 0);
    ShellExportFlags *flags = shellExportFlagsSnapshot();
    if (flags) {
        vprocEnvSetAttachment(state->env, flags, shellExportFlagsFree);
    }
    if (gParamCount > 0 && gParamValues) {
        state->params = (char **)calloc((size_t)gParamCount, sizeof(char *));
        if (!state->params) {
            shellBodyStateFree(state);
            return NULL;
        }
        for (int i = 0; i < gParamCount; ++i) {
            state->params[i] = strdup(gParamValues[i] ? gParamValues[i] : "");
            state->param_count = i + 1;
            if (!state->params[i]) {
                shellBodyStateFree(state);
                return NULL;
            }
        }
    }
    if (!vprocShellGetcwdShim(state->cwd, sizeof(state->cwd))) {
        state->cwd[0] = '\0';
    }
    state->dollar_pid = shellRuntimeDollarPid();
    /* A thread's own, and the stage's thread is another. */
    state->ifs_unset = gShellIfsUnset;
    /* Per thread there, and the body's thread may be another. */
    if (gShellErrTrap.command) {
        state->err_trap = strdup(gShellErrTrap.command);
        state->traps_dormant_err = gShellErrTrap.dormant;
    }
    if (gShellReturnTrap.command) {
        state->return_trap = strdup(gShellReturnTrap.command);
        state->traps_dormant_return = gShellReturnTrap.dormant;
    }
    shellCallStackCapture(&state->calls);
    state->in_loop = gShellLoopStackSize > 0;
    if (gShellVariableAttributeCount > 0) {
        state->attributes = (ShellVariableAttribute *)calloc(gShellVariableAttributeCount, sizeof(ShellVariableAttribute));
        if (!state->attributes) {
            shellBodyStateFree(state);
            return NULL;
        }
        for (size_t i = 0; i < gShellVariableAttributeCount; ++i) {
            state->attributes[i].name = strdup(gShellVariableAttributes[i].name);
            state->attributes[i].flags = gShellVariableAttributes[i].flags;
            state->attribute_count = i + 1;
            if (!state->attributes[i].name) {
                shellBodyStateFree(state);
                return NULL;
            }
        }
    }
    return state;
}

typedef struct {
    ShellRuntimeState *previous_ctx;
    VProcEnv *previous_env;
    int previous_subshell_pid;
    char **saved_params;
    int saved_param_count;
    bool saved_params_owned;
    ShellVariableAttribute *saved_attributes;
    size_t saved_attribute_count;
    bool saved_ifs_unset;
    ShellTrapEntry saved_err_trap;
    ShellTrapEntry saved_debug_trap;
    ShellTrapEntry saved_return_trap;
} ShellBodyScope;

static void shellBodyScopeEnter(ShellBodyState *state, ShellBodyScope *scope) {
    memset(scope, 0, sizeof(*scope));
    scope->previous_subshell_pid = vprocSubshellPid();
    scope->saved_params = gParamValues;
    scope->saved_param_count = gParamCount;
    scope->saved_params_owned = gShellPositionalOwned;
    if (!state) {
        return;
    }
    scope->previous_ctx = shellRuntimeActivateContext(state->ctx);
    /* The body may run on the shell's own thread, whose traps come back with it. */
    scope->saved_err_trap = gShellErrTrap;
    scope->saved_debug_trap = gShellDebugTrap;
    scope->saved_return_trap = gShellReturnTrap;
    memset(&gShellErrTrap, 0, sizeof(gShellErrTrap));
    memset(&gShellDebugTrap, 0, sizeof(gShellDebugTrap));
    memset(&gShellReturnTrap, 0, sizeof(gShellReturnTrap));
    if (state->err_trap) {
        gShellErrTrap.action = SHELL_TRAP_ACTION_COMMAND;
        gShellErrTrap.command = state->err_trap;
        gShellErrTrap.dormant = state->traps_dormant_err;
        state->err_trap = NULL;
    }
    if (state->return_trap) {
        gShellReturnTrap.action = SHELL_TRAP_ACTION_COMMAND;
        gShellReturnTrap.command = state->return_trap;
        gShellReturnTrap.dormant = state->traps_dormant_return;
        state->return_trap = NULL;
    }
    shellSubshellResetState(state->dollar_pid);
    scope->previous_env = vprocEnvActivate(state->env);
    VProc *self = vprocCurrent();
    if (self) {
        vprocSetSubshellPid(vprocPid(self));
    }
    if (state->cwd[0]) {
        (void)vprocChdirShim(state->cwd);
    }
    gParamValues = state->params;
    gParamCount = state->param_count;
    gShellPositionalOwned = (state->params != NULL);
    state->params = NULL;
    scope->saved_attributes = gShellVariableAttributes;
    scope->saved_attribute_count = gShellVariableAttributeCount;
    gShellVariableAttributes = state->attributes;
    gShellVariableAttributeCount = state->attribute_count;
    state->attributes = NULL;
    state->attribute_count = 0;
    scope->saved_ifs_unset = gShellIfsUnset;
    gShellIfsUnset = state->ifs_unset;
    shellCallStackSwap(&state->calls);
    (void)vprocConsumeClosedPipe();
}

static void shellBodyScopeLeave(ShellBodyState *state, const ShellBodyScope *scope) {
    if (!state) {
        return;
    }
    if (gShellPositionalOwned) {
        shellFreeOwnedPositionalParameters();
    }
    gParamValues = scope->saved_params;
    gParamCount = scope->saved_param_count;
    gShellPositionalOwned = scope->saved_params_owned;
    shellVariableAttributesFree(gShellVariableAttributes, gShellVariableAttributeCount);
    gShellVariableAttributes = scope->saved_attributes;
    gShellVariableAttributeCount = scope->saved_attribute_count;
    gShellIfsUnset = scope->saved_ifs_unset;
    shellTrapClearEntry(&gShellErrTrap);
    shellTrapClearEntry(&gShellDebugTrap);
    shellTrapClearEntry(&gShellReturnTrap);
    gShellErrTrap = scope->saved_err_trap;
    gShellDebugTrap = scope->saved_debug_trap;
    gShellReturnTrap = scope->saved_return_trap;
    shellCallStackSwap(&state->calls);
    vprocEnvActivate(scope->previous_env);
    vprocSetSubshellPid(scope->previous_subshell_pid);
    shellRuntimeActivateContext(scope->previous_ctx);
    shellBodyStateFree(state);
}
#endif

/* A compound command run apart from the shell. On a host this is a forked child, which
 * has its own everything. On iOS it is this thread and the vproc it runs under, with what
 * shellBodyStateCapture took. `exit` ends the body, and its status is the command's. */
static bool shellRunBody(VM *vm, ShellCommand *cmd) {
    VM *body_vm = (VM *)malloc(sizeof(VM));
    if (!body_vm) {
        runtimeError(vm, "subshell: out of memory");
        shellUpdateStatus(1);
        return true;
    }
    initVM(body_vm);
    body_vm->frontendContext = vm ? vm->frontendContext : NULL;
#if defined(PSCAL_TARGET_IOS)
    ShellBodyState *state = cmd->body_state;
    cmd->body_state = NULL;
    ShellBodyScope scope;
    shellBodyScopeEnter(state, &scope);
    if (state) {
        body_vm->frontendContext = state->ctx;
    }
#endif
    size_t redirect_scope_depth = shellRuntimeRedirectScopeDepth();
    InterpretResult result = interpretBytecode(body_vm, &cmd->body->chunk,
                                               globalSymbols, constGlobalSymbols, procedure_table, 0);
    shellRuntimeFinishChunk(body_vm);
    shellRuntimeUnwindRedirectScopes(redirect_scope_depth);
    /* Whatever ended the body ends with it: it may run on the shell's own thread. */
    gShellReturnPending = false;
    gShellDiscardPending = false;
    bool exit_requested = shellRuntimeConsumeExitRequested();
    int status = (result == INTERPRET_OK || exit_requested) ? shellRuntimeLastStatus() : 1;
    shellRuntimeRunExitTrap();
    if (shellRuntimeConsumeExitRequested()) {
        status = shellRuntimeLastStatus();
    }
    bool left_stages = shellRuntimeChunkLeftStages();
    if (!left_stages) {
        freeVM(body_vm);
        free(body_vm);
    }
#if defined(PSCAL_TARGET_IOS)
    if (left_stages && state) {
        state->ctx = NULL;
    }
    shellBodyScopeLeave(state, &scope);
#endif
    shellUpdateStatus(status);
    return true;
}

static void shellCheckBuiltinOutput(VM *vm, const char *name);

#if defined(PSCAL_TARGET_IOS)
static bool shellInvokeBuiltin(VM *vm, ShellCommand *cmd);

/* A function, eval or source as a stage runs apart too, as a process would under bash. The
 * stage's thread shares the shell's context, so the commands inside would count as stages
 * of the pipeline it is in: `echo a | f` lost f's output. */
static bool shellStageRunsShellCode(const ShellCommand *cmd) {
    if (!cmd || cmd->body || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    const char *name = cmd->argv[0];
    if (!cmd->ignore_functions && shellFindCompiledFunction(name)) {
        return true;
    }
    return strcmp(name, "eval") == 0 || strcmp(name, "source") == 0 || strcmp(name, ".") == 0;
}

static bool shellCommandIsFrontendToolName(const char *name);

/* A job runs apart from the shell as a forked child does, so what any builtin in it changes stays
 * there: `cd d &`, `read v <<< x &`, `alias a=b &`. printf (-v), pwd and kill (%n) are applets too,
 * but run as builtins here. What is only a program keeps to what it was handed. */
static bool shellJobNeedsOwnState(const ShellCommand *cmd) {
    if (shellStageRunsShellCode(cmd)) {
        return true;
    }
    if (!cmd || cmd->body || cmd->argc == 0 || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    char lookup_buf[PATH_MAX];
    const char *name = shellCommandLookupName(cmd->argv[0], lookup_buf, sizeof(lookup_buf));
    if (!shellIsRuntimeBuiltin(name) || shellCommandIsFrontendToolName(name) || shellIOSFrontendHandler(name)) {
        return false;
    }
    return !smallclueFindApplet(name) || strcmp(name, "printf") == 0 || strcmp(name, "pwd") == 0 ||
           strcmp(name, "kill") == 0;
}

/* So does a stage: `cd d | cat`, `echo x | read v`. Only `jobs | wc -l` lists the shell's, as
 * under bash; a stage's `kill %1` finds no such job there either. */
static bool shellStageNeedsOwnState(const ShellCommand *cmd) {
    return shellJobNeedsOwnState(cmd) && (shellStageRunsShellCode(cmd) || strcmp(cmd->argv[0], "jobs") != 0);
}

static bool shellRunShellCodeStage(VM *vm, ShellCommand *cmd) {
    VM *stage_vm = (VM *)malloc(sizeof(VM));
    if (!stage_vm) {
        runtimeError(vm, "subshell: out of memory");
        shellUpdateStatus(1);
        return true;
    }
    initVM(stage_vm);
    ShellBodyState *state = cmd->body_state;
    cmd->body_state = NULL;
    stage_vm->frontendContext = state->ctx;
    ShellBodyScope scope;
    /* `true | break` in a loop has only itself to leave, and says nothing, as under bash. */
    bool leaves_itself = state->in_loop && !shellStageRunsShellCode(cmd) &&
                         (strcmp(cmd->argv[0], "break") == 0 || strcmp(cmd->argv[0], "continue") == 0);
    shellBodyScopeEnter(state, &scope);
    bool ran = true;
    if (leaves_itself) {
        shellUpdateStatus(0);
    } else {
        ran = shellInvokeBuiltin(stage_vm, cmd);
    }
    (void)shellRuntimeConsumeExitRequested();
    int status = shellRuntimeLastStatus();
    shellRuntimeRunExitTrap();
    if (shellRuntimeConsumeExitRequested()) {
        status = shellRuntimeLastStatus();
    }
    freeVM(stage_vm);
    free(stage_vm);
    shellBodyScopeLeave(state, &scope);
    shellUpdateStatus(status);
    return ran;
}
#endif

static bool shellInvokeBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv[0] || *cmd->argv[0] == '\0') {
        return false;
    }
    if (cmd->body) {
        return shellRunBody(vm, cmd);
    }
#if defined(PSCAL_TARGET_IOS)
    if (cmd->body_state) {
        return shellRunShellCodeStage(vm, cmd);
    }
#endif
    const char *name_raw = cmd->argv[0];
    if (!cmd->ignore_functions && shellFindCompiledFunction(name_raw)) {
        ShellExecRedirBackup *redir_backups = NULL;
        size_t redir_backup_count = 0;
        shellFlushStandardStreams();
        if (!shellApplyExecRedirections(cmd, &redir_backups, &redir_backup_count)) {
            return true;
        }
        bool ran = shellInvokeFunction(vm, cmd);
        shellFlushStandardStreams();
        shellRestoreExecRedirections(redir_backups, redir_backup_count);
        shellFreeExecRedirBackups(redir_backups, redir_backup_count);
        return ran;
    }
    char lookup_buf[PATH_MAX];
    const char *name = shellCommandLookupName(name_raw, lookup_buf, sizeof(lookup_buf));
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *lookup = canonical ? canonical : name;
    if (shellRuntimeBuiltinDisabled(lookup)) {
        return false;
    }
    bool is_runtime_builtin = shellIsRuntimeBuiltin(lookup);
    if (!is_runtime_builtin) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    /* The VM's math and file routines of these names take values, not a command
     * line (`ln -s a b` called ln(x), `halt -w` called Halt("-w")). As commands they
     * mean the applets. poweroff and reboot get here as halt, their canonical name. */
    if ((strcasecmp(lookup, "ln") == 0 || strcasecmp(lookup, "mkdir") == 0 ||
         strcasecmp(lookup, "rmdir") == 0 || strcasecmp(lookup, "reset") == 0 ||
         strcasecmp(lookup, "halt") == 0) &&
        smallclueFindApplet(lookup) != NULL) {
        return false;
    }
#endif
    VmBuiltinFn handler = getVmBuiltinHandler(lookup);
    if (!handler && canonical && canonical != name) {
        handler = getVmBuiltinHandler(name);
    }
#if defined(PSCAL_TARGET_IOS)
    if (!handler && canonical) {
        if (strcasecmp(canonical, "clike") == 0) {
            handler = vmBuiltinShellClike;
        } else if (strcasecmp(canonical, "pascal") == 0) {
            handler = vmBuiltinShellPascal;
        } else if (strcasecmp(canonical, "rea") == 0) {
            handler = vmBuiltinShellRea;
        } else if (strcasecmp(canonical, "aether") == 0) {
            handler = vmBuiltinShellAether;
        } else if (strcasecmp(canonical, "pscalvm") == 0) {
            handler = vmBuiltinShellPscalVm;
        } else if (strcasecmp(canonical, "pscaljson2bc") == 0) {
            handler = vmBuiltinShellPscalJson2bc;
        } else if (strcasecmp(canonical, "exsh") == 0 || strcasecmp(canonical, "sh") == 0) {
            handler = vmBuiltinShellExshTool;
        }
    }
#endif
    if (!handler && is_runtime_builtin) {
        /* Fallback to canonical lookup via builtin table if registry failed. */
        const char *fallback = shellBuiltinCanonicalName(name);
        if (fallback && fallback != canonical) {
            handler = getVmBuiltinHandler(fallback);
            canonical = fallback;
            lookup = canonical ? canonical : name;
        }
    }
#if defined(PSCAL_TARGET_IOS)
    if (!handler && is_runtime_builtin && shellDispatchIOSFrontendBuiltin(vm, cmd)) {
        return true;
    }
#endif
    if (!handler) {
        if (!is_runtime_builtin) {
            return false;
        }
        fprintf(stderr, "[builtin-missing] name=%s canonical=%s\n",
                name,
                canonical ? canonical : "<null>");
        const char *label = canonical ? canonical : name;
        if (vm) {
            runtimeError(vm, "shell builtin '%s': not available", label ? label : "<builtin>");
        } else {
            fprintf(stderr, "exsh: shell builtin '%s' is not available\n", label ? label : "<builtin>");
        }
        shellUpdateStatus(127);
        return true;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    bool include_assignments_as_args = false;
    if (canonical) {
        if (strcmp(canonical, "local") == 0 ||
            strcmp(canonical, "declare") == 0 ||
            strcmp(canonical, "typeset") == 0) {
            include_assignments_as_args = true;
        }
    }
    int extra_assign_args = (include_assignments_as_args && cmd->assignment_count > 0)
                                ? (int)cmd->assignment_count
                                : 0;
    int total_args = arg_count + extra_assign_args;
    Value *args = NULL;
    int previous_line = gShellCurrentCommandLine;
    int previous_column = gShellCurrentCommandColumn;
    shellRuntimeSetCurrentCommandLocation(cmd->line, cmd->column);
    ShellExecRedirBackup *redir_backups = NULL;
    size_t redir_backup_count = 0;
    shellFlushStandardStreams();
    if (!shellApplyExecRedirections(cmd, &redir_backups, &redir_backup_count)) {
        if (args) {
            for (int i = 0; i < arg_count; ++i) {
                freeValue(&args[i]);
            }
            free(args);
        }
        shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
        return true;
    }

    const char *previous_name = shellRuntimeCurrentCommandName();
    shellRuntimeSetCurrentCommandName(name);
    bool previous_background = gShellCurrentBuiltinBackground;
    gShellCurrentBuiltinBackground = cmd->background;
    bool profile_enabled = vmOpcodeProfileIsEnabled();
    const char *profile_name = canonical ? canonical : name;
    if (profile_enabled) {
        vmProfileShellBuiltin(profile_name);
    }

    bool handled = false;
    if (canonical && strcmp(canonical, "test") == 0) {
        const char **expr_args = (const char **)(cmd->argv + 1);
        int expr_argc = (int)cmd->argc - 1;
        bool is_bracket = (name && strcmp(name, "[") == 0);
        bool fast_result = false;
        if (shellBuiltinTestFastEval(expr_args, expr_argc, is_bracket, &fast_result)) {
            shellUpdateStatus(fast_result ? 0 : 1);
            handled = true;
        }
    }
    if (!handled && canonical && strcmp(canonical, ":") == 0) {
        shellUpdateStatus(cmd->empty_stage_status);
        handled = true;
    }
    if (!handled && canonical && strcmp(canonical, "true") == 0) {
        shellUpdateStatus(0);
        handled = true;
    }
    if (!handled && canonical && strcmp(canonical, "false") == 0) {
        shellUpdateStatus(1);
        handled = true;
    }
    if (!handled && canonical && strcmp(canonical, "__shell_arithmetic") == 0) {
        const char *expression = (cmd->argc > 1 && cmd->argv[1]) ? cmd->argv[1] : NULL;
        int status = 0;
        if (expression && shellBuiltinArithmeticFastEval(expression, &status)) {
            shellUpdateStatus(status);
            handled = true;
        }
    }

    if (!handled) {
        if (total_args > 0) {
            args = calloc((size_t)total_args, sizeof(Value));
            if (!args) {
                runtimeError(vm, "shell builtin '%s': out of memory", name);
                shellUpdateStatus(1);
                gShellCurrentBuiltinBackground = previous_background;
                shellRuntimeSetCurrentCommandName(previous_name);
                shellFlushStandardStreams();
                shellRestoreExecRedirections(redir_backups, redir_backup_count);
                shellFreeExecRedirBackups(redir_backups, redir_backup_count);
                shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
                return true;
            }
            int arg_index = 0;
            for (; arg_index < arg_count; ++arg_index) {
                args[arg_index] = makeString(cmd->argv[arg_index + 1]);
            }
            if (extra_assign_args > 0) {
                for (size_t j = 0; j < cmd->assignment_count; ++j, ++arg_index) {
                    const char *text = cmd->assignments[j].text ? cmd->assignments[j].text : "";
                    args[arg_index] = makeString(text);
                }
            }
        }
        handler(vm, total_args, args);
        shellCheckBuiltinOutput(vm, name);
    }
    gShellCurrentBuiltinBackground = previous_background;
    shellRuntimeSetCurrentCommandName(previous_name);
    shellFlushStandardStreams();
    shellRestoreExecRedirections(redir_backups, redir_backup_count);
    shellFreeExecRedirBackups(redir_backups, redir_backup_count);
    if (args) {
        for (int i = 0; i < total_args; ++i) {
            freeValue(&args[i]);
        }
        free(args);
    }
    shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
    return true;
}

static int shellStatusFromWait(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return status;
}

static void shellFreeJob(ShellJob *job) {
    if (!job) {
        return;
    }
    if (job->pids && job->pid_count > 0) {
        for (size_t i = 0; i < job->pid_count; ++i) {
            if (job->pids[i] > 0) {
                vprocSetJobId(job->pids[i], 0);
                vprocSetCommandLabel(job->pids[i], NULL);
            }
        }
    }
    if (job->pgid > 0) {
        vprocSetJobId(job->pgid, 0);
        vprocSetCommandLabel(job->pgid, NULL);
    }
    free(job->command);
    job->command = NULL;
    free(job->pids);
    job->pids = NULL;
    job->pid_count = 0;
    job->pgid = -1;
    job->running = false;
    job->stopped = false;
    job->disowned = false;
    job->inherited = false;
    job->last_status = 0;
}

static void shellRemoveJobAt(size_t index) {
    if (index >= gShellJobCount) {
        return;
    }
    ShellJob *job = &gShellJobs[index];
    shellFreeJob(job);
    if (index + 1 < gShellJobCount) {
        memmove(&gShellJobs[index],
                &gShellJobs[index + 1],
                (gShellJobCount - index - 1) * sizeof(ShellJob));
    }
    gShellJobCount--;
    if (gShellJobCount == 0 && gShellJobs) {
        free(gShellJobs);
        gShellJobs = NULL;
    }
}

/* Call before the job leaves the table. As in bash the status stays known by the job's last pid
 * however often it is asked for. A disowned job gives 0: bash forgets it when told to, before it
 * has a status. */
static ShellFinishedJob *shellRememberFinishedJob(const ShellJob *job, bool reported) {
    ShellRuntimeState *ctx = shellGetContext();
    if (!ctx) {
        return NULL;
    }
    if (ctx->finished_job_count == SHELL_FINISHED_JOB_MAX) {
        ctx->finished_job_count--;
        memmove(&ctx->finished_jobs[0], &ctx->finished_jobs[1],
                ctx->finished_job_count * sizeof(ctx->finished_jobs[0]));
    }
    ShellFinishedJob *entry = &ctx->finished_jobs[ctx->finished_job_count++];
    entry->pgid = job->pgid;
    entry->last_pid = job->last_pid;
    entry->id = job->id;
    entry->status = job->disowned ? 0 : job->last_status;
    entry->has_id = !job->disowned;
    entry->reported = reported || job->disowned;
    entry->killed_now = false;
    return entry;
}

static ShellFinishedJob *shellFindFinishedJobByPid(pid_t pid) {
    ShellRuntimeState *ctx = shellGetContext();
    for (size_t i = ctx ? ctx->finished_job_count : 0; i > 0; --i) {
        ShellFinishedJob *entry = &ctx->finished_jobs[i - 1];
        if (entry->last_pid == pid || entry->pgid == pid) {
            return entry;
        }
    }
    return NULL;
}

static ShellFinishedJob *shellFindFinishedJobById(size_t id) {
    ShellRuntimeState *ctx = shellGetContext();
    for (size_t i = ctx ? ctx->finished_job_count : 0; i > 0; --i) {
        ShellFinishedJob *entry = &ctx->finished_jobs[i - 1];
        if (entry->has_id && entry->id == id) {
            return entry;
        }
    }
    return NULL;
}

/* A job is starting. What has been reported gives up its number. A pid the system has used again
 * means the new job from now on. */
static void shellFinishedJobsBeforeNewJob(const pid_t *pids, size_t pid_count) {
    ShellRuntimeState *ctx = shellGetContext();
    if (!ctx) {
        return;
    }
    size_t kept = 0;
    for (size_t i = 0; i < ctx->finished_job_count; ++i) {
        ShellFinishedJob entry = ctx->finished_jobs[i];
        /* Not by group: on iOS a subshell's jobs are all in the subshell's. */
        bool reused = false;
        for (size_t j = 0; j < pid_count && !reused; ++j) {
            reused = entry.last_pid == pids[j];
        }
        if (reused) {
            continue;
        }
        if (entry.reported) {
            entry.has_id = false;
        }
        ctx->finished_jobs[kept++] = entry;
    }
    ctx->finished_job_count = kept;
}

/* `jobs` has reported them, or a prompt has. Their pids still work, %n no longer does, and wait -n
 * passes them by. */
static void shellFinishedJobsReported(void) {
    ShellRuntimeState *ctx = shellGetContext();
    for (size_t i = 0; ctx && i < ctx->finished_job_count; ++i) {
        ctx->finished_jobs[i].reported = true;
        ctx->finished_jobs[i].has_id = false;
    }
}

#if defined(PSCAL_TARGET_IOS)
static ShellJob *shellFindJobByPid(pid_t pid);
#endif

/* A subshell numbers the jobs it started, and so do a substitution and a shell started as a
 * command. The session's others are not its to wait for, and a stamp on its own would have the
 * shell above mirror them. */
static bool shellNumbersItsOwnJobs(void) {
    return gShellRuntime.in_subshell || gShellCommandSubFrame || gShellRuntime.started_as_command;
}

static size_t shellAllocateJobId(void) {
    size_t candidate = 1;
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int current_sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : 0;
    size_t snap_cap = shellNumbersItsOwnJobs() ? 0 : vprocSnapshot(NULL, 0);
    if (snap_cap > 0) {
        VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
        if (snaps) {
            size_t snap_count = vprocSnapshot(snaps, snap_cap);
            bool used = true;
            while (used) {
                used = false;
                if (shellFindFinishedJobById(candidate)) {
                    used = true;
                    candidate++;
                    continue;
                }
                for (size_t i = 0; i < snap_count; ++i) {
                    const VProcSnapshot *s = &snaps[i];
                    if (!s || s->pid <= 0 || s->job_id <= 0) {
                        continue;
                    }
                    if (current_sid > 0 && s->sid > 0 && s->sid != current_sid) {
                        continue;
                    }
                    if ((size_t)s->job_id == candidate) {
                        used = true;
                        candidate++;
                        break;
                    }
                }
            }
            free(snaps);
            gShellRuntimeDefault.job_next_id = candidate + 1;
            if (SC()) {
                SC()->job_next_id = gShellRuntimeDefault.job_next_id;
            }
            return candidate;
        }
    }
#endif
    bool used = true;
    while (used) {
        used = false;
        if (shellFindFinishedJobById(candidate)) {
            used = true;
            candidate++;
            continue;
        }
        for (size_t i = 0; i < gShellJobCount; ++i) {
            if (gShellJobs[i].id == candidate) {
                used = true;
                candidate++;
                break;
            }
        }
    }
    gShellRuntimeDefault.job_next_id = candidate + 1;
    if (SC()) {
        SC()->job_next_id = gShellRuntimeDefault.job_next_id;
    }
    return candidate;
}

static ShellJob *shellRegisterJob(pid_t pgid, const pid_t *pids, size_t pid_count, const ShellCommand *cmd) {
    if (pgid <= 0 || !pids || pid_count == 0) {
        return NULL;
    }
    pid_t *pid_copy = malloc(sizeof(pid_t) * pid_count);
    if (!pid_copy) {
        return NULL;
    }
    memcpy(pid_copy, pids, sizeof(pid_t) * pid_count);
    shellFinishedJobsBeforeNewJob(pids, pid_count);

    char label_buf[128] = {0};
    char *summary = NULL;
    if (cmd && cmd->argc > 0 && cmd->argv) {
        size_t len = 0;
        for (size_t i = 0; i < cmd->argc; ++i) {
            len += strlen(cmd->argv[i]) + 1;
        }
        summary = malloc(len + 1);
        if (summary) {
            summary[0] = '\0';
            for (size_t i = 0; i < cmd->argc; ++i) {
                strcat(summary, cmd->argv[i]);
                if (i + 1 < cmd->argc) {
                    strcat(summary, " ");
                }
            }
        }
    }

    if (!summary) {
        /* Try to reuse any stored label from the vproc table. */
        if (!vprocGetCommandLabel(pgid, label_buf, sizeof(label_buf)) && pid_count > 0) {
            (void)vprocGetCommandLabel(pids[0], label_buf, sizeof(label_buf));
        }
        if (label_buf[0]) {
            summary = strdup(label_buf);
        } else {
            summary = strdup("unknown");
        }
    }
    if (summary && label_buf[0] == '\0') {
        strncpy(label_buf, summary, sizeof(label_buf) - 1);
        label_buf[sizeof(label_buf) - 1] = '\0';
    }

    ShellJob *new_jobs = realloc(gShellJobs, sizeof(ShellJob) * (gShellJobCount + 1));
    if (!new_jobs) {
        free(summary);
        free(pid_copy);
        return NULL;
    }

    gShellJobs = new_jobs;
    ShellJob *job = &gShellJobs[gShellJobCount++];
    /* realloc leaves what an old job left, and choosing a number looks at every slot. */
    memset(job, 0, sizeof(*job));
    /* Preserve any existing vproc job id; otherwise allocate a new one and stamp it. A shell with
     * numbers of its own does neither: its jobs share its group, whose row holds the number its
     * shell gave it. */
    bool own_numbers = shellNumbersItsOwnJobs();
    int job_id = own_numbers ? 0 : vprocGetJobId(pgid);
    if (!own_numbers && job_id <= 0 && pid_count > 0) {
        job_id = vprocGetJobId(pids[0]);
    }
    if (job_id > 0) {
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *existing = &gShellJobs[i];
            if (existing->id != (size_t)job_id) {
                continue;
            }
            bool same_job = (existing->pgid == pgid);
            if (!same_job && existing->pids && existing->pid_count > 0) {
                for (size_t j = 0; j < existing->pid_count; ++j) {
                    if (existing->pids[j] <= 0) {
                        continue;
                    }
                    for (size_t k = 0; k < pid_count; ++k) {
                        if (existing->pids[j] == pids[k]) {
                            same_job = true;
                            break;
                        }
                    }
                    if (same_job) {
                        break;
                    }
                }
            }
            if (!same_job) {
                job_id = 0;
            }
            break;
        }
    }
    if (job_id <= 0) {
        job_id = (int)shellAllocateJobId();
        if (!own_numbers) {
            vprocSetJobId(pgid, job_id);
            for (size_t i = 0; i < pid_count; ++i) {
                vprocSetJobId(pids[i], job_id);
            }
        }
    } else {
        size_t next_id = (size_t)job_id + 1;
        if (gShellRuntimeDefault.job_next_id < next_id) {
            gShellRuntimeDefault.job_next_id = next_id;
        }
        if (SC() && SC()->job_next_id < next_id) {
            SC()->job_next_id = next_id;
        }
    }
    job->id = (size_t)job_id;
    job->pgid = pgid;
    job->pids = pid_copy;
    job->pid_count = pid_count;
    job->last_pid = pids[pid_count - 1];
    for (size_t i = 0; i < pid_count; ++i) {
        vprocSetCommandLabel(pids[i], summary);
        if (pgid > 0) {
            vprocSetPgid(pids[i], (int)pgid);
            int shell_pid = vprocGetShellSelfPid();
            if (shell_pid > 0 && pids[i] != shell_pid) {
                int sid = vprocGetSid((int)pids[i]);
                /* Never rewrite the session leader's parent (kernel/sid). */
                if (sid > 0 && sid == (int)pids[i]) {
                    continue;
                }
                /* The vproc that starts a job is the one whose `wait` must find it. A task
                 * mirrored from the vproc table (no command) keeps its parent. */
                VProc *waiter = cmd ? vprocCurrent() : NULL;
                int waiter_pid = waiter ? vprocPid(waiter) : -1;
                if (waiter_pid > 0 && waiter_pid != pids[i]) {
                    vprocSetParent(pids[i], waiter_pid);
                } else if (!cmd) {
                    vprocSetParent(pids[i], shell_pid);
                }
            }
        }
    }
    vprocSetCommandLabel(pgid, summary);
    job->running = true;
    job->stopped = false;
    job->disowned = false;
    job->inherited = false;
    job->last_status = 0;
    job->command = summary;
    /* One mirrored from the vproc table may be anyone's. */
    if (cmd) {
        shellCommandSubRecordJob(pids, pid_count);
    }
    return job;
}

static void shellJobMarkRunning(ShellJob *job) {
    if (!job) {
        return;
    }
    job->running = true;
    job->stopped = false;
}

static void shellJobMarkStopped(ShellJob *job, int status) {
    if (!job) {
        return;
    }
    job->running = false;
    job->stopped = true;
    job->last_status = status;
}

static bool shellJobSendSignal(const ShellJob *job, int signo, bool prefer_group) {
    if (!job || signo <= 0) {
        return false;
    }
    bool delivered = false;
    if (prefer_group && job->pgid > 0) {
        if (shellJobControlSendPgid(job->pgid, signo) == 0) {
            delivered = true;
        }
    }
    if (!prefer_group || !delivered) {
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid <= 0) {
                continue;
            }
            if (shellJobControlSendPid(pid, signo) == 0) {
                delivered = true;
            }
        }
    }
    return delivered;
}

static bool shellJobContinue(ShellJob *job, bool prefer_group) {
    if (!job) {
        return false;
    }
    bool ok = shellJobSendSignal(job, SIGCONT, prefer_group);
    if (ok) {
        shellJobMarkRunning(job);
    }
    return ok;
}

static int shellCollectJobs(void) {
    bool prompt_dbg = false;
    shellCommandSubReapOrphans();
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = 0;
    int current_sid = 0;
    /* Mirror synthetic vproc tasks into the job table if they were launched
     * without registering a ShellJob.
     *
     * Important: do NOT treat every synthetic vproc task as a "job". Jobs are
     * shell-managed background pipelines. We only mirror entries that already
     * carry a non-zero vproc job id, otherwise core session entities (kernel,
     * shell, pumps, etc.) pollute the job table and break job-spec semantics. */
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    prompt_dbg = getenv("PSCALI_PROMPT_DEBUG") != NULL;
    if (prompt_dbg) {
        fprintf(stderr, "[jobs] collect begin count=%zu\n", gShellJobCount);
    }
    shell_pid = vprocGetShellSelfPid();
    current_sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : 0;
    /* A subshell's jobs are the ones it started, not its shell's that run beside it. So are a
     * substitution's, and those of a shell started as a command, which would also collect the
     * caller's. */
    size_t snap_cap = shellNumbersItsOwnJobs() ? 0 : vprocSnapshot(NULL, 0);
    if (snap_cap > 0) {
            VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
            if (snaps) {
                size_t snap_count = vprocSnapshot(snaps, snap_cap);
                if (prompt_dbg) {
                    fprintf(stderr, "[jobs] snapshot count=%zu\n", snap_count);
                }
                if (dbg) {
                    fprintf(stderr, "[jobs-ios] snapshot count=%zu\n", snap_count);
                }
    for (size_t i = 0; i < snap_count; ++i) {
        const VProcSnapshot *s = &snaps[i];
        if (!s || s->pid <= 0 || s->exited) {
            continue;
        }
        /* When multiple shells/windows are active in the same process, vproc
         * snapshots can include tasks from other sessions. Only mirror tasks
         * that belong to the current shell session. */
        if (current_sid > 0 && s->sid > 0 && s->sid != current_sid) {
            continue;
        }
        bool has_jobid = (s->job_id > 0);
        if (!has_jobid) {
            continue;
        }
        /* Ignore snapshots that have no command label; these are typically
         * helper threads (output pumps, etc.) that should not appear as
         * jobs. */
        if (s->command[0] == '\0') {
            continue;
        }
        /* Skip snapshots whose label is the shell (self) to avoid mirroring the
         * foreground shell as a background job after reaping. */
        if (strcmp(s->command, "shell") == 0) {
            continue;
        }
        if (shell_pid > 0 && s->pid == shell_pid) {
            continue;
        }
        if (s->pid == vprocGetShellSelfPid() ||
            pthread_equal(s->tid, pthread_self())) {
            continue;
        }
                    if (dbg) {
                        fprintf(stderr, "[jobs-ios] snapshot pid=%d tid=%p exited=%d stopped=%d status=%d label='%s'\n",
                                (int)s->pid,
                                (void *)s->tid,
                                (int)s->exited,
                                (int)s->stopped,
                                s->status,
                                s->command);
                    }
                    if (shellFindJobByPid((pid_t)s->pid)) {
                        continue;
                    }
                    pid_t pid_copy[1] = { (pid_t)s->pid };
                    ShellJob *job = shellRegisterJob((pid_t)s->pid, pid_copy, 1, NULL);
                    if (job) {
                        job->running = !s->exited && !s->stopped;
                        job->stopped = s->stopped;
                        job->last_status = s->status;
                        if (!job->command && s->command[0]) {
                            job->command = strdup(s->command);
                        }
                        /* If the snapshot carries a job id, keep it stable so %n works. */
                        if (s->job_id > 0) {
                            job->id = (size_t)s->job_id;
                            vprocSetJobId(s->pid, s->job_id);
                        }
                    }
                }
                free(snaps);
            }
        }
#endif
    int reaped = 0;
    for (size_t i = 0; i < gShellJobCount;) {
        ShellJob *job = &gShellJobs[i];
        bool job_active = false;
        if (job->inherited) {
            ++i;
            continue;
        }
        if (prompt_dbg) {
            fprintf(stderr,
                    "[jobs] scan job[%zu] id=%zu pgid=%d stopped=%d running=%d pid_count=%zu\n",
                    i,
                    job->id,
                    (int)job->pgid,
                    (int)job->stopped,
                    (int)job->running,
                    job->pid_count);
        }

#if defined(PSCAL_TARGET_IOS)
        if (current_sid > 0 && job && job->pgid > 0) {
            int job_sid = vprocGetSid((int)job->pgid);
            if (job_sid > 0 && job_sid != current_sid) {
                shellRemoveJobAt(i);
                reaped++;
                continue;
            }
        }
        int stored_id = vprocGetJobId(job->pgid);
        if (stored_id <= 0 && job->pid_count > 0) {
            stored_id = vprocGetJobId(job->pids[0]);
        }
        if (stored_id > 0 && !shellNumbersItsOwnJobs()) {
            job->id = (size_t)stored_id;
        }
#endif

        if (!job->pids || job->pid_count == 0) {
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        for (size_t j = 0; j < job->pid_count; ++j) {
            pid_t pid = job->pids[j];
            if (pid <= 0) {
                continue;
            }
            if (prompt_dbg) {
                fprintf(stderr, "[jobs] waitpid pid=%d opts=WNOHANG|WUNTRACED|WCONTINUED\n", (int)pid);
            }
            int status = 0;
            pid_t res = vprocWaitPidShim(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (prompt_dbg) {
                fprintf(stderr, "[jobs] waitpid pid=%d -> res=%d errno=%d status=%d\n",
                        (int)pid, (int)res, errno, status);
            }
            if (res == 0) {
                job_active = true;
                continue;
            }
            if (res < 0) {
                if (errno == EINTR) {
                    job_active = true;
                    continue;
                }
                if (errno == ECHILD) {
                    /* Synthetic tasks may not be parented to the shell; treat as still active. */
                    job_active = true;
                    continue;
                }
                job->pids[j] = -1;
                continue;
            }
            if (WIFSTOPPED(status)) {
                job->stopped = true;
                job->running = false;
                job_active = true;
            } else if (WIFCONTINUED(status)) {
                job->stopped = false;
                job->running = true;
                job_active = true;
            } else {
                /* A job gives what its last command gave, whichever ended last. */
                if (j + 1 == job->pid_count) {
                    job->last_status = shellStatusFromWait(status);
                }
                job->pids[j] = -1;
            }
        }

        if (!job->stopped) {
            for (size_t j = 0; j < job->pid_count; ++j) {
                if (job->pids[j] > 0) {
                    job_active = true;
                    job->running = true;
                    break;
                }
            }
        }

        bool all_done = true;
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] > 0) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            /* Its status is for `wait`. A job finishing is no command the script ran, so
             * it leaves $? and set -e alone. */
            shellRememberFinishedJob(job, false);
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        if (!job_active && !job->stopped) {
            job->running = true;
        }

        ++i;
    }
    if (gShellJobCount == 0) {
        shellResetJobIds();
    }
    if (prompt_dbg) {
        fprintf(stderr, "[jobs] collect end reaped=%d count=%zu\n", reaped, gShellJobCount);
    }
#if defined(PSCAL_TARGET_IOS)
    if (dbg) {
        fprintf(stderr, "[jobs-ios] table after collect count=%zu\n", gShellJobCount);
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *job = &gShellJobs[i];
            fprintf(stderr, "[jobs-ios]   job id=%zu pgid=%d running=%d stopped=%d pid_count=%zu command='%s'\n",
                    job->id,
                    (int)job->pgid,
                    (int)job->running,
                    (int)job->stopped,
                    job->pid_count,
                    job->command ? job->command : "");
            for (size_t j = 0; j < job->pid_count; ++j) {
                fprintf(stderr, "[jobs-ios]     pid[%zu]=%d\n", j, (int)job->pids[j]);
            }
        }
    }
#endif
    return reaped;
}

/* A prompt is about to print, and bash reports there what has ended. What `kill` just ended it has
 * not heard of yet, so that waits for the prompt after. */
void shellRuntimeJobsBeforePrompt(void) {
    shellCollectJobs();
    ShellRuntimeState *ctx = shellGetContext();
    for (size_t i = 0; ctx && i < ctx->finished_job_count; ++i) {
        ShellFinishedJob *entry = &ctx->finished_jobs[i];
        if (entry->killed_now) {
            entry->killed_now = false;
        } else {
            entry->reported = true;
            entry->has_id = false;
        }
    }
}

static size_t shellJobVisibleCount(void) {
    size_t count = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->disowned) {
            ++count;
        }
    }
    return count;
}

static bool shellJobFindVisibleIndex(size_t visible_number, size_t *out_index) {
    if (!out_index || visible_number == 0) {
        return false;
    }
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    size_t ordinal = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (job->disowned) {
            continue;
        }
        ++ordinal;
        if (dbg) {
            fprintf(stderr, "[jobs-ios] vis=%zu ordinal=%zu id=%zu pgid=%d running=%d stopped=%d\n",
                    visible_number, ordinal, job->id, (int)job->pgid,
                    (int)job->running, (int)job->stopped);
        }
        if (job->id == visible_number || ordinal == visible_number) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

static bool shellFindJobIndexByPid(pid_t pid, size_t *out_index) {
    if (!out_index || pid <= 0) {
        return false;
    }
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->pids || job->pid_count == 0) {
            continue;
        }
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] == pid) {
                *out_index = i;
                return true;
            }
        }
    }
    return false;
}

static bool shellParseJobSpecifier(VM *vm, const char *name, Value spec, size_t *out_index) {
    if (VALUE_TYPE(spec) == TYPE_STRING && AS_STRING(spec)) {
        const char *text = AS_STRING(spec);
        if (text[0] == '%') {
            text++;
        }
        if (*text == '\0') {
            runtimeError(vm, "%s: invalid job spec", name);
            return false;
        }
        char *end = NULL;
        long index = strtol(text, &end, 10);
        if (*end != '\0' || index <= 0) {
            runtimeError(vm, "%s: invalid job '%s'", name, AS_STRING(spec));
            return false;
        }
        size_t pid_index = 0;
        if (shellFindJobIndexByPid((pid_t)index, &pid_index)) {
            *out_index = pid_index;
            return true;
        }
        if (!shellJobFindVisibleIndex((size_t)index, out_index)) {
            runtimeError(vm, "%s: invalid job '%s'", name, AS_STRING(spec));
            return false;
        }
        return true;
    }

    if (IS_INTLIKE(spec)) {
        long index = (long)AS_INTEGER(spec);
        if (index <= 0) {
            runtimeError(vm, "%s: invalid job index", name);
            return false;
        }
        size_t pid_index = 0;
        if (shellFindJobIndexByPid((pid_t)index, &pid_index)) {
            *out_index = pid_index;
            return true;
        }
        if (shellJobFindVisibleIndex((size_t)index, out_index)) {
            return true;
        }
        runtimeError(vm, "%s: invalid job index", name);
        return false;
    }

    runtimeError(vm, "%s: job spec must be a string or integer", name);
    return false;
}

static bool shellResolveJobIndex(VM *vm, const char *name, int arg_count, Value *args, size_t *out_index) {
    size_t visible_count = shellJobVisibleCount();
#if defined(PSCAL_TARGET_IOS)
    if (visible_count == 0) {
        /* If jobs were not registered, try to mirror from vproc snapshots. */
        shellCollectJobs();
        visible_count = shellJobVisibleCount();
    }
#endif
    if (arg_count == 0) {
        if (visible_count == 0) {
            runtimeError(vm, "%s: no current job", name);
            return false;
        }
        for (size_t i = gShellJobCount; i > 0; --i) {
            ShellJob *job = &gShellJobs[i - 1];
            if (!job->disowned) {
                *out_index = i - 1;
                return true;
            }
        }
        runtimeError(vm, "%s: no current job", name);
        return false;
    }
    if (arg_count > 1) {
        runtimeError(vm, "%s: too many arguments", name);
        return false;
    }

    if (shellParseJobSpecifier(vm, name, args[0], out_index)) {
        return true;
    }
#if defined(PSCAL_TARGET_IOS)
    /* One more attempt after refreshing vproc snapshot. */
    shellCollectJobs();
    return shellParseJobSpecifier(vm, name, args[0], out_index);
#else
    return false;
#endif
}

static void shellParseMetadata(const char *meta, ShellCommand *cmd) {
    if (!meta || !cmd) {
        return;
    }
    char *copy = strdup(meta);
    if (!copy) {
        return;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "bg") == 0) {
                shellParseBool(value, &cmd->background);
            } else if (strcmp(key, "pipe") == 0) {
                cmd->pipeline_index = atoi(value);
            } else if (strcmp(key, "head") == 0) {
                shellParseBool(value, &cmd->is_pipeline_head);
            } else if (strcmp(key, "tail") == 0) {
                shellParseBool(value, &cmd->is_pipeline_tail);
            } else if (strcmp(key, "line") == 0) {
                cmd->line = atoi(value);
            } else if (strcmp(key, "col") == 0) {
                cmd->column = atoi(value);
            }
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    free(copy);
}

/* One word's fields: brace expansion, the expansions, splitting and globbing. */
static bool shellExpandWordFields(const char *text,
                                  uint8_t flags,
                                  const char *meta,
                                  size_t meta_len,
                                  bool allow_braces,
                                  ShellStringArray *out) {
    if (allow_braces) {
        ShellStringArray pieces = (ShellStringArray){0};
        if (shellBraceExpandWord(text, &pieces)) {
            bool ok = true;
            for (size_t i = 0; i < pieces.count && ok; ++i) {
                ok = shellExpandWordFields(pieces.items[i], flags, NULL, 0, false, out);
            }
            shellStringArrayFree(&pieces);
            return ok;
        }
    }
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    bool zero_array = false;
    char *expanded = shellExpandWord(text, flags, meta, meta_len, &quoted_map, &quoted_len, &zero_array);
    if (!expanded) {
        return false;
    }
    if (quoted_map && !shellMarkQuotedGlobChars(&expanded, &quoted_map, &quoted_len)) {
        free(expanded);
        free(quoted_map);
        return false;
    }
    char **fields = NULL;
    size_t field_count = 0;
    bool split = shellSplitExpandedWord(expanded, flags, quoted_map, quoted_len, zero_array, &fields, &field_count);
    /* a=("" x): quotes make a field even with nothing in them. */
    if (split && field_count == 0 && !zero_array && !expanded[0] &&
        (strchr(text, SHELL_QUOTE_MARK_SINGLE) || strchr(text, SHELL_QUOTE_MARK_DOUBLE))) {
        fields = (char **)calloc(1, sizeof(char *));
        if (fields) {
            fields[0] = strdup("");
            field_count = 1;
        }
    }
    free(expanded);
    free(quoted_map);
    if (!split) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < field_count; ++i) {
        char *field = fields[i];
        fields[i] = NULL;
        if (!field) {
            continue;
        }
        if (!ok) {
            free(field);
            continue;
        }
        bool appended = false;
        if (!gShellRuntime.noglob_enabled && shellWordShouldGlob(flags, field) && shellFieldIsGlobPattern(field)) {
            char **glob_matches = NULL;
            size_t glob_count = 0;
            char *glob_pattern = shellGlobPatternFromField(field);
            int glob_status = glob_pattern ? shellExpandGlobWithOptions(glob_pattern, &glob_matches, &glob_count)
                                           : GLOB_NOSPACE;
            free(glob_pattern);
            if (glob_status == 0) {
                for (size_t g = 0; g < glob_count; ++g) {
                    char *match = glob_matches[g];
                    glob_matches[g] = NULL;
                    if (!ok || !match || !shellStringArrayAppend(out, match)) {
                        free(match);
                        ok = false;
                    }
                }
                free(glob_matches);
                appended = true;
            } else if (glob_status == GLOB_NOMATCH && shellShoptOptionEnabled("failglob")) {
                shellStripGlobMarks(field);
                shellReportRecoverableError(gShellCurrentVm, true, "no match: %s", field);
                shellExpansionFail();
                appended = true;
            } else if (glob_status == GLOB_NOMATCH && shellShoptOptionEnabled("nullglob")) {
                appended = true;
            } else if (glob_status != GLOB_NOMATCH) {
                fprintf(stderr, "exsh: glob failed for '%s'\n", field);
            }
        }
        if (appended) {
            free(field);
        } else {
            shellStripGlobMarks(field);
            if (!shellStringArrayAppend(out, field)) {
                free(field);
                ok = false;
            }
        }
    }
    free(fields);
    return ok;
}

/* Bash's arrays keep each element's quoting: a=("b c" d) has two. The word is one piece
 * of raw text with its markers, so split it into elements here, expand each as a word of
 * its own, and hand back the literal the array parser reads. */
static char *shellExpandArrayLiteralAssignment(const char *text, uint8_t flags, bool mark_literal) {
    const char *eq = strchr(text, '=');
    const char *open = eq ? strchr(eq, '(') : NULL;
    const char *close = text + strlen(text);
    while (close > text && close[-1] != ')') {
        close--;
    }
    if (!open || close <= open + 1) {
        return NULL;
    }
    close--;
    bool append = eq > text && eq[-1] == '+';
    size_t name_len = (size_t)(eq - text) - (append ? 1 : 0);
    const ShellArrayVariable *existing = shellArrayRegistryLookup(text, name_len);
    bool existing_associative = existing && existing->kind == SHELL_ARRAY_KIND_ASSOCIATIVE;
    uint8_t element_flags = flags & (uint8_t)~SHELL_WORD_FLAG_ASSIGNMENT;
    element_flags &= (uint8_t)~(SHELL_WORD_FLAG_SINGLE_QUOTED | SHELL_WORD_FLAG_DOUBLE_QUOTED);

    ShellStringArray keys = (ShellStringArray){0};
    ShellStringArray values = (ShellStringArray){0};
    bool any_key = false;
    bool ok = true;
    if (append && existing) {
        for (size_t i = 0; i < existing->count && ok; ++i) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%zu", i);
            ok = shellStringArrayAppend(&keys, strdup((existing->keys && existing->keys[i]) ? existing->keys[i] : buffer)) &&
                 shellStringArrayAppend(&values, strdup(existing->values[i] ? existing->values[i] : ""));
        }
        any_key = existing_associative;
    } else if (append) {
        bool is_set = false;
        char *scalar = shellLookupParameterValueInternal(text, name_len, &is_set);
        if (is_set && scalar) {
            ok = shellStringArrayAppend(&keys, strdup("0")) && shellStringArrayAppend(&values, strdup(scalar));
        }
        free(scalar);
    }
    /* Appending goes after the highest index. */
    long long next_index = (long long)values.count;
    if (append && existing && existing->sparse) {
        next_index = 0;
        for (size_t i = 0; i < keys.count; ++i) {
            long long index = strtoll(keys.items[i], NULL, 10);
            if (index >= next_index) {
                next_index = index + 1;
            }
        }
    }

    size_t body_len = (size_t)(close - open - 1);
    const char *body = open + 1;
    size_t pos = 0;
    while (ok && pos < body_len) {
        char c = body[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pos++;
            continue;
        }
        size_t start = pos;
        bool in_single = false;
        bool in_double = false;
        while (pos < body_len) {
            c = body[pos];
            if (!in_single && !in_double && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
                break;
            }
            pos = shellBraceSkip(body, body_len, pos, &in_single, &in_double);
        }
        size_t element_len = pos - start;
        const char *element = body + start;
        /* [key]=value */
        size_t key_end = 0;
        if (element[0] == '[') {
            bool skip_single = false;
            bool skip_double = false;
            size_t k = 1;
            while (k < element_len) {
                if (element[k] == ']' && !skip_single && !skip_double) {
                    if (k + 1 < element_len && element[k + 1] == '=') {
                        key_end = k;
                    }
                    break;
                }
                k = shellBraceSkip(element, element_len, k, &skip_single, &skip_double);
            }
        }
        char *copy = (char *)malloc(element_len + 1);
        if (!copy) {
            ok = false;
            break;
        }
        memcpy(copy, element, element_len);
        copy[element_len] = '\0';
        if (key_end > 0) {
            copy[key_end] = '\0';
            char *key = shellExpandWord(copy + 1, element_flags, NULL, 0, NULL, NULL, NULL);
            char *value = shellExpandWord(copy + key_end + 2, element_flags, NULL, 0, NULL, NULL, NULL);
            if (key && value) {
                shellFlattenArraySeparators(key);
                shellFlattenArraySeparators(value);
                char *end = NULL;
                long long index = strtoll(key, &end, 10);
                if (end && *end == '\0' && end != key && !existing_associative) {
                    next_index = index + 1;
                }
                any_key = true;
                ok = shellStringArrayAppend(&keys, key) && shellStringArrayAppend(&values, value);
            } else {
                free(key);
                free(value);
                ok = false;
            }
        } else {
            ShellStringArray fields = (ShellStringArray){0};
            ok = shellExpandWordFields(copy, element_flags, NULL, 0, true, &fields);
            for (size_t i = 0; i < fields.count && ok; ++i) {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%lld", next_index++);
                ok = shellStringArrayAppend(&keys, strdup(buffer)) && shellStringArrayAppend(&values, fields.items[i]);
                fields.items[i] = NULL;
            }
            shellStringArrayFree(&fields);
        }
        free(copy);
    }

    /* Keys 0, 1, 2... are a plain list. Anything else keeps its keys, which is how a
     * sparse array holds together in a registry that has no gaps. */
    bool dense = !existing_associative;
    for (size_t i = 0; i < keys.count && dense; ++i) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%zu", i);
        dense = strcmp(keys.items[i], buffer) == 0;
    }
    (void)any_key;

    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    if (ok) {
        shellBufferAppendSlice(&out, &length, &capacity, text, name_len);
        shellBufferAppendChar(&out, &length, &capacity, '=');
        if (mark_literal) {
            shellBufferAppendChar(&out, &length, &capacity, SHELL_ARRAY_LITERAL_MARK);
        }
        shellBufferAppendChar(&out, &length, &capacity, '(');
        for (size_t i = 0; i < values.count; ++i) {
            if (i > 0) {
                shellBufferAppendChar(&out, &length, &capacity, ' ');
            }
            if (!dense) {
                shellBufferAppendChar(&out, &length, &capacity, '[');
                shellBufferAppendQuoted(&out, &length, &capacity, keys.items[i]);
                shellBufferAppendString(&out, &length, &capacity, "]=");
            }
            shellBufferAppendQuoted(&out, &length, &capacity, values.items[i]);
        }
        shellBufferAppendChar(&out, &length, &capacity, ')');
    }
    shellStringArrayFree(&keys);
    shellStringArrayFree(&values);
    return out;
}

/* x+=y as the x=... it comes to. The array forms are handled before this. */
static bool shellResolveAppendAssignment(char **assignment) {
    char *text = *assignment;
    char *eq = strchr(text, '=');
    if (!eq || eq == text || eq[-1] != '+') {
        return true;
    }
    size_t full_len = (size_t)(eq - 1 - text);
    size_t name_len = 0;
    size_t consumed = 0;
    const char *subscript = NULL;
    size_t subscript_len = 0;
    if (!shellParamParseName(text, full_len, &name_len, &subscript, &subscript_len, &consumed) ||
        consumed != full_len) {
        return true;
    }
    ShellParamSubject subject;
    if (!shellParamLoadSubject(text, name_len, subscript, subscript_len, &subject)) {
        return false;
    }
    const char *value = eq + 1;
    const char *old_value = (subject.value && subject.is_set) ? subject.value : "";
    char *name = strndup(text, name_len);
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    shellBufferAppendSlice(&out, &length, &capacity, text, full_len);
    shellBufferAppendChar(&out, &length, &capacity, '=');
    if (name && shellVariableHasAttribute(name, SHELL_VAR_ATTR_INTEGER)) {
        /* Evaluated here, so the plain assignment sees a number. */
        size_t n = strlen(old_value) + strlen(value) + 8;
        char *expr = (char *)malloc(n);
        long long total = 0;
        if (expr) {
            snprintf(expr, n, "(%s)+(%s)", *old_value ? old_value : "0", *value ? value : "0");
            if (shellArithmeticEvaluateText(expr, 0, &total, NULL, 0)) {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%lld", total);
                shellBufferAppendString(&out, &length, &capacity, buffer);
            }
            free(expr);
        }
    } else {
        shellBufferAppendString(&out, &length, &capacity, old_value);
        shellBufferAppendString(&out, &length, &capacity, value);
    }
    free(name);
    shellParamSubjectFree(&subject);
    if (!out) {
        return false;
    }
    free(text);
    *assignment = out;
    return true;
}

static bool shellCommandIsDeclaration(const char *name) {
    return strcmp(name, "declare") == 0 || strcmp(name, "typeset") == 0 || strcmp(name, "local") == 0 ||
           strcmp(name, "export") == 0 || strcmp(name, "readonly") == 0;
}

/* What the [[ builtin reads as an operator. One that came out of quotes or an expansion
 * is an operand: `x='!'; [[ $x == '!' ]]`. */
static bool shellCondTextLooksLikeOperator(const char *text) {
    static const char *const kOps[] = {"&&", "||", "(", ")", "!", "==", "=", "!=", "=~", "<", ">"};
    if (text[0] == '-') {
        return true;
    }
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
        if (strcmp(text, kOps[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The text after =~: what came from quotes matches literally. */
static char *shellRegexFromExpanded(const char *expanded, const bool *map, size_t map_len) {
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t n = strlen(expanded);
    for (size_t i = 0; i < n; ++i) {
        if (map && i < map_len && map[i] && strchr(".^$*+?()[]{}|\\", expanded[i])) {
            shellBufferAppendChar(&out, &length, &capacity, '\\');
        }
        shellBufferAppendChar(&out, &length, &capacity, expanded[i]);
    }
    return out ? out : strdup("");
}

static bool shellAddDoubleBracketWord(ShellCommand *cmd, const char *text, uint8_t flags, const char *meta,
                                      size_t meta_len, bool *saw_command_word) {
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    /* Bash expands an operand only when it gets there: [[ -z ${x-} || $x == a ]] under set -u. */
    bool saved_suppressed = gShellNounsetSuppressed;
    gShellNounsetSuppressed = true;
    gShellProcSubDisabled = true;
    char *expanded = shellExpandWord(text, flags, meta, meta_len, &quoted_map, &quoted_len, NULL);
    gShellProcSubDisabled = false;
    gShellNounsetSuppressed = saved_suppressed;
    if (!expanded) {
        return false;
    }
    shellFlattenArraySeparators(expanded);
    if (cmd->argc > 0 && strcmp(cmd->argv[cmd->argc - 1], "=~") == 0) {
        char *regex = shellRegexFromExpanded(expanded, quoted_map, quoted_len);
        free(expanded);
        expanded = regex;
    } else if (quoted_map && cmd->argc > 0 && !shellMarkQuotedGlobChars(&expanded, &quoted_map, &quoted_len)) {
        free(expanded);
        free(quoted_map);
        return false;
    }
    free(quoted_map);
    if (!expanded) {
        return false;
    }
    if (cmd->argc > 0 && strcmp(text, expanded) != 0 && shellCondTextLooksLikeOperator(expanded)) {
        size_t n = strlen(expanded);
        char *protected_text = (char *)malloc(n + 2);
        if (!protected_text) {
            free(expanded);
            return false;
        }
        protected_text[0] = SHELL_GLOB_LITERAL_MARK;
        memcpy(protected_text + 1, expanded, n + 1);
        free(expanded);
        expanded = protected_text;
    }
    if (!shellCommandAppendArgOwned(cmd, expanded)) {
        return false;
    }
    if (saw_command_word) {
        *saw_command_word = true;
    }
    return true;
}

static bool shellAddArg(ShellCommand *cmd, const char *arg, bool *saw_command_word) {
    if (!cmd || !arg) {
        return false;
    }
    const char *text = NULL;
    const char *meta = NULL;
    size_t meta_len = 0;
    uint8_t flags = 0;
    if (!shellDecodeWordSpec(arg, &text, &flags, &meta, &meta_len)) {
        return false;
    }
    bool has_command = saw_command_word ? *saw_command_word : cmd->argc > 0;
    bool assignment_word = (flags & SHELL_WORD_FLAG_ASSIGNMENT) != 0;
    bool at_assignment = assignment_word && !has_command;
    bool declaration = assignment_word && has_command && cmd->argc > 0 && shellCommandIsDeclaration(cmd->argv[0]);

    if ((at_assignment || declaration) && shellAssignmentIsArrayLiteral(text, flags)) {
        char *assignment = shellExpandArrayLiteralAssignment(text, flags, declaration);
        if (!assignment) {
            return false;
        }
        return declaration ? shellCommandAppendArgOwned(cmd, assignment)
                           : shellCommandAppendAssignmentOwned(cmd, assignment, true);
    }

    if ((cmd->argc == 0 && !has_command && strcmp(text, "[[") == 0) ||
        (cmd->argc > 0 && !at_assignment && strcmp(cmd->argv[0], "[[") == 0)) {
        cmd->disable_field_splitting = true;
        return shellAddDoubleBracketWord(cmd, text, flags, meta, meta_len, saw_command_word);
    }

    if (at_assignment || declaration || cmd->disable_field_splitting) {
        char *expanded = shellExpandWord(text, flags, meta, meta_len, NULL, NULL, NULL);
        if (!expanded) {
            return false;
        }
        shellFlattenArraySeparators(expanded);
        if ((at_assignment || declaration) && !shellResolveAppendAssignment(&expanded)) {
            free(expanded);
            return false;
        }
        if (at_assignment && shellLooksLikeAssignment(expanded)) {
            return shellCommandAppendAssignmentOwned(cmd, expanded, false);
        }
        if (declaration || cmd->disable_field_splitting) {
            if (!shellCommandAppendArgOwned(cmd, expanded)) {
                return false;
            }
            if (saw_command_word) {
                *saw_command_word = true;
            }
            return true;
        }
        free(expanded);
    }

    ShellStringArray fields = (ShellStringArray){0};
    bool ok = shellExpandWordFields(text, flags, meta, meta_len, !at_assignment, &fields);
    for (size_t i = 0; i < fields.count && ok; ++i) {
        ok = shellCommandAppendArgOwned(cmd, fields.items[i]);
        fields.items[i] = NULL;
    }
    if (ok && fields.count > 0 && saw_command_word) {
        *saw_command_word = true;
    }
    shellStringArrayFree(&fields);
    return ok;
}

static int decodeHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

static char *decodeHexString(const char *hex, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!hex || *hex == '\0') {
        if (out_len) {
            *out_len = 0;
        }
        return strdup("");
    }
    size_t len = strlen(hex);
    if ((len & 1) != 0) {
        return NULL;
    }
    size_t decoded_len = len / 2;
    char *decoded = (char *)malloc(decoded_len + 1);
    if (!decoded) {
        return NULL;
    }
    for (size_t i = 0; i < decoded_len; ++i) {
        int high = decodeHexDigit(hex[i * 2]);
        int low = decodeHexDigit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            free(decoded);
            return NULL;
        }
        decoded[i] = (char)((high << 4) | low);
    }
    decoded[decoded_len] = '\0';
    if (out_len) {
        *out_len = decoded_len;
    }
    return decoded;
}

static bool shellAppendCombinedRedirect(ShellCommand *cmd, const char *path, bool append);

/* An unquoted expansion must leave exactly one word to open. */
static bool shellRedirectTargetIsAmbiguous(const char *expanded, uint8_t flags, const bool *quoted_map, size_t quoted_len) {
    if (*expanded == '\0') {
        return (flags & (SHELL_WORD_FLAG_SINGLE_QUOTED | SHELL_WORD_FLAG_DOUBLE_QUOTED)) == 0;
    }
    const char *ifs = shellSafeGetenv("IFS");
    if (!ifs) {
        ifs = " \t\n";
    }
    for (size_t i = 0; expanded[i]; ++i) {
        if ((!quoted_map || i >= quoted_len || !quoted_map[i]) && strchr(ifs, expanded[i])) {
            return true;
        }
    }
    return false;
}

static bool shellAddRedirection(ShellCommand *cmd, const char *spec) {
    if (!cmd || !spec) {
        return false;
    }
    if (strncmp(spec, "redir:", 6) != 0) {
        return false;
    }
    const char *payload = spec + 6;
    char *copy = strdup(payload);
    if (!copy) {
        return false;
    }

    const char *fd_text = "";
    const char *type_text = "";
    const char *word_hex = "";
    const char *here_hex = "";
    const char *here_string_hex = "";
    bool here_quoted = false;

    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        const char *key = cursor;
        const char *value = "";
        if (eq) {
            *eq = '\0';
            value = eq + 1;
        }
        if (strcmp(key, "fd") == 0) {
            fd_text = value;
        } else if (strcmp(key, "type") == 0) {
            type_text = value;
        } else if (strcmp(key, "word") == 0) {
            word_hex = value;
        } else if (strcmp(key, "here") == 0) {
            here_hex = value;
        } else if (strcmp(key, "hereq") == 0) {
            shellParseBool(value, &here_quoted);
        } else if (strcmp(key, "hstr") == 0) {
            here_string_hex = value;
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }

    if (!type_text || *type_text == '\0') {
        free(copy);
        return false;
    }

    int fd = -1;
    if (fd_text && fd_text[0] == '{') {
        /* {name}> f: the runtime picks it. */
    } else if (fd_text && *fd_text) {
        fd = atoi(fd_text);
    } else if (strcmp(type_text, "<") == 0 || strcmp(type_text, "<<") == 0 || strcmp(type_text, "<<<") == 0 || strcmp(type_text, "<&") == 0 || strcmp(type_text, "<>") == 0) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }

    ShellRedirection redir;
    memset(&redir, 0, sizeof(redir));
    redir.fd = fd;

    char *word_encoded = decodeHexString(word_hex, NULL);
    const char *target_spec = (word_encoded && *word_encoded) ? word_encoded : "";
    const char *target_text = NULL;
    const char *target_meta = NULL;
    size_t target_meta_len = 0;
    uint8_t target_flags = 0;
    if (target_spec && *target_spec) {
        shellDecodeWordSpec(target_spec, &target_text, &target_flags, &target_meta, &target_meta_len);
    }

    char *expanded_target = NULL;
    bool ambiguous_target = false;
    if (strcmp(type_text, "<<") != 0) {
        if (!target_spec || *target_spec == '\0') {
            free(word_encoded);
            free(copy);
            return false;
        }
        bool *target_quoted_map = NULL;
        size_t target_quoted_len = 0;
        expanded_target = shellExpandWord(target_text,
                                          target_flags,
                                          target_meta,
                                          target_meta_len,
                                          &target_quoted_map,
                                          &target_quoted_len,
                                          NULL);
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        ambiguous_target = strcmp(type_text, "<<<") != 0 &&
                           shellRedirectTargetIsAmbiguous(expanded_target, target_flags,
                                                          target_quoted_map, target_quoted_len);
        free(target_quoted_map);
        shellFlattenArraySeparators(expanded_target);
    }

    if (ambiguous_target) {
        redir.kind = SHELL_RUNTIME_REDIR_DUP;
        redir.ambiguous = true;
        redir.dup_target_fd = -1;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDONLY;
        redir.mode = 0;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (gShellRuntime.noclobber_enabled) {
            /* set -C: a regular file stays as it is, and /dev/null still works. */
            struct stat st;
            if (stat(expanded_target, &st) != 0 || S_ISREG(st.st_mode)) {
                redir.flags = O_WRONLY | O_CREAT | O_EXCL;
            }
        }
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_APPEND;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "&>") == 0 || strcmp(type_text, "&>>") == 0) {
        /* Redirect both stdout and stderr. */
        bool append = (strcmp(type_text, "&>>") == 0);
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;

        /* Prepare a dup redirection for stderr to stdout. */
        ShellRedirection dup_redir;
        memset(&dup_redir, 0, sizeof(dup_redir));
        dup_redir.fd = STDERR_FILENO;
        dup_redir.kind = SHELL_RUNTIME_REDIR_DUP;
        dup_redir.dup_target_fd = STDOUT_FILENO;

        ShellRedirection *new_redirs = realloc(cmd->redirs,
                                               sizeof(ShellRedirection) * (cmd->redir_count + 2));
        if (!new_redirs) {
            free(redir.path);
            free(expanded_target);
            free(word_encoded);
            free(copy);
            return false;
        }
        cmd->redirs = new_redirs;
        cmd->redirs[cmd->redir_count++] = redir;
        cmd->redirs[cmd->redir_count++] = dup_redir;
        free(word_encoded);
        free(copy);
        return true;
    } else if (strcmp(type_text, "<>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDWR | O_CREAT;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">|") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<&") == 0 || strcmp(type_text, ">&") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_DUP;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        if (strcmp(expanded_target, "-") == 0) {
            redir.close_target = true;
        } else {
            char *endptr = NULL;
            errno = 0;
            long value = strtol(expanded_target, &endptr, 10);
            if (errno != 0 || !endptr || *endptr != '\0' || *expanded_target == '\0') {
                if (fd == STDOUT_FILENO && strcmp(type_text, ">&") == 0 && *expanded_target) {
                    /* `>&word` that is not a descriptor means `&>word`. */
                    bool added = shellAppendCombinedRedirect(cmd, expanded_target, false);
                    free(expanded_target);
                    free(word_encoded);
                    free(copy);
                    return added;
                }
                redir.ambiguous = true;
                redir.dup_target_fd = -1;
                redir.path = expanded_target;
                expanded_target = NULL;
            } else {
                redir.dup_target_fd = (int)value;
            }
        }
        free(expanded_target);
        expanded_target = NULL;
    } else if (strcmp(type_text, "<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HEREDOC;
        size_t body_len = 0;
        char *decoded = decodeHexString(here_hex ? here_hex : "", &body_len);
        if (!decoded) {
            free(word_encoded);
            free(copy);
            return false;
        }
        char *expanded = shellExpandHereDocument(decoded, here_quoted);
        if (!expanded) {
            expanded = decoded;
        } else {
            free(decoded);
        }
        redir.here_doc = expanded;
        redir.here_doc_length = expanded ? strlen(expanded) : 0;
        redir.here_doc_quoted = here_quoted;
    } else if (strcmp(type_text, "<<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HERE_STRING;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        char *literal = decodeHexString(here_string_hex ? here_string_hex : "", NULL);
        if (!literal) {
            free(expanded_target);
            free(word_encoded);
            free(copy);
            return false;
        }
        redir.here_string_literal = literal;
        redir.here_string = expanded_target;
        redir.here_string_length = expanded_target ? strlen(expanded_target) : 0;
        expanded_target = NULL;
    } else {
        free(expanded_target);
        free(word_encoded);
        free(copy);
        return false;
    }

    ShellRedirection *new_redirs = realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 1));
    if (!new_redirs) {
        free(expanded_target);
        free(redir.path);
        free(redir.here_doc);
        free(redir.here_string_literal);
        free(redir.here_string);
        free(word_encoded);
        free(copy);
        return false;
    }
    cmd->redirs = new_redirs;
    if (fd_text && fd_text[0] == '{') {
        redir.fd_var = strndup(fd_text + 1, strlen(fd_text) - 2);
    }
    cmd->redirs[cmd->redir_count++] = redir;
    free(word_encoded);
    free(copy);
    return true;
}
