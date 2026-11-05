/* What bash does between the braces of ${...}, the word-splitting helpers around it, tilde
 * and brace expansion. Included from shell_word_expansion.inc. */

/* An expansion error ends a script, as under bash: ${x:?msg}, set -u, a bad substitution. A
 * prompt just drops the command. */
static void shellExpansionFail(void) {
    gShellExpansionFailed = true;
    shellUpdateStatus(1);
    if (gShellExpandingJobWords) {
        return;
    }
    if (!shellRuntimeIsInteractive() || gShellRuntime.in_subshell) {
        shellRuntimeRequestExit();
        if (gShellCurrentVm) {
            gShellCurrentVm->exit_requested = true;
            gShellCurrentVm->current_builtin_name = "expansion";
        }
    }
}

/* Bash takes arithmetic in a word more lightly: all that is running goes, the rest of its line
 * with it, and it reads on from the text it is reading, be that eval's or a sourced file's. A
 * subshell ends. */
static void shellArithmeticExpansionFail(void) {
    if (gShellRuntime.in_subshell) {
        shellExpansionFail();
        return;
    }
    gShellExpansionFailed = true;
    shellUpdateStatus(1);
    /* set -u has ended the script by now. */
    if (gShellExpandingJobWords || gShellExitRequested) {
        return;
    }
    gShellDiscardPending = true;
    gShellDiscardInText = gShellCommandSubFrame == NULL;
    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = true;
        gShellCurrentVm->current_builtin_name = "return";
    }
}

static void shellFlattenArraySeparators(char *text) {
    for (char *cursor = text; cursor && *cursor; ++cursor) {
        if (*cursor == SHELL_ARRAY_ELEMENT_SEP) {
            *cursor = ' ';
        }
    }
}

/* "$*": the elements joined with IFS's first character, or nothing if IFS is empty. */
static char *shellJoinWithIfs(char **items, size_t count) {
    const char *ifs = shellCurrentIfs();
    char separator = ifs ? ifs[0] : ' ';
    size_t total = 1;
    for (size_t i = 0; i < count; ++i) {
        total += (items[i] ? strlen(items[i]) : 0) + 1;
    }
    char *out = (char *)malloc(total);
    if (!out) {
        return NULL;
    }
    size_t length = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0 && separator) {
            out[length++] = separator;
        }
        size_t n = items[i] ? strlen(items[i]) : 0;
        if (n > 0) {
            memcpy(out + length, items[i], n);
            length += n;
        }
    }
    out[length] = '\0';
    return out;
}

/* Bash's arrays that the runtime keeps elsewhere: copy them in when they are read. */
static void shellRefreshSpecialArray(const char *name, size_t len) {
    if (len == 13 && strncmp(name, "BASH_VERSINFO", 13) == 0) {
        struct utsname info;
        char *items[6] = {"5", "2", "0", "1", "exsh", "unknown"};
        if (uname(&info) == 0) {
            items[5] = info.machine;
        }
        shellArrayRegistryStore("BASH_VERSINFO", items, NULL, 6, SHELL_ARRAY_KIND_INDEXED);
    } else if (len == 11 && strncmp(name, "BASH_SOURCE", 11) == 0) {
        size_t count = gShellSourceNameCount + 1;
        char **items = (char **)calloc(count, sizeof(char *));
        if (items) {
            for (size_t i = 0; i < gShellSourceNameCount; ++i) {
                items[i] = strdup(gShellSourceNames[i]);
            }
            items[count - 1] = strdup(gShellArg0 ? gShellArg0 : "exsh");
            shellArrayRegistryStore("BASH_SOURCE", items, NULL, count, SHELL_ARRAY_KIND_INDEXED);
            shellFreeArrayValues(items, count);
        }
    } else if (len == 10 && strncmp(name, "PIPESTATUS", 10) == 0) {
        char **items = (char **)calloc(gShellPipeStatusCount ? gShellPipeStatusCount : 1, sizeof(char *));
        if (!items) {
            return;
        }
        for (size_t i = 0; i < gShellPipeStatusCount; ++i) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%d", gShellPipeStatus[i]);
            items[i] = strdup(buffer);
        }
        shellArrayRegistryStore("PIPESTATUS", items, NULL, gShellPipeStatusCount, SHELL_ARRAY_KIND_INDEXED);
        shellFreeArrayValues(items, gShellPipeStatusCount);
    } else if (len == 8 && strncmp(name, "FUNCNAME", 8) == 0) {
        if (gShellFunctionNameCount == 0) {
            shellArrayRegistryRemove("FUNCNAME");
            return;
        }
        /* Bash ends the list with the script itself. */
        size_t count = gShellFunctionNameCount + 1;
        char **items = (char **)calloc(count, sizeof(char *));
        if (items) {
            for (size_t i = 0; i < gShellFunctionNameCount; ++i) {
                items[i] = strdup(gShellFunctionNames[i]);
            }
            items[count - 1] = strdup("main");
            shellArrayRegistryStore("FUNCNAME", items, NULL, count, SHELL_ARRAY_KIND_INDEXED);
            shellFreeArrayValues(items, count);
        }
    }
}

/* The word inside ${x:-word}: the lexer left it as raw shell text. Quotes become the
 * markers shellExpandWord reads, and nested expansions stay as they are. */
static char *shellParamMarkWord(const char *raw, size_t len, bool in_double) {
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    shellBufferEnsure(&out, &length, &capacity, len + 8);
    if (out) {
        /* An empty word leaves the loop below with nothing appended. */
        out[0] = '\0';
    }
    bool in_inner_double = false;
    size_t i = 0;
    while (i < len) {
        char c = raw[i];
        bool quoted_context = in_double || in_inner_double;
        if (c == '\\' && i + 1 < len) {
            char next = raw[i + 1];
            if (next == '\n') {
                i += 2;
                continue;
            }
            if (!quoted_context || next == '$' || next == '`' || next == '"' || next == '\\') {
                shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_SINGLE);
                shellBufferAppendChar(&out, &length, &capacity, next);
                shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_SINGLE);
                i += 2;
                continue;
            }
        }
        if (c == '\'' && !quoted_context) {
            size_t stop = shellScanSingleQuoted(raw, len, i + 1);
            if (stop == SHELL_SCAN_UNTERMINATED) {
                stop = len + 1;
            }
            shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_SINGLE);
            shellBufferAppendSlice(&out, &length, &capacity, raw + i + 1, stop - i - 2);
            shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_SINGLE);
            i = stop;
            continue;
        }
        if (c == '"') {
            in_inner_double = !in_inner_double;
            shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_DOUBLE);
            i++;
            continue;
        }
        if (c == '$' && i + 1 < len && raw[i + 1] == '"' && !quoted_context) {
            i++;
            continue;
        }
        if (c == '$' && i + 1 < len && raw[i + 1] == '\'' && !quoted_context) {
            char *decoded = (char *)malloc(len - i + 1);
            size_t decoded_len = 0;
            size_t stop = decoded ? shellScanAnsiC(raw, len, i + 2, decoded, &decoded_len) : len;
            shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_SINGLE);
            for (size_t k = 0; decoded && k < decoded_len; ++k) {
                if (decoded[k] == SHELL_QUOTE_MARK_SINGLE || decoded[k] == SHELL_QUOTE_MARK_DOUBLE ||
                    decoded[k] == SHELL_QUOTE_MARK_BYTE) {
                    shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_BYTE);
                }
                shellBufferAppendChar(&out, &length, &capacity, decoded[k]);
            }
            shellBufferAppendChar(&out, &length, &capacity, SHELL_QUOTE_MARK_SINGLE);
            free(decoded);
            i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
            continue;
        }
        if (c == '$' || c == '`') {
            size_t stop = c == '$' ? shellScanDollar(raw, len, i, quoted_context) : shellScanBacktick(raw, len, i + 1);
            if (stop == SHELL_SCAN_UNTERMINATED || stop > len) {
                stop = len;
            }
            shellBufferAppendSlice(&out, &length, &capacity, raw + i, stop - i);
            i = stop;
            continue;
        }
        shellBufferAppendChar(&out, &length, &capacity, c);
        i++;
    }
    if (!out) {
        return strdup("");
    }
    return out;
}

static char *shellParamExpandWord(const char *raw, size_t len, bool in_double, bool **out_map, size_t *out_map_len) {
    char *marked = shellParamMarkWord(raw, len, in_double);
    if (!marked) {
        return NULL;
    }
    uint8_t flags = in_double ? SHELL_WORD_FLAG_DOUBLE_QUOTED : 0;
    char *expanded = shellExpandWord(marked, flags, NULL, 0, out_map, out_map_len, NULL);
    free(marked);
    return expanded;
}

/* A pattern word: what came from quotes is literal, and an unquoted expansion still
 * matches as a pattern. Double quotes around the whole ${...} make no difference here. */
static char *shellParamExpandPattern(const char *raw, size_t len) {
    bool *map = NULL;
    size_t map_len = 0;
    char *text = shellParamExpandWord(raw, len, false, &map, &map_len);
    if (!text) {
        return NULL;
    }
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t text_len = strlen(text);
    for (size_t i = 0; i < text_len; ++i) {
        if (map && i < map_len && map[i] && strchr("*?[]\\", text[i])) {
            shellBufferAppendChar(&out, &length, &capacity, '\\');
        }
        shellBufferAppendChar(&out, &length, &capacity, text[i]);
    }
    free(map);
    free(text);
    return out ? out : strdup("");
}

static void shellParamBadSubstitution(const char *inner, size_t inner_len) {
    shellReportRecoverableError(gShellCurrentVm, true, "${%.*s}: bad substitution", (int)inner_len, inner);
    shellExpansionFail();
}

/* Key to look an element up by: arithmetic for an indexed array, text for an associative
 * one. Past the start of the array by a negative index gives NULL. */
static char *shellParamSubscriptKey(const char *name, size_t name_len, const char *subscript, size_t subscript_len) {
    const ShellArrayVariable *var = shellArrayRegistryLookup(name, name_len);
    bool *map = NULL;
    size_t map_len = 0;
    if (var && var->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && !var->sparse) {
        char *key = shellParamExpandWord(subscript, subscript_len, false, &map, &map_len);
        free(map);
        return key;
    }
    char *expr = (char *)malloc(subscript_len + 1);
    if (!expr) {
        return NULL;
    }
    memcpy(expr, subscript, subscript_len);
    expr[subscript_len] = '\0';
    long long value = 0;
    bool ok = shellArithmeticEvaluate(expr, &value, "");
    free(expr);
    if (!ok) {
        shellArithmeticExpansionFail();
        return NULL;
    }
    if (value < 0) {
        value += var ? (long long)var->count : 0;
        if (value < 0) {
            return NULL;
        }
    }
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    return strdup(buffer);
}

/* False if the subscript's own arithmetic failed, reported by then. With no key and no failure
 * the index is before the start of the array. */
static bool shellParamSubscriptKeyChecked(const char *name, size_t name_len, const char *subscript, size_t subscript_len,
                                          char **out_key) {
    bool failed_before = gShellExpansionFailed;
    gShellExpansionFailed = false;
    *out_key = shellParamSubscriptKey(name, name_len, subscript, subscript_len);
    bool failed = gShellExpansionFailed;
    gShellExpansionFailed = failed_before || failed;
    return !failed;
}

typedef struct {
    bool is_list; /* $@ $* a[@] a[*] */
    char style;
    char **items;
    size_t count;
    char *value;
    bool is_set;
    bool failed; /* its subscript's arithmetic, reported by then */
} ShellParamSubject;

static void shellParamSubjectFree(ShellParamSubject *subject) {
    shellFreeArrayValues(subject->items, subject->count);
    free(subject->value);
    memset(subject, 0, sizeof(*subject));
}

static char **shellParamCopyItems(char **items, size_t count) {
    char **copy = (char **)calloc(count ? count : 1, sizeof(char *));
    if (!copy) {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        copy[i] = strdup(items && items[i] ? items[i] : "");
    }
    return copy;
}

static bool shellParamLoadList(const char *name, size_t name_len, char style, ShellParamSubject *subject) {
    subject->is_list = true;
    subject->style = style;
    if (name_len == 1 && (name[0] == '@' || name[0] == '*')) {
        subject->count = gParamCount > 0 ? (size_t)gParamCount : 0;
        subject->items = shellParamCopyItems(gParamValues, subject->count);
        subject->is_set = subject->count > 0;
        return subject->items != NULL;
    }
    shellRefreshSpecialArray(name, name_len);
    const ShellArrayVariable *var = shellArrayRegistryLookup(name, name_len);
    if (var) {
        subject->count = var->count;
        subject->items = shellParamCopyItems(var->values, var->count);
        subject->is_set = var->count > 0;
        return subject->items != NULL;
    }
    bool is_set = false;
    char *value = shellLookupParameterValueInternal(name, name_len, &is_set);
    if (!value) {
        return false;
    }
    if (is_set) {
        size_t count = 0;
        char **items = (char **)calloc(1, sizeof(char *));
        if (items) {
            items[0] = value;
            value = NULL;
            count = 1;
        }
        subject->items = items;
        subject->count = count;
        subject->is_set = true;
    }
    free(value);
    return true;
}

static bool shellParamLoadSubject(const char *name, size_t name_len, const char *subscript, size_t subscript_len,
                                  ShellParamSubject *subject) {
    memset(subject, 0, sizeof(*subject));
    if (name_len == 1 && (name[0] == '@' || name[0] == '*')) {
        return shellParamLoadList(name, name_len, name[0], subject);
    }
    if (subscript) {
        const char *as_written = subscript;
        size_t as_written_len = subscript_len;
        while (subscript_len > 0 && isspace((unsigned char)subscript[0])) {
            subscript++;
            subscript_len--;
        }
        while (subscript_len > 0 && isspace((unsigned char)subscript[subscript_len - 1])) {
            subscript_len--;
        }
        if (subscript_len == 1 && (subscript[0] == '@' || subscript[0] == '*')) {
            return shellParamLoadList(name, name_len, subscript[0], subject);
        }
        shellRefreshSpecialArray(name, name_len);
        /* An expression is reported as written. */
        const ShellArrayVariable *keyed = shellArrayRegistryLookup(name, name_len);
        bool arithmetic = !(keyed && keyed->kind == SHELL_ARRAY_KIND_ASSOCIATIVE && !keyed->sparse);
        char *key = NULL;
        bool key_ok = shellParamSubscriptKeyChecked(name, name_len, arithmetic ? as_written : subscript,
                                                    arithmetic ? as_written_len : subscript_len, &key);
        if (!key) {
            subject->failed = !key_ok;
            subject->value = strdup("");
            return subject->value != NULL;
        }
        const ShellArrayVariable *var = shellArrayRegistryLookup(name, name_len);
        if (var) {
            if (var->kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
                for (size_t i = 0; i < var->count; ++i) {
                    if (var->keys && var->keys[i] && strcmp(var->keys[i], key) == 0) {
                        subject->is_set = true;
                        break;
                    }
                }
            } else {
                subject->is_set = strtoull(key, NULL, 10) < var->count;
            }
        } else {
            bool is_set = false;
            free(shellLookupParameterValueInternal(name, name_len, &is_set));
            subject->is_set = is_set && strcmp(key, "0") == 0;
        }
        subject->value = shellExpandArraySubscriptValue(name, name_len, key, strlen(key), NULL, NULL);
        free(key);
        return subject->value != NULL;
    }
    if (name_len == 1 && name[0] == '!') {
        char buffer[32] = "";
        if (gShellRuntime.last_bg_pid > 0) {
            snprintf(buffer, sizeof(buffer), "%d", (int)gShellRuntime.last_bg_pid);
        }
        subject->value = strdup(buffer);
        subject->is_set = buffer[0] != '\0';
        return subject->value != NULL;
    }
    shellRefreshSpecialArray(name, name_len);
    subject->value = shellLookupParameterValueInternal(name, name_len, &subject->is_set);
    return subject->value != NULL;
}

/* The first bytes of a character, so a UTF-8 sequence is never cut. */
static size_t shellParamNextChar(const char *value, size_t len, size_t i) {
    i++;
    while (i < len && ((unsigned char)value[i] & 0xC0) == 0x80) {
        i++;
    }
    return i;
}

static bool shellParamPatternHasGlob(const char *pattern) {
    return strpbrk(pattern, "*?[\\") != NULL;
}

/* End of the longest match starting at `start`, or (size_t)-1. work is a private copy of value. */
static size_t shellParamMatchAt(char *work, size_t len, size_t start, const char *pattern, bool plain, size_t plain_len,
                                bool to_end) {
    if (plain) {
        if (start + plain_len > len || memcmp(work + start, pattern, plain_len) != 0) {
            return (size_t)-1;
        }
        size_t end = start + plain_len;
        return (to_end && end != len) ? (size_t)-1 : end;
    }
    size_t end = len;
    while (true) {
        if (end == len || ((unsigned char)work[end] & 0xC0) != 0x80) {
            char saved = work[end];
            work[end] = '\0';
            int rc = shellPatternMatch(pattern, work + start);
            work[end] = saved;
            if (rc == 0) {
                return end;
            }
        }
        if (to_end || end == start) {
            return (size_t)-1;
        }
        end--;
    }
}

/* anchor: 0 anywhere, '#' at the start, '%' at the end. */
static char *shellParamReplace(const char *value, const char *pattern, const char *replacement, bool all, char anchor) {
    size_t len = strlen(value);
    if (!*pattern && !anchor) {
        return strdup(value);
    }
    char *work = strdup(value);
    if (!work) {
        return NULL;
    }
    bool plain = !shellParamPatternHasGlob(pattern);
    size_t plain_len = strlen(pattern);
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t pos = 0;
    while (pos <= len) {
        size_t start = pos;
        size_t end = (size_t)-1;
        while (start <= len) {
            if (anchor != '#' || start == 0) {
                end = shellParamMatchAt(work, len, start, pattern, plain, plain_len, anchor == '%');
            }
            if (end != (size_t)-1 || anchor == '#') {
                break;
            }
            if (start == len) {
                break;
            }
            start = shellParamNextChar(value, len, start);
        }
        if (end == (size_t)-1) {
            break;
        }
        shellBufferAppendSlice(&out, &length, &capacity, value + pos, start - pos);
        shellBufferAppendString(&out, &length, &capacity, replacement);
        if (end == start) {
            if (!all || start >= len) {
                pos = start;
                break;
            }
            size_t next = shellParamNextChar(value, len, start);
            shellBufferAppendSlice(&out, &length, &capacity, value + start, next - start);
            pos = next;
        } else {
            pos = end;
        }
        if (!all || anchor) {
            break;
        }
    }
    if (pos < len) {
        shellBufferAppendSlice(&out, &length, &capacity, value + pos, len - pos);
    }
    free(work);
    return out ? out : strdup("");
}

/* ASCII only, which is as far as the C locale goes too. */
static char *shellParamChangeCase(const char *value, bool upper, bool all, const char *pattern) {
    char *out = strdup(value);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; out[i]; ++i) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x80) {
            bool applies = true;
            if (pattern && *pattern) {
                char one[2] = {(char)c, '\0'};
                applies = shellPatternMatch(pattern, one) == 0;
            }
            if (applies) {
                out[i] = (char)(upper ? toupper(c) : tolower(c));
            }
        }
        if (!all) {
            break;
        }
    }
    return out;
}

/* Quoted so that it reads back as the same word: ${x@Q}. */
static char *shellParamQuote(const char *value) {
    char *out = NULL;
    size_t length = 0;
    size_t capacity = 0;
    shellBufferAppendChar(&out, &length, &capacity, '\'');
    for (const char *cursor = value; *cursor; ++cursor) {
        if (*cursor == '\'') {
            shellBufferAppendString(&out, &length, &capacity, "'\\''");
        } else {
            shellBufferAppendChar(&out, &length, &capacity, *cursor);
        }
    }
    shellBufferAppendChar(&out, &length, &capacity, '\'');
    return out;
}

typedef enum {
    SHELL_PARAM_OP_REMOVE,
    SHELL_PARAM_OP_REPLACE,
    SHELL_PARAM_OP_CASE,
    SHELL_PARAM_OP_QUOTE
} ShellParamOpKind;

typedef struct {
    ShellParamOpKind kind;
    bool suffix;
    bool all;
    bool upper;
    char anchor;
    const char *pattern;
    const char *replacement;
} ShellParamOp;

static char *shellParamApplyOp(const ShellParamOp *op, const char *value) {
    switch (op->kind) {
        case SHELL_PARAM_OP_REMOVE:
            return op->suffix ? shellRemovePatternSuffix(value, op->pattern, op->all)
                              : shellRemovePatternPrefix(value, op->pattern, op->all);
        case SHELL_PARAM_OP_REPLACE:
            return shellParamReplace(value, op->pattern, op->replacement, op->all, op->anchor);
        case SHELL_PARAM_OP_CASE:
            return shellParamChangeCase(value, op->upper, op->all, op->pattern);
        case SHELL_PARAM_OP_QUOTE:
            return shellParamQuote(value);
    }
    return NULL;
}

/* Where the pattern of ${x/pattern/word} ends. */
static size_t shellParamPatternEnd(const char *raw, size_t len) {
    size_t i = 0;
    while (i < len) {
        char c = raw[i];
        if (c == '/') {
            return i;
        }
        if (c == '\\') {
            i += 2;
        } else if (c == '\'') {
            size_t stop = shellScanSingleQuoted(raw, len, i + 1);
            i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
        } else if (c == '"') {
            size_t stop = shellScanDoubleQuoted(raw, len, i + 1);
            i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
        } else if (c == '$') {
            size_t stop = shellScanDollar(raw, len, i, false);
            i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
        } else if (c == '`') {
            size_t stop = shellScanBacktick(raw, len, i + 1);
            i = stop == SHELL_SCAN_UNTERMINATED ? len : stop;
        } else {
            i++;
        }
    }
    return len;
}

/* The offset and length of ${x:offset:length}. */
static bool shellParamEvalSlice(const char *label, const char *raw, size_t len, long long *out_offset, bool *out_has_length,
                                long long *out_length) {
    size_t split = len;
    int depth = 0;
    int ternary = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = raw[i];
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
        } else if (depth == 0 && c == '?') {
            ternary++;
        } else if (depth == 0 && c == ':') {
            if (ternary > 0) {
                ternary--;
            } else {
                split = i;
                break;
            }
        }
    }
    char *expr = (char *)malloc(len + 1);
    if (!expr) {
        return false;
    }
    memcpy(expr, raw, split);
    expr[split] = '\0';
    bool ok = shellArithmeticEvaluate(expr, out_offset, label);
    if (ok) {
        *out_has_length = split < len;
        if (*out_has_length) {
            memcpy(expr, raw + split + 1, len - split - 1);
            expr[len - split - 1] = '\0';
            ok = shellArithmeticEvaluate(expr, out_length, label);
        }
    }
    if (!ok) {
        shellArithmeticExpansionFail();
    }
    free(expr);
    return ok;
}

static char *shellParamSliceScalar(const char *value, long long offset, bool has_length, long long length) {
    size_t value_len = strlen(value);
    bool utf8 = value_len > 0 && isValidUtf8Bytes(value, value_len);
    long long logical = utf8 ? (long long)utf8CodepointCount(value, value_len) : (long long)value_len;
    if (offset < 0) {
        offset += logical;
        if (offset < 0) {
            return strdup("");
        }
    }
    if (offset > logical) {
        return strdup("");
    }
    long long end = logical;
    if (has_length) {
        end = length < 0 ? logical + length : offset + length;
        if (end > logical) {
            end = logical;
        }
        if (end < offset) {
            if (length < 0) {
                shellReportRecoverableError(gShellCurrentVm, true, "%lld: substring expression < 0", length);
                shellExpansionFail();
            }
            return strdup("");
        }
    }
    size_t start_byte = utf8 ? utf8ByteOffsetForCodepointIndex(value, value_len, (size_t)offset) : (size_t)offset;
    size_t end_byte = utf8 ? utf8ByteOffsetForCodepointIndex(value, value_len, (size_t)end) : (size_t)end;
    size_t n = end_byte - start_byte;
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, value + start_byte, n);
    out[n] = '\0';
    return out;
}

/* ${@:1:2} and ${a[@]:1:2}. Positional parameters count from $0. */
static void shellParamSliceList(ShellParamSubject *subject, bool positional, long long offset, bool has_length,
                                long long length) {
    size_t count = subject->count;
    long long total = (long long)count + (positional ? 1 : 0);
    if (offset < 0) {
        offset += total;
    }
    if (has_length && length < 0) {
        shellReportRecoverableError(gShellCurrentVm, true, "%lld: substring expression < 0", length);
        shellExpansionFail();
        length = 0;
    }
    char **kept = (char **)calloc(count + 1, sizeof(char *));
    size_t kept_count = 0;
    if (kept && offset >= 0) {
        for (long long logical = offset; logical < total; ++logical) {
            if (has_length && logical - offset >= length) {
                break;
            }
            if (positional && logical == 0) {
                kept[kept_count++] = strdup(gShellArg0 ? gShellArg0 : "exsh");
            } else {
                size_t index = (size_t)(logical - (positional ? 1 : 0));
                kept[kept_count++] = subject->items[index];
                subject->items[index] = NULL;
            }
        }
    }
    shellFreeArrayValues(subject->items, count);
    subject->items = kept;
    subject->count = kept_count;
}

static char *shellParamFinishList(const ShellParamSubject *subject, bool in_double, bool *out_array, size_t *out_count) {
    if (subject->style == '*' && in_double) {
        return shellJoinWithIfs(subject->items, subject->count);
    }
    if (out_array) {
        *out_array = true;
    }
    if (out_count) {
        *out_count = subject->count;
    }
    return shellJoinArrayValuesWithSeparator(subject->items, subject->count, SHELL_ARRAY_ELEMENT_SEP);
}

static int shellParamCompareStrings(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ${!prefix*}: the names of the set variables. */
static char *shellParamNamesWithPrefix(const char *prefix, size_t prefix_len, char style, bool in_double,
                                       bool *out_array, size_t *out_count) {
    ShellParamSubject subject;
    memset(&subject, 0, sizeof(subject));
    subject.is_list = true;
    subject.style = style;
    size_t capacity = 0;
    void *walk = NULL;
    for (char **entry = pscalEnvironWalkBegin(&walk); entry && *entry; ++entry) {
        const char *eq = strchr(*entry, '=');
        if (!eq || strncmp(*entry, prefix, prefix_len) != 0) {
            continue;
        }
        char *name = strndup(*entry, (size_t)(eq - *entry));
        if (name && !shellAppendArrayValue(&subject.items, &subject.count, &capacity, name)) {
            break;
        }
    }
    pscalEnvironWalkEnd(walk);
    for (size_t i = 0; i < gShellArrayVarCount; ++i) {
        const char *name = gShellArrayVars[i].name;
        if (!name || strncmp(name, prefix, prefix_len) != 0 || shellSafeGetenv(name)) {
            continue;
        }
        char *copy = strdup(name);
        if (copy && !shellAppendArrayValue(&subject.items, &subject.count, &capacity, copy)) {
            break;
        }
    }
    if (subject.count > 1) {
        qsort(subject.items, subject.count, sizeof(char *), shellParamCompareStrings);
    }
    char *result = shellParamFinishList(&subject, in_double, out_array, out_count);
    shellParamSubjectFree(&subject);
    return result;
}

/* ${!a[@]}: the indices or keys of an array. */
static char *shellParamArrayKeys(const char *name, size_t name_len, char style, bool in_double, bool *out_array,
                                 size_t *out_count) {
    ShellParamSubject subject;
    memset(&subject, 0, sizeof(subject));
    subject.is_list = true;
    subject.style = style;
    const ShellArrayVariable *var = shellArrayRegistryLookup(name, name_len);
    size_t capacity = 0;
    if (var) {
        for (size_t i = 0; i < var->count; ++i) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%zu", i);
            char *key = strdup((var->keys && var->keys[i]) ? var->keys[i] : buffer);
            if (key) {
                shellAppendArrayValue(&subject.items, &subject.count, &capacity, key);
            }
        }
    } else {
        bool is_set = false;
        free(shellLookupParameterValueInternal(name, name_len, &is_set));
        if (is_set) {
            char *key = strdup("0");
            if (key) {
                shellAppendArrayValue(&subject.items, &subject.count, &capacity, key);
            }
        }
    }
    char *result = shellParamFinishList(&subject, in_double, out_array, out_count);
    shellParamSubjectFree(&subject);
    return result;
}

/* The name and optional subscript at the start of a parameter expression. */
static bool shellParamParseName(const char *inner, size_t inner_len, size_t *out_name_len, const char **out_subscript,
                                size_t *out_subscript_len, size_t *out_consumed) {
    *out_subscript = NULL;
    *out_subscript_len = 0;
    if (inner_len == 0) {
        return false;
    }
    size_t name_len = 0;
    unsigned char first = (unsigned char)inner[0];
    if (strchr("@*?$!-#", first)) {
        name_len = 1;
    } else if (isdigit(first)) {
        while (name_len < inner_len && isdigit((unsigned char)inner[name_len])) {
            name_len++;
        }
    } else if (!consumeShellIdentifier(inner, inner_len, &name_len, false)) {
        return false;
    } else if (name_len < inner_len && inner[name_len] == '[') {
        size_t i = name_len + 1;
        int depth = 1;
        while (i < inner_len && depth > 0) {
            char c = inner[i];
            if (c == '\'' || c == '"') {
                size_t stop = c == '\'' ? shellScanSingleQuoted(inner, inner_len, i + 1)
                                        : shellScanDoubleQuoted(inner, inner_len, i + 1);
                if (stop == SHELL_SCAN_UNTERMINATED) {
                    return false;
                }
                i = stop;
                continue;
            }
            if (c == '[') {
                depth++;
            } else if (c == ']') {
                depth--;
            }
            i++;
        }
        if (depth != 0) {
            return false;
        }
        *out_subscript = inner + name_len + 1;
        *out_subscript_len = i - name_len - 2;
        *out_name_len = name_len;
        *out_consumed = i;
        return true;
    }
    *out_name_len = name_len;
    *out_consumed = name_len;
    return true;
}

static char *shellExpandBraceParameter(const char *inner, size_t inner_len, bool in_double, bool *out_array,
                                       size_t *out_count, bool **out_map, size_t *out_map_len);

static char *shellParamLength(const char *inner, size_t inner_len) {
    size_t name_len = 0;
    size_t consumed = 0;
    const char *subscript = NULL;
    size_t subscript_len = 0;
    if (!shellParamParseName(inner, inner_len, &name_len, &subscript, &subscript_len, &consumed) ||
        consumed != inner_len) {
        return NULL;
    }
    ShellParamSubject subject;
    if (!shellParamLoadSubject(inner, name_len, subscript, subscript_len, &subject)) {
        return NULL;
    }
    size_t length = 0;
    if (subject.is_list) {
        length = subject.count;
    } else {
        if (!subject.is_set && gShellRuntime.nounset_enabled && !gShellNounsetSuppressed && !strchr("?$-#", inner[0])) {
            shellReportRecoverableError(gShellCurrentVm, true, "%.*s: unbound variable", (int)name_len, inner);
            shellExpansionFail();
        }
        length = subject.value ? strlen(subject.value) : 0;
        if (length > 0 && isValidUtf8Bytes(subject.value, length)) {
            length = utf8CodepointCount(subject.value, length);
        }
    }
    shellParamSubjectFree(&subject);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%zu", length);
    return strdup(buffer);
}

/* ${!@} and ${!*}: "$*" names the variable, and what follows applies to that. With no positional
 * parameters nothing is named, so only a default gives anything. */
static char *shellParamIndirectPositional(const char *inner, size_t inner_len, bool in_double, bool *out_array,
                                          size_t *out_count, bool **out_map, size_t *out_map_len) {
    const char *op = inner + 2;
    size_t op_len = inner_len - 2;
    if (op_len > 0 && !strchr(":-=+?#%/@", op[0])) {
        shellParamBadSubstitution(inner, inner_len);
        return strdup("");
    }
    bool colon_assign = op_len > 1 && op[0] == ':' && op[1] == '=';
    bool assigns = colon_assign || (op_len > 0 && op[0] == '=');
    ShellParamSubject positional;
    if (!shellParamLoadSubject(inner + 1, 1, NULL, 0, &positional)) {
        return strdup("");
    }
    char *target = positional.count > 0 ? shellJoinWithIfs(positional.items, positional.count) : NULL;
    bool any = positional.count > 0;
    shellParamSubjectFree(&positional);
    if (!any) {
        if (assigns) {
            shellReportRecoverableError(gShellCurrentVm, true, "$!%c: cannot assign in this way", inner[1]);
            shellExpansionFail();
            return strdup("");
        }
        if (gShellRuntime.nounset_enabled && !gShellNounsetSuppressed && op_len == 0) {
            shellReportRecoverableError(gShellCurrentVm, true, "!%c: unbound variable", inner[1]);
            shellExpansionFail();
            return strdup("");
        }
        size_t skip = op_len > 0 && op[0] == ':' ? 1 : 0;
        if (op_len > skip && op[skip] == '-') {
            return shellParamExpandWord(op + skip + 1, op_len - skip - 1, in_double, out_map, out_map_len);
        }
        return strdup("");
    }
    if (!target) {
        return NULL;
    }
    size_t target_len = strlen(target);
    size_t name_len = 0;
    size_t consumed = 0;
    const char *subscript = NULL;
    size_t subscript_len = 0;
    if (target_len == 0 || !shellParamParseName(target, target_len, &name_len, &subscript, &subscript_len, &consumed) ||
        consumed != target_len) {
        shellReportRecoverableError(gShellCurrentVm, true, "%s: invalid variable name", target);
        shellExpansionFail();
        free(target);
        return strdup("");
    }
    if (assigns) {
        /* The name holds a value, and that is all this gives. It takes no new one. */
        ShellParamSubject named;
        bool present = false;
        if (shellParamLoadSubject(target, name_len, subscript, subscript_len, &named)) {
            present = named.is_set && named.value && (!colon_assign || named.value[0]);
            shellParamSubjectFree(&named);
        }
        if (!present) {
            shellReportRecoverableError(gShellCurrentVm, true, "$!%c: cannot assign in this way", inner[1]);
            shellExpansionFail();
            free(target);
            return strdup("");
        }
    }
    char *combined = (char *)malloc(target_len + op_len + 1);
    if (!combined) {
        free(target);
        return NULL;
    }
    memcpy(combined, target, target_len);
    memcpy(combined + target_len, op, op_len);
    combined[target_len + op_len] = '\0';
    free(target);
    char *result = shellExpandBraceParameter(combined, target_len + op_len, in_double, out_array, out_count, out_map,
                                             out_map_len);
    free(combined);
    return result;
}

static char *shellExpandBraceParameter(const char *inner, size_t inner_len, bool in_double, bool *out_array,
                                       size_t *out_count, bool **out_map, size_t *out_map_len) {
    if (inner_len == 0) {
        shellParamBadSubstitution(inner, inner_len);
        return strdup("");
    }

    /* ${#x}. A `#` followed by an operator is $# with that operator: ${#:-e}. */
    if (inner[0] == '#' && inner_len > 1 &&
        !(strchr(":-=?+%#/", inner[1]) && !(inner_len == 2 && inner[1] != ':') )) {
        char *length = shellParamLength(inner + 1, inner_len - 1);
        if (!length) {
            shellParamBadSubstitution(inner, inner_len);
            return strdup("");
        }
        return length;
    }
    /* ${!name}, ${!name[@]}, ${!prefix*}. An operator right after the `!` is $! with that
     * operator: ${!:-x}. */
    if (inner[0] == '!' && inner_len > 1 && !strchr(":-=+%/^,", inner[1])) {
        const char *rest = inner + 1;
        size_t rest_len = inner_len - 1;
        if (rest[0] == '!' || rest[0] == '$') {
            shellParamBadSubstitution(inner, inner_len);
            return strdup("");
        }
        if (rest[0] == '@' || rest[0] == '*') {
            return shellParamIndirectPositional(inner, inner_len, in_double, out_array, out_count, out_map, out_map_len);
        }
        char last = rest[rest_len - 1];
        size_t name_len = 0;
        if ((last == '*' || last == '@') && rest_len > 1 &&
            consumeShellIdentifier(rest, rest_len - 1, &name_len, false) && name_len == rest_len - 1) {
            return shellParamNamesWithPrefix(rest, name_len, last, in_double, out_array, out_count);
        }
        size_t consumed = 0;
        const char *subscript = NULL;
        size_t subscript_len = 0;
        if (!shellParamParseName(rest, rest_len, &name_len, &subscript, &subscript_len, &consumed)) {
            shellParamBadSubstitution(inner, inner_len);
            return strdup("");
        }
        if (subscript && subscript_len == 1 && (subscript[0] == '@' || subscript[0] == '*') && consumed == rest_len) {
            return shellParamArrayKeys(rest, name_len, subscript[0], in_double, out_array, out_count);
        }
        ShellParamSubject reference;
        if (!shellParamLoadSubject(rest, name_len, subscript, subscript_len, &reference)) {
            return strdup("");
        }
        char *target = reference.value ? strdup(reference.value) : NULL;
        shellParamSubjectFree(&reference);
        if (!target || !*target) {
            free(target);
            return strdup("");
        }
        size_t target_len = strlen(target);
        size_t op_len = rest_len - consumed;
        char *combined = (char *)malloc(target_len + op_len + 1);
        if (!combined) {
            free(target);
            return NULL;
        }
        memcpy(combined, target, target_len);
        memcpy(combined + target_len, rest + consumed, op_len);
        combined[target_len + op_len] = '\0';
        free(target);
        char *result = shellExpandBraceParameter(combined, target_len + op_len, in_double, out_array, out_count, out_map,
                                                 out_map_len);
        free(combined);
        return result;
    }

    size_t name_len = 0;
    size_t consumed = 0;
    const char *subscript = NULL;
    size_t subscript_len = 0;
    if (!shellParamParseName(inner, inner_len, &name_len, &subscript, &subscript_len, &consumed)) {
        shellParamBadSubstitution(inner, inner_len);
        return strdup("");
    }
    const char *op = inner + consumed;
    size_t op_len = inner_len - consumed;

    ShellParamSubject subject;
    if (!shellParamLoadSubject(inner, name_len, subscript, subscript_len, &subject)) {
        return NULL;
    }
    if (subject.failed) {
        shellParamSubjectFree(&subject);
        return strdup("");
    }

    /* ${x:-word} ${x-word} ${x:=word} ${x=word} ${x:?word} ${x?word} ${x:+word} ${x+word} */
    bool colon = op_len >= 2 && op[0] == ':' && strchr("-=?+", op[1]);
    if (colon || (op_len >= 1 && strchr("-=?+", op[0]))) {
        char kind = colon ? op[1] : op[0];
        const char *word = op + (colon ? 2 : 1);
        size_t word_len = op_len - (colon ? 2 : 1);
        bool present;
        if (subject.is_list) {
            present = subject.count > 0;
            if (colon && present && subject.count == 1 && !subject.items[0][0]) {
                present = false;
            }
        } else {
            present = subject.is_set && (!colon || subject.value[0] != '\0');
        }
        char *result = NULL;
        if (kind == '+') {
            if (present) {
                result = shellParamExpandWord(word, word_len, in_double, out_map, out_map_len);
            } else {
                result = strdup("");
            }
        } else if (present) {
            result = subject.is_list ? shellParamFinishList(&subject, in_double, out_array, out_count)
                                     : strdup(subject.value);
        } else if (kind == '-') {
            result = shellParamExpandWord(word, word_len, in_double, out_map, out_map_len);
        } else if (kind == '=') {
            result = shellParamExpandWord(word, word_len, in_double, NULL, NULL);
            if (result) {
                if (name_len == 1 && !isalpha((unsigned char)inner[0]) && inner[0] != '_') {
                    shellReportRecoverableError(gShellCurrentVm, true, "$%.*s: cannot assign in this way", (int)name_len,
                                                inner);
                    shellExpansionFail();
                } else if (isdigit((unsigned char)inner[0])) {
                    shellReportRecoverableError(gShellCurrentVm, true, "$%.*s: cannot assign in this way", (int)name_len,
                                                inner);
                    shellExpansionFail();
                } else {
                    char *name = strndup(inner, name_len);
                    if (name) {
                        if (subscript) {
                            char *key = shellParamSubscriptKey(inner, name_len, subscript, subscript_len);
                            if (key) {
                                shellArrayRegistrySetElement(name, key, result);
                                free(key);
                            }
                        } else {
                            shellSetTrackedVariable(name, result, false);
                        }
                        free(name);
                    }
                }
            }
        } else {
            char *message = word_len > 0 ? shellParamExpandWord(word, word_len, in_double, NULL, NULL) : NULL;
            shellReportRecoverableError(gShellCurrentVm, true, "%.*s: %s", (int)name_len, inner,
                                        message ? message : "parameter null or not set");
            free(message);
            shellExpansionFail();
            result = strdup("");
        }
        shellParamSubjectFree(&subject);
        return result;
    }

    if (!subject.is_list && !subject.is_set && gShellRuntime.nounset_enabled && !gShellNounsetSuppressed &&
        !strchr("?$-#", inner[0])) {
        shellReportRecoverableError(gShellCurrentVm, true, "%.*s: unbound variable", (int)name_len, inner);
        shellExpansionFail();
        shellParamSubjectFree(&subject);
        return strdup("");
    }

    if (op_len == 0) {
        char *result = subject.is_list ? shellParamFinishList(&subject, in_double, out_array, out_count)
                                       : strdup(subject.value);
        shellParamSubjectFree(&subject);
        return result;
    }

    /* ${x:offset:length} */
    if (op[0] == ':') {
        long long offset = 0;
        long long length = 0;
        bool has_length = false;
        char *result = NULL;
        /* It reports under the name as written. Nothing to slice, and it is not worked out. */
        char *label = strndup(inner, (size_t)(op - inner));
        if (!subject.is_set && !(name_len == 1 && (inner[0] == '@' || inner[0] == '*'))) {
            result = subject.is_list ? shellParamFinishList(&subject, in_double, out_array, out_count) : strdup("");
        } else if (shellParamEvalSlice(label ? label : "", op + 1, op_len - 1, &offset, &has_length, &length)) {
            if (subject.is_list) {
                shellParamSliceList(&subject, name_len == 1 && (inner[0] == '@' || inner[0] == '*'), offset, has_length,
                                    length);
                result = shellParamFinishList(&subject, in_double, out_array, out_count);
            } else {
                result = shellParamSliceScalar(subject.value, offset, has_length, length);
            }
        } else {
            result = strdup("");
        }
        free(label);
        shellParamSubjectFree(&subject);
        return result;
    }

    ShellParamOp apply;
    memset(&apply, 0, sizeof(apply));
    char *pattern = NULL;
    char *replacement = NULL;
    if (op[0] == '#' || op[0] == '%') {
        apply.kind = SHELL_PARAM_OP_REMOVE;
        apply.suffix = op[0] == '%';
        apply.all = op_len >= 2 && op[1] == op[0];
        size_t skip = apply.all ? 2 : 1;
        pattern = shellParamExpandPattern(op + skip, op_len - skip);
        apply.pattern = pattern;
    } else if (op[0] == '/') {
        apply.kind = SHELL_PARAM_OP_REPLACE;
        size_t skip = 1;
        if (op_len >= 2 && op[1] == '/') {
            apply.all = true;
            skip = 2;
        } else if (op_len >= 2 && (op[1] == '#' || op[1] == '%')) {
            apply.anchor = op[1];
            skip = 2;
        }
        size_t rest_len = op_len - skip;
        size_t pattern_len = shellParamPatternEnd(op + skip, rest_len);
        pattern = shellParamExpandPattern(op + skip, pattern_len);
        if (pattern_len < rest_len) {
            replacement = shellParamExpandWord(op + skip + pattern_len + 1, rest_len - pattern_len - 1, false, NULL, NULL);
        } else {
            replacement = strdup("");
        }
        apply.pattern = pattern;
        apply.replacement = replacement;
    } else if (op[0] == '^' || op[0] == ',') {
        apply.kind = SHELL_PARAM_OP_CASE;
        apply.upper = op[0] == '^';
        apply.all = op_len >= 2 && op[1] == op[0];
        size_t skip = apply.all ? 2 : 1;
        pattern = shellParamExpandPattern(op + skip, op_len - skip);
        apply.pattern = pattern;
    } else if (op_len == 2 && op[0] == '@' && strchr("QUuL", op[1])) {
        if (op[1] == 'Q') {
            apply.kind = SHELL_PARAM_OP_QUOTE;
        } else {
            apply.kind = SHELL_PARAM_OP_CASE;
            apply.upper = op[1] != 'L';
            apply.all = op[1] != 'u';
        }
    } else {
        shellParamSubjectFree(&subject);
        shellParamBadSubstitution(inner, inner_len);
        return strdup("");
    }

    char *result = NULL;
    if ((apply.kind == SHELL_PARAM_OP_QUOTE || apply.pattern) || apply.kind == SHELL_PARAM_OP_CASE) {
        if (subject.is_list) {
            for (size_t i = 0; i < subject.count; ++i) {
                char *changed = shellParamApplyOp(&apply, subject.items[i] ? subject.items[i] : "");
                if (changed) {
                    free(subject.items[i]);
                    subject.items[i] = changed;
                }
            }
            result = shellParamFinishList(&subject, in_double, out_array, out_count);
        } else {
            result = shellParamApplyOp(&apply, subject.value);
        }
    }
    free(pattern);
    free(replacement);
    shellParamSubjectFree(&subject);
    return result ? result : strdup("");
}

/* ~, ~/x, ~user, ~+, ~-. Any quoted character in the prefix leaves it alone. */
static char *shellTildeExpansion(const char *text, size_t text_len, bool in_assignment, size_t *out_consumed) {
    size_t end = 1;
    while (end < text_len && text[end] != '/' && !(in_assignment && text[end] == ':')) {
        char c = text[end];
        if (c == SHELL_QUOTE_MARK_SINGLE || c == SHELL_QUOTE_MARK_DOUBLE || c == '$' || c == '`' || c == '\\' ||
            c == SHELL_ARRAY_ELEMENT_SEP) {
            return NULL;
        }
        end++;
    }
    *out_consumed = end;
    size_t name_len = end - 1;
    const char *name = text + 1;
    if (name_len == 0) {
        const char *home = NULL;
        char *value = NULL;
        bool is_set = false;
        value = shellLookupParameterValueInternal("HOME", 4, &is_set);
        if (is_set && value) {
            return value;
        }
        free(value);
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
        return home ? strdup(home) : NULL;
    }
    if (name_len == 1 && (name[0] == '+' || name[0] == '-')) {
        bool is_set = false;
        const char *variable = name[0] == '+' ? "PWD" : "OLDPWD";
        char *value = shellLookupParameterValueInternal(variable, strlen(variable), &is_set);
        if (is_set && value) {
            return value;
        }
        free(value);
        return NULL;
    }
    char user[256];
    if (name_len >= sizeof(user)) {
        return NULL;
    }
    memcpy(user, name, name_len);
    user[name_len] = '\0';
    struct passwd *pw = getpwnam(user);
    return (pw && pw->pw_dir) ? strdup(pw->pw_dir) : NULL;
}

/* foo{a,b} and {1..3}: before anything else happens to a word, on its raw text with
 * its quote markers. The pieces it makes no longer line up with recorded substitutions,
 * so they go through the expander's inline parse. */

/* Skips what brace expansion must not look into. Returns the next offset to consider. */
static size_t shellBraceSkip(const char *s, size_t len, size_t i, bool *in_single, bool *in_double) {
    char c = s[i];
    if (c == SHELL_QUOTE_MARK_BYTE && i + 1 < len) {
        return i + 2;
    }
    if (c == SHELL_QUOTE_MARK_SINGLE && !*in_double) {
        *in_single = !*in_single;
        return i + 1;
    }
    if (c == SHELL_QUOTE_MARK_DOUBLE && !*in_single) {
        *in_double = !*in_double;
        return i + 1;
    }
    if (*in_single) {
        return i + 1;
    }
    if (c == '$' && i + 1 < len && (s[i + 1] == '{' || s[i + 1] == '(')) {
        size_t stop = shellScanDollar(s, len, i, *in_double);
        return stop == SHELL_SCAN_UNTERMINATED ? len : stop;
    }
    if (c == '`') {
        size_t stop = shellScanBacktick(s, len, i + 1);
        return stop == SHELL_SCAN_UNTERMINATED ? len : stop;
    }
    return i + 1;
}

static bool shellBraceParseInteger(const char *s, size_t len, long long *out, int *out_width) {
    if (len == 0) {
        return false;
    }
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        i = 1;
    }
    if (i == len) {
        return false;
    }
    int width = 0;
    bool padded = s[i] == '0' && len - i > 1;
    for (size_t j = i; j < len; ++j) {
        if (!isdigit((unsigned char)s[j])) {
            return false;
        }
    }
    if (padded) {
        width = (int)len;
    }
    char buffer[32];
    if (len >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, s, len);
    buffer[len] = '\0';
    *out = strtoll(buffer, NULL, 10);
    *out_width = width;
    return true;
}

/* {a..c} and {1..9..2}. Text with a marker or an expansion in it is no sequence. */
static bool shellBraceSequence(const char *s, size_t len, ShellStringArray *out) {
    const char *first_dots = NULL;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (s[i] == '.' && s[i + 1] == '.') {
            first_dots = s + i;
            break;
        }
    }
    if (!first_dots) {
        return false;
    }
    const char *lhs = s;
    size_t lhs_len = (size_t)(first_dots - s);
    const char *rhs = first_dots + 2;
    size_t rhs_len = len - lhs_len - 2;
    long long step = 0;
    for (size_t i = 0; i + 1 < rhs_len; ++i) {
        if (rhs[i] == '.' && rhs[i + 1] == '.') {
            int ignored = 0;
            if (!shellBraceParseInteger(rhs + i + 2, rhs_len - i - 2, &step, &ignored)) {
                return false;
            }
            rhs_len = i;
            break;
        }
    }
    long long from = 0;
    long long to = 0;
    int from_width = 0;
    int to_width = 0;
    bool numeric = shellBraceParseInteger(lhs, lhs_len, &from, &from_width) &&
                   shellBraceParseInteger(rhs, rhs_len, &to, &to_width);
    bool alpha = !numeric && lhs_len == 1 && rhs_len == 1 && isalpha((unsigned char)lhs[0]) &&
                 isalpha((unsigned char)rhs[0]);
    if (!numeric && !alpha) {
        return false;
    }
    if (alpha) {
        from = (unsigned char)lhs[0];
        to = (unsigned char)rhs[0];
    }
    if (step < 0) {
        step = -step;
    }
    if (step == 0) {
        step = 1;
    }
    int width = from_width > to_width ? from_width : to_width;
    /* Bash stops making words at some point too; this keeps a typo from eating memory. */
    if ((to > from ? to - from : from - to) / step > 10000000) {
        return false;
    }
    long long value = from;
    while (from <= to ? value <= to : value >= to) {
        char buffer[64];
        if (alpha) {
            buffer[0] = (char)value;
            buffer[1] = '\0';
        } else if (width > 0) {
            snprintf(buffer, sizeof(buffer), "%0*lld", width, value);
        } else {
            snprintf(buffer, sizeof(buffer), "%lld", value);
        }
        char *copy = strdup(buffer);
        if (!copy || !shellStringArrayAppend(out, copy)) {
            free(copy);
            return false;
        }
        value += from <= to ? step : -step;
    }
    return true;
}

static bool shellBraceExpandInto(const char *text, size_t len, ShellStringArray *out) {
    bool in_single = false;
    bool in_double = false;
    size_t i = 0;
    while (i < len) {
        if (text[i] != '{' || in_single || in_double) {
            i = shellBraceSkip(text, len, i, &in_single, &in_double);
            continue;
        }
        /* Find the closer and the commas that belong to this brace. */
        size_t open = i;
        int depth = 1;
        bool inner_single = false;
        bool inner_double = false;
        size_t j = i + 1;
        size_t *commas = NULL;
        size_t comma_count = 0;
        size_t comma_capacity = 0;
        size_t close = len;
        while (j < len) {
            char c = text[j];
            if (!inner_single && !inner_double) {
                if (c == '{') {
                    depth++;
                } else if (c == '}') {
                    if (--depth == 0) {
                        close = j;
                        break;
                    }
                } else if (c == ',' && depth == 1) {
                    if (comma_count == comma_capacity) {
                        comma_capacity = comma_capacity ? comma_capacity * 2 : 8;
                        size_t *grown = (size_t *)realloc(commas, comma_capacity * sizeof(size_t));
                        if (!grown) {
                            free(commas);
                            return false;
                        }
                        commas = grown;
                    }
                    commas[comma_count++] = j;
                }
            }
            j = shellBraceSkip(text, len, j, &inner_single, &inner_double);
        }
        if (close == len) {
            free(commas);
            i = open + 1;
            continue;
        }
        ShellStringArray alternatives = (ShellStringArray){0};
        bool has_alternatives = false;
        if (comma_count > 0) {
            has_alternatives = true;
            size_t start = open + 1;
            for (size_t k = 0; k <= comma_count; ++k) {
                size_t stop = k < comma_count ? commas[k] : close;
                if (!shellBraceExpandInto(text + start, stop - start, &alternatives)) {
                    free(commas);
                    shellStringArrayFree(&alternatives);
                    return false;
                }
                start = stop + 1;
            }
        } else {
            has_alternatives = shellBraceSequence(text + open + 1, close - open - 1, &alternatives);
        }
        free(commas);
        if (!has_alternatives) {
            shellStringArrayFree(&alternatives);
            i = open + 1;
            continue;
        }
        ShellStringArray tails = (ShellStringArray){0};
        if (!shellBraceExpandInto(text + close + 1, len - close - 1, &tails)) {
            shellStringArrayFree(&alternatives);
            shellStringArrayFree(&tails);
            return false;
        }
        for (size_t a = 0; a < alternatives.count; ++a) {
            for (size_t t = 0; t < tails.count; ++t) {
                size_t alt_len = strlen(alternatives.items[a]);
                size_t tail_len = strlen(tails.items[t]);
                char *word = (char *)malloc(open + alt_len + tail_len + 1);
                if (!word) {
                    shellStringArrayFree(&alternatives);
                    shellStringArrayFree(&tails);
                    return false;
                }
                memcpy(word, text, open);
                memcpy(word + open, alternatives.items[a], alt_len);
                memcpy(word + open + alt_len, tails.items[t], tail_len);
                word[open + alt_len + tail_len] = '\0';
                if (!shellStringArrayAppend(out, word)) {
                    free(word);
                    shellStringArrayFree(&alternatives);
                    shellStringArrayFree(&tails);
                    return false;
                }
            }
        }
        shellStringArrayFree(&alternatives);
        shellStringArrayFree(&tails);
        return true;
    }
    char *whole = (char *)malloc(len + 1);
    if (!whole) {
        return false;
    }
    memcpy(whole, text, len);
    whole[len] = '\0';
    if (!shellStringArrayAppend(out, whole)) {
        free(whole);
        return false;
    }
    return true;
}

/* NULL when the word has nothing to expand: the usual case, and it keeps its metadata. */
static bool shellBraceExpandWord(const char *text, ShellStringArray *out) {
    if (gShellRuntime.braceexpand_disabled || !strchr(text, '{') || !strchr(text, '}')) {
        return false;
    }
    if (!shellBraceExpandInto(text, strlen(text), out)) {
        return false;
    }
    if (out->count == 1 && strcmp(out->items[0], text) == 0) {
        shellStringArrayFree(out);
        *out = (ShellStringArray){0};
        return false;
    }
    return true;
}
