/* Bash's arithmetic: C's integer operators over long long, for $((...)), ((...)), let,
 * declare -i and array subscripts. A variable's value is itself an expression. Included
 * from shell_word_expansion.inc. */

#define SHELL_ARITHMETIC_MAX_DEPTH 1024

typedef struct {
    const char *input;
    size_t length;
    size_t pos;
    int noeval; /* inside the side of && || ?: that is not taken */
    int depth;
    char *error;
    size_t error_size;
    bool failed;
} ShellArithmeticParser;

static bool shellArithmeticParseComma(ShellArithmeticParser *parser, long long *out_value);
static bool shellArithmeticParseAssignment(ShellArithmeticParser *parser, long long *out_value);
static bool shellArithmeticEvaluateText(const char *text, int depth, long long *out_value, char *error, size_t error_size);

static bool shellArithmeticFail(ShellArithmeticParser *parser, const char *message) {
    if (!parser->failed && parser->error && parser->error_size > 0) {
        const char *rest = parser->pos < parser->length ? parser->input + parser->pos : "";
        snprintf(parser->error, parser->error_size, "%s (error token is \"%s\")", message, rest);
    }
    parser->failed = true;
    return false;
}

static void shellArithmeticSkipWhitespace(ShellArithmeticParser *parser) {
    /* Double quotes mean nothing here and bash drops them: $(("1" + 1)). */
    while (parser->pos < parser->length &&
           (isspace((unsigned char)parser->input[parser->pos]) || parser->input[parser->pos] == '"')) {
        parser->pos++;
    }
}

static bool shellArithmeticParseValueString(const char *text, long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *endptr = NULL;
    long long value = strtoll(text, &endptr, 0);
    if (errno != 0) {
        return false;
    }
    if (endptr && *endptr != '\0') {
        while (endptr && *endptr && isspace((unsigned char)*endptr)) {
            endptr++;
        }
        if (endptr && *endptr != '\0') {
            return false;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticPeek(const ShellArithmeticParser *parser, const char *op) {
    size_t n = strlen(op);
    return parser->pos + n <= parser->length && memcmp(parser->input + parser->pos, op, n) == 0;
}

static int shellArithmeticDigitValue(char c, int base) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return base <= 36 ? c - 'A' + 10 : c - 'A' + 36;
    }
    if (c == '@') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

/* 12, 0x1f, 017, 2#101, 64#_ */
static bool shellArithmeticParseNumber(ShellArithmeticParser *parser, long long *out_value) {
    const char *s = parser->input;
    size_t i = parser->pos;
    size_t end = parser->length;
    int base = 10;
    unsigned long long value = 0;
    size_t digits_start = i;
    while (i < end && isdigit((unsigned char)s[i])) {
        i++;
    }
    if (i < end && s[i] == '#') {
        long parsed = strtol(s + digits_start, NULL, 10);
        if (parsed < 2 || parsed > 64) {
            return shellArithmeticFail(parser, "invalid arithmetic base");
        }
        base = (int)parsed;
        i++;
        digits_start = i;
    } else if (end - digits_start >= 2 && s[digits_start] == '0' && (s[digits_start + 1] == 'x' || s[digits_start + 1] == 'X')) {
        base = 16;
        digits_start += 2;
    } else if (s[digits_start] == '0') {
        base = 8;
    }
    i = digits_start;
    while (i < end && (isalnum((unsigned char)s[i]) || s[i] == '@' || s[i] == '_')) {
        int digit = shellArithmeticDigitValue(s[i], base);
        if (digit < 0 || digit >= base) {
            parser->pos = digits_start;
            return shellArithmeticFail(parser, "value too great for base");
        }
        value = value * (unsigned)base + (unsigned)digit;
        i++;
    }
    if (i == digits_start && base != 8) {
        return shellArithmeticFail(parser, "invalid number");
    }
    parser->pos = i;
    *out_value = (long long)value;
    return true;
}

typedef struct {
    const char *name;
    size_t name_len;
    const char *subscript;
    size_t subscript_len;
} ShellArithmeticLvalue;

/* name or name[subscript] at the cursor. */
static bool shellArithmeticParseLvalue(ShellArithmeticParser *parser, ShellArithmeticLvalue *out) {
    size_t name_len = 0;
    if (parser->pos >= parser->length ||
        !consumeShellIdentifier(parser->input + parser->pos, parser->length - parser->pos, &name_len, false)) {
        return false;
    }
    out->name = parser->input + parser->pos;
    out->name_len = name_len;
    out->subscript = NULL;
    out->subscript_len = 0;
    parser->pos += name_len;
    if (parser->pos < parser->length && parser->input[parser->pos] == '[') {
        size_t i = parser->pos + 1;
        int depth = 1;
        while (i < parser->length && depth > 0) {
            if (parser->input[i] == '[') {
                depth++;
            } else if (parser->input[i] == ']') {
                depth--;
            }
            i++;
        }
        if (depth != 0) {
            return shellArithmeticFail(parser, "bad array subscript");
        }
        out->subscript = parser->input + parser->pos + 1;
        out->subscript_len = i - parser->pos - 2;
        parser->pos = i;
    }
    return true;
}

static char *shellParamSubscriptKey(const char *name, size_t name_len, const char *subscript, size_t subscript_len);

static bool shellArithmeticReadVariable(ShellArithmeticParser *parser, const ShellArithmeticLvalue *lvalue,
                                        long long *out_value) {
    if (parser->noeval > 0) {
        *out_value = 0;
        return true;
    }
    char *text = NULL;
    if (lvalue->subscript) {
        char *key = shellParamSubscriptKey(lvalue->name, lvalue->name_len, lvalue->subscript, lvalue->subscript_len);
        if (key) {
            text = shellExpandArraySubscriptValue(lvalue->name, lvalue->name_len, key, strlen(key), NULL, NULL);
            free(key);
        }
    } else {
        bool is_set = false;
        text = shellLookupParameterValueInternal(lvalue->name, lvalue->name_len, &is_set);
        if (!is_set && gShellRuntime.nounset_enabled) {
            free(text);
            if (parser->error && parser->error_size > 0) {
                snprintf(parser->error, parser->error_size, "%.*s: unbound variable",
                         (int)lvalue->name_len, lvalue->name);
            }
            parser->failed = true;
            return false;
        }
    }
    if (!text) {
        *out_value = 0;
        return true;
    }
    if (parser->depth >= SHELL_ARITHMETIC_MAX_DEPTH) {
        free(text);
        return shellArithmeticFail(parser, "expression recursion level exceeded");
    }
    bool ok = shellArithmeticEvaluateText(text, parser->depth + 1, out_value, parser->error, parser->error_size);
    free(text);
    if (!ok) {
        parser->failed = true;
    }
    return ok;
}

static bool shellArithmeticStoreVariable(ShellArithmeticParser *parser, const ShellArithmeticLvalue *lvalue,
                                         long long value) {
    if (parser->noeval > 0) {
        return true;
    }
    char name[256];
    if (lvalue->name_len >= sizeof(name)) {
        return shellArithmeticFail(parser, "name too long");
    }
    memcpy(name, lvalue->name, lvalue->name_len);
    name[lvalue->name_len] = '\0';
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    bool ok;
    if (lvalue->subscript) {
        char *key = shellParamSubscriptKey(lvalue->name, lvalue->name_len, lvalue->subscript, lvalue->subscript_len);
        if (!key) {
            return shellArithmeticFail(parser, "bad array subscript");
        }
        ok = shellArrayRegistrySetElement(name, key, buffer);
        free(key);
    } else {
        ok = shellSetTrackedVariable(name, buffer, false);
    }
    if (!ok) {
        if (parser->error && parser->error_size > 0) {
            snprintf(parser->error, parser->error_size, "%s: readonly variable", name);
        }
        parser->failed = true;
    }
    return ok;
}

static bool shellArithmeticParsePrimary(ShellArithmeticParser *parser, long long *out_value) {
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return shellArithmeticFail(parser, "syntax error: operand expected");
    }
    char c = parser->input[parser->pos];
    if (c == '(') {
        parser->pos++;
        if (!shellArithmeticParseComma(parser, out_value)) {
            return false;
        }
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length || parser->input[parser->pos] != ')') {
            return shellArithmeticFail(parser, "missing `)'");
        }
        parser->pos++;
        return true;
    }
    if (isdigit((unsigned char)c)) {
        return shellArithmeticParseNumber(parser, out_value);
    }
    ShellArithmeticLvalue lvalue;
    if (shellArithmeticParseLvalue(parser, &lvalue)) {
        if (!shellArithmeticReadVariable(parser, &lvalue, out_value)) {
            return false;
        }
        shellArithmeticSkipWhitespace(parser);
        bool increment = shellArithmeticPeek(parser, "++");
        bool decrement = shellArithmeticPeek(parser, "--");
        if (increment || decrement) {
            parser->pos += 2;
            return shellArithmeticStoreVariable(parser, &lvalue, *out_value + (increment ? 1 : -1));
        }
        return true;
    }
    if (parser->failed) {
        return false;
    }
    return shellArithmeticFail(parser, "syntax error: operand expected");
}

static bool shellArithmeticParseUnary(ShellArithmeticParser *parser, long long *out_value) {
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return shellArithmeticFail(parser, "syntax error: operand expected");
    }
    if (shellArithmeticPeek(parser, "++") || shellArithmeticPeek(parser, "--")) {
        bool increment = parser->input[parser->pos] == '+';
        size_t saved = parser->pos;
        parser->pos += 2;
        shellArithmeticSkipWhitespace(parser);
        ShellArithmeticLvalue lvalue;
        if (shellArithmeticParseLvalue(parser, &lvalue)) {
            long long value = 0;
            if (!shellArithmeticReadVariable(parser, &lvalue, &value)) {
                return false;
            }
            value += increment ? 1 : -1;
            *out_value = value;
            return shellArithmeticStoreVariable(parser, &lvalue, value);
        }
        if (parser->failed) {
            return false;
        }
        parser->pos = saved;
    }
    char c = parser->input[parser->pos];
    if (c == '+' || c == '-' || c == '!' || c == '~') {
        parser->pos++;
        long long value = 0;
        if (!shellArithmeticParseUnary(parser, &value)) {
            return false;
        }
        switch (c) {
            case '-': *out_value = (long long)(0ULL - (unsigned long long)value); break;
            case '!': *out_value = !value; break;
            case '~': *out_value = ~value; break;
            default: *out_value = value; break;
        }
        return true;
    }
    return shellArithmeticParsePrimary(parser, out_value);
}

typedef struct {
    const char *text;
    int precedence;
} ShellArithmeticBinaryOp;

/* Longest first. */
static const ShellArithmeticBinaryOp gShellArithmeticOps[] = {
    {"||", 1}, {"&&", 2}, {"==", 6}, {"!=", 6}, {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"**", 11},
    {"|", 3},  {"^", 4},  {"&", 5},  {"<", 7},  {">", 7},  {"+", 9},  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10},
};

static const ShellArithmeticBinaryOp *shellArithmeticPeekBinaryOp(ShellArithmeticParser *parser) {
    shellArithmeticSkipWhitespace(parser);
    for (size_t i = 0; i < sizeof(gShellArithmeticOps) / sizeof(gShellArithmeticOps[0]); ++i) {
        const ShellArithmeticBinaryOp *op = &gShellArithmeticOps[i];
        if (!shellArithmeticPeek(parser, op->text)) {
            continue;
        }
        /* `a += 1` belongs to the assignment level above. */
        size_t n = strlen(op->text);
        if (op->precedence != 6 && !(op->text[0] == '<' && op->text[1] == '=') &&
            !(op->text[0] == '>' && op->text[1] == '=') && parser->pos + n < parser->length &&
            parser->input[parser->pos + n] == '=') {
            return NULL;
        }
        return op;
    }
    return NULL;
}

static bool shellArithmeticApply(ShellArithmeticParser *parser, const char *op, long long lhs, long long rhs,
                                 long long *out_value) {
    unsigned long long ul = (unsigned long long)lhs;
    unsigned long long ur = (unsigned long long)rhs;
    switch (op[0]) {
        case '+': *out_value = (long long)(ul + ur); return true;
        case '-': *out_value = (long long)(ul - ur); return true;
        case '^': *out_value = lhs ^ rhs; return true;
        case '|': *out_value = lhs | rhs; return true;
        case '&': *out_value = lhs & rhs; return true;
        case '*':
            if (op[1] == '*') {
                if (rhs < 0) {
                    return shellArithmeticFail(parser, "exponent less than 0");
                }
                unsigned long long result = 1;
                unsigned long long base = ul;
                unsigned long long exponent = ur;
                while (exponent > 0) {
                    if (exponent & 1) {
                        result *= base;
                    }
                    base *= base;
                    exponent >>= 1;
                }
                *out_value = (long long)result;
                return true;
            }
            *out_value = (long long)(ul * ur);
            return true;
        case '/':
        case '%':
            if (rhs == 0) {
                if (parser->noeval > 0) {
                    *out_value = 0;
                    return true;
                }
                return shellArithmeticFail(parser, "division by 0");
            }
            if (rhs == -1) {
                /* LLONG_MIN / -1 traps. */
                *out_value = op[0] == '/' ? (long long)(0ULL - ul) : 0;
                return true;
            }
            *out_value = op[0] == '/' ? lhs / rhs : lhs % rhs;
            return true;
        case '<':
            if (op[1] == '<') {
                *out_value = (long long)(ul << (ur & 63));
            } else if (op[1] == '=') {
                *out_value = lhs <= rhs;
            } else {
                *out_value = lhs < rhs;
            }
            return true;
        case '>':
            if (op[1] == '>') {
                *out_value = lhs >> (ur & 63);
            } else if (op[1] == '=') {
                *out_value = lhs >= rhs;
            } else {
                *out_value = lhs > rhs;
            }
            return true;
        case '=': *out_value = lhs == rhs; return true;
        case '!': *out_value = lhs != rhs; return true;
        default:
            return shellArithmeticFail(parser, "syntax error");
    }
}

static bool shellArithmeticParseBinary(ShellArithmeticParser *parser, int min_precedence, long long *out_value) {
    long long lhs = 0;
    if (!shellArithmeticParseUnary(parser, &lhs)) {
        return false;
    }
    while (true) {
        const ShellArithmeticBinaryOp *op = shellArithmeticPeekBinaryOp(parser);
        if (!op || op->precedence < min_precedence) {
            break;
        }
        parser->pos += strlen(op->text);
        long long rhs = 0;
        bool logical_or = op->precedence == 1;
        bool logical_and = op->precedence == 2;
        bool skip = (logical_or && lhs != 0) || (logical_and && lhs == 0);
        if (skip) {
            parser->noeval++;
        }
        /* Everything is left associative but `**`. */
        int next = op->precedence == 11 ? op->precedence : op->precedence + 1;
        bool ok = shellArithmeticParseBinary(parser, next, &rhs);
        if (skip) {
            parser->noeval--;
        }
        if (!ok) {
            return false;
        }
        if (logical_or) {
            lhs = lhs != 0 || rhs != 0;
        } else if (logical_and) {
            lhs = lhs != 0 && rhs != 0;
        } else if (!shellArithmeticApply(parser, op->text, lhs, rhs, &lhs)) {
            return false;
        }
    }
    *out_value = lhs;
    return true;
}

static bool shellArithmeticParseTernary(ShellArithmeticParser *parser, long long *out_value) {
    long long condition = 0;
    if (!shellArithmeticParseBinary(parser, 1, &condition)) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length || parser->input[parser->pos] != '?') {
        *out_value = condition;
        return true;
    }
    parser->pos++;
    long long when_true = 0;
    long long when_false = 0;
    if (!condition) {
        parser->noeval++;
    }
    bool ok = shellArithmeticParseComma(parser, &when_true);
    if (!condition) {
        parser->noeval--;
    }
    if (!ok) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length || parser->input[parser->pos] != ':') {
        return shellArithmeticFail(parser, "`:' expected for conditional expression");
    }
    parser->pos++;
    if (condition) {
        parser->noeval++;
    }
    ok = shellArithmeticParseAssignment(parser, &when_false);
    if (condition) {
        parser->noeval--;
    }
    if (!ok) {
        return false;
    }
    *out_value = condition ? when_true : when_false;
    return true;
}

static bool shellArithmeticParseAssignment(ShellArithmeticParser *parser, long long *out_value) {
    static const char *const kOps[] = {"<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|="};
    shellArithmeticSkipWhitespace(parser);
    size_t saved = parser->pos;
    ShellArithmeticLvalue lvalue;
    if (shellArithmeticParseLvalue(parser, &lvalue)) {
        shellArithmeticSkipWhitespace(parser);
        const char *op = NULL;
        for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
            if (shellArithmeticPeek(parser, kOps[i])) {
                op = kOps[i];
                break;
            }
        }
        bool plain = !op && shellArithmeticPeek(parser, "=") && !shellArithmeticPeek(parser, "==");
        if (op || plain) {
            parser->pos += plain ? 1 : strlen(op);
            long long rhs = 0;
            if (!shellArithmeticParseAssignment(parser, &rhs)) {
                return false;
            }
            long long value = rhs;
            if (op) {
                long long current = 0;
                if (!shellArithmeticReadVariable(parser, &lvalue, &current)) {
                    return false;
                }
                char binary[3] = {op[0], strlen(op) == 3 ? op[1] : '\0', '\0'};
                if (!shellArithmeticApply(parser, binary, current, rhs, &value)) {
                    return false;
                }
            }
            *out_value = value;
            return shellArithmeticStoreVariable(parser, &lvalue, value);
        }
    }
    if (parser->failed) {
        return false;
    }
    parser->pos = saved;
    return shellArithmeticParseTernary(parser, out_value);
}

static bool shellArithmeticParseComma(ShellArithmeticParser *parser, long long *out_value) {
    if (!shellArithmeticParseAssignment(parser, out_value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length || parser->input[parser->pos] != ',') {
            return true;
        }
        parser->pos++;
        if (!shellArithmeticParseAssignment(parser, out_value)) {
            return false;
        }
    }
}

/* Text with its expansions already done. Empty text is 0. */
static bool shellArithmeticEvaluateText(const char *text, int depth, long long *out_value, char *error, size_t error_size) {
    ShellArithmeticParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.input = text ? text : "";
    parser.length = strlen(parser.input);
    parser.depth = depth;
    parser.error = error;
    parser.error_size = error_size;
    *out_value = 0;
    shellArithmeticSkipWhitespace(&parser);
    if (parser.pos >= parser.length) {
        return true;
    }
    if (!shellArithmeticParseComma(&parser, out_value)) {
        return false;
    }
    shellArithmeticSkipWhitespace(&parser);
    if (parser.pos < parser.length) {
        return shellArithmeticFail(&parser, "syntax error in expression");
    }
    return true;
}

/* An expression as written: `$x` and `$(cmd)` first, as text. The flag keeps `~1` from
 * being taken for a tilde and treats backslashes as in double quotes. */
static bool shellArithmeticEvaluate(const char *expr, long long *out_value, char *error, size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!expr) {
        *out_value = 0;
        return true;
    }
    if (!strpbrk(expr, "$`")) {
        return shellArithmeticEvaluateText(expr, 0, out_value, error, error_size);
    }
    char *expanded = shellExpandWord(expr, SHELL_WORD_FLAG_HERE_DOCUMENT, NULL, 0, NULL, NULL, NULL);
    if (!expanded) {
        return false;
    }
    bool ok = shellArithmeticEvaluateText(expanded, 0, out_value, error, error_size);
    free(expanded);
    return ok;
}

static char *shellEvaluateArithmetic(const char *expr, bool *out_error) {
    if (out_error) {
        *out_error = false;
    }
    long long value = 0;
    if (!expr || !shellArithmeticEvaluate(expr, &value, NULL, 0)) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    char *result = strdup(buffer);
    if (!result && out_error) {
        *out_error = true;
    }
    return result;
}

static bool shellLetEvaluateExpression(VM *vm, const char *text, long long *out_value) {
    char error[256];
    if (!shellArithmeticEvaluate(text, out_value, error, sizeof(error))) {
        if (error[0]) {
            shellReportRecoverableError(vm, true, "let: %s: %s", text ? text : "", error);
        }
        shellMarkArithmeticError();
        return false;
    }
    return true;
}
