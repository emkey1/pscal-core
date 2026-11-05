/* Bash's arithmetic: C's integer operators over long long, for $((...)), ((...)), let,
 * declare -i and array subscripts. A variable's value is itself an expression. Included
 * from shell_word_expansion.inc. */

#define SHELL_ARITHMETIC_MAX_DEPTH 1024

typedef struct {
    const char *input;
    size_t length;
    size_t pos;
    size_t last_token; /* where the token read last starts */
    int noeval; /* inside the side of && || ?: that is not taken */
    int depth;
    const char *label; /* what a report starts with: "((", "let", "" for nothing */
    bool failed;
} ShellArithmeticParser;

static bool shellArithmeticParseComma(ShellArithmeticParser *parser, long long *out_value);
static bool shellArithmeticParseAssignment(ShellArithmeticParser *parser, long long *out_value);
static bool shellArithmeticEvaluateText(const char *text, int depth, long long *out_value, const char *label);

/* Bash's report: the expression this level is reading, up to `expr_end`, and from `token` to there. */
static bool shellArithmeticFailAt(ShellArithmeticParser *parser, const char *message, size_t token, size_t expr_end) {
    if (!parser->failed) {
        size_t start = 0;
        while (start < expr_end && (parser->input[start] == ' ' || parser->input[start] == '\t')) {
            start++;
        }
        shellReportRecoverableError(gShellCurrentVm, true, "%s%s%.*s: %s (error token is \"%.*s\")", parser->label,
                                    parser->label[0] ? ": " : "", (int)(expr_end - start), parser->input + start, message,
                                    (int)(expr_end - token), parser->input + token);
    }
    parser->failed = true;
    return false;
}

static bool shellArithmeticFail(ShellArithmeticParser *parser, const char *message) {
    return shellArithmeticFailAt(parser, message, parser->last_token, parser->length);
}

/* Every token comes after one of these, so this is where the last one read starts. At the end
 * there is no token, and the last one stands: `1+` names `+`. */
static void shellArithmeticSkipWhitespace(ShellArithmeticParser *parser) {
    while (parser->pos < parser->length && isspace((unsigned char)parser->input[parser->pos])) {
        parser->pos++;
    }
    if (parser->pos < parser->length) {
        parser->last_token = parser->pos;
    }
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
    /* A number is all of this to a report, which stops there: 1+09+1 says `1+09` and `09`. */
    size_t token = i;
    size_t token_end = i;
    while (token_end < end && (isalnum((unsigned char)s[token_end]) || s[token_end] == '#' || s[token_end] == '@' ||
                               s[token_end] == '_')) {
        token_end++;
    }
    while (i < end && isdigit((unsigned char)s[i])) {
        i++;
    }
    bool based = i < end && s[i] == '#';
    if (based) {
        long parsed = strtol(s + digits_start, NULL, 10);
        if (parsed < 2 || parsed > 64) {
            return shellArithmeticFailAt(parser, parsed == 0 ? "invalid number" : "invalid arithmetic base", token, token_end);
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
            return shellArithmeticFailAt(parser, "value too great for base", token, token_end);
        }
        value = value * (unsigned)base + (unsigned)digit;
        i++;
    }
    if (i == digits_start && based) {
        return shellArithmeticFailAt(parser, "invalid integer constant", token, token_end);
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

static bool shellArithmeticReadVariable(ShellArithmeticParser *parser, const ShellArithmeticLvalue *lvalue,
                                        long long *out_value) {
    if (parser->noeval > 0) {
        *out_value = 0;
        return true;
    }
    char *text = NULL;
    if (lvalue->subscript) {
        char *key = NULL;
        if (!shellParamSubscriptKeyChecked(lvalue->name, lvalue->name_len, lvalue->subscript, lvalue->subscript_len, &key)) {
            parser->failed = true;
            return false;
        }
        if (key) {
            text = shellExpandArraySubscriptValue(lvalue->name, lvalue->name_len, key, strlen(key), NULL, NULL);
            free(key);
        }
    } else {
        bool is_set = false;
        text = shellLookupParameterValueInternal(lvalue->name, lvalue->name_len, &is_set);
        if (!is_set && gShellRuntime.nounset_enabled) {
            free(text);
            /* It ends a script from any command, with no expression and no label. */
            shellReportRecoverableError(gShellCurrentVm, true, "%.*s: unbound variable", (int)lvalue->name_len, lvalue->name);
            shellExpansionFail();
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
    bool ok = shellArithmeticEvaluateText(text, parser->depth + 1, out_value, parser->label);
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
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", value);
    char *key = NULL;
    if (lvalue->subscript) {
        if (!shellParamSubscriptKeyChecked(lvalue->name, lvalue->name_len, lvalue->subscript, lvalue->subscript_len, &key)) {
            parser->failed = true;
            return false;
        }
        if (!key) {
            return shellArithmeticFail(parser, "bad array subscript");
        }
    }
    char *name = strndup(lvalue->name, lvalue->name_len);
    if (!name) {
        free(key);
        parser->failed = true;
        return false;
    }
    bool ok = key ? shellArrayRegistrySetElement(name, key, buffer) : shellSetTrackedVariable(name, buffer, false);
    free(key);
    if (!ok) {
        /* No expression and no label either. */
        shellReportRecoverableError(gShellCurrentVm, true, "%s: readonly variable", name);
        parser->failed = true;
    }
    free(name);
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
            if (!shellArithmeticStoreVariable(parser, &lvalue, value)) {
                return false;
            }
            shellArithmeticSkipWhitespace(parser);
            if (shellArithmeticPeek(parser, "++")) {
                return shellArithmeticFail(parser, "++: assignment requires lvalue");
            }
            if (shellArithmeticPeek(parser, "--")) {
                return shellArithmeticFail(parser, "--: assignment requires lvalue");
            }
            return true;
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
    bool after_paren = parser->input[parser->last_token] == ')';
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos < parser->length) {
        unsigned char c = (unsigned char)parser->input[parser->pos];
        if (c < 0x80 && !isalnum(c) && c != '_' && !strchr("+-*/%<>=!&|^~?:,()", c)) {
            shellArithmeticFail(parser, after_paren ? "syntax error: operand expected" : "syntax error: invalid arithmetic operator");
            return NULL;
        }
    }
    for (size_t i = 0; i < sizeof(gShellArithmeticOps) / sizeof(gShellArithmeticOps[0]); ++i) {
        const ShellArithmeticBinaryOp *op = &gShellArithmeticOps[i];
        if (!shellArithmeticPeek(parser, op->text)) {
            continue;
        }
        /* `a += 1` belongs to the assignment level above. There is no **=, &&= or ||=. */
        size_t n = strlen(op->text);
        if (op->precedence != 6 && op->precedence != 11 && op->precedence > 2 && !(op->text[0] == '<' && op->text[1] == '=') &&
            !(op->text[0] == '>' && op->text[1] == '=') && parser->pos + n < parser->length &&
            parser->input[parser->pos + n] == '=') {
            return NULL;
        }
        return op;
    }
    return NULL;
}

static bool shellArithmeticApply(ShellArithmeticParser *parser, const char *op, long long lhs, long long rhs,
                                 size_t divisor, long long *out_value) {
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
                return shellArithmeticFailAt(parser, "division by 0", divisor, parser->length);
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
        if (parser->failed) {
            return false;
        }
        if (!op || op->precedence < min_precedence) {
            break;
        }
        parser->pos += strlen(op->text);
        /* Bash 5 names all that is left from the divisor on: 1/0+1 says `0+1`. Blanks go, a newline stays. */
        size_t operand = parser->pos;
        while (operand < parser->length && (parser->input[operand] == ' ' || parser->input[operand] == '\t')) {
            operand++;
        }
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
        } else if (!shellArithmeticApply(parser, op->text, lhs, rhs, operand, &lhs)) {
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
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length || parser->input[parser->pos] == ':') {
        return shellArithmeticFail(parser, "expression expected");
    }
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
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return shellArithmeticFail(parser, "expression expected");
    }
    if (condition) {
        parser->noeval++;
    }
    /* Not an assignment: a?b:c=1 is (a?b:c)=1, which is an error. */
    ok = shellArithmeticParseTernary(parser, &when_false);
    if (condition) {
        parser->noeval--;
    }
    if (!ok) {
        return false;
    }
    *out_value = condition ? when_true : when_false;
    return true;
}

/* The assignment operator at the cursor, compound or not. */
static bool shellArithmeticPeekAssignment(const ShellArithmeticParser *parser, const char **out_op) {
    static const char *const kOps[] = {"<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|="};
    *out_op = NULL;
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
        if (shellArithmeticPeek(parser, kOps[i])) {
            *out_op = kOps[i];
            return true;
        }
    }
    return shellArithmeticPeek(parser, "=") && !shellArithmeticPeek(parser, "==");
}

static bool shellArithmeticParseAssignment(ShellArithmeticParser *parser, long long *out_value) {
    shellArithmeticSkipWhitespace(parser);
    size_t saved = parser->pos;
    ShellArithmeticLvalue lvalue;
    if (shellArithmeticParseLvalue(parser, &lvalue)) {
        shellArithmeticSkipWhitespace(parser);
        const char *op = NULL;
        if (shellArithmeticPeekAssignment(parser, &op)) {
            bool plain = !op;
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
                if (!shellArithmeticApply(parser, binary, current, rhs, parser->last_token, &value)) {
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
    if (!shellArithmeticParseTernary(parser, out_value)) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    const char *op = NULL;
    if (shellArithmeticPeekAssignment(parser, &op)) {
        return shellArithmeticFail(parser, "attempted assignment to non-variable");
    }
    return true;
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

/* Text with its expansions already done. Empty text is 0. What goes wrong is reported under
 * `label`: "((" or "let" for `((: 1/0: division by 0`, and "" for no label. */
static bool shellArithmeticEvaluateText(const char *text, int depth, long long *out_value, const char *label) {
    ShellArithmeticParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.input = text ? text : "";
    parser.length = strlen(parser.input);
    parser.depth = depth;
    parser.label = label;
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

/* An expression as written, without its double quotes, which mean nothing here. Those in a
 * substitution are its own, and those in a value are an error when the value is read. NULL if there
 * are none to drop. */
static char *shellArithmeticDropQuotes(const char *expr) {
    if (!strchr(expr, '"')) {
        return NULL;
    }
    size_t len = strlen(expr);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < len; ) {
        size_t stop = i + 1;
        if (expr[i] == '$') {
            stop = shellScanDollar(expr, len, i, true);
        } else if (expr[i] == '`') {
            stop = shellScanBacktick(expr, len, i + 1);
        } else if (expr[i] == '"') {
            i++;
            continue;
        }
        if (stop == SHELL_SCAN_UNTERMINATED) {
            stop = len;
        }
        memcpy(out + used, expr + i, stop - i);
        used += stop - i;
        i = stop;
    }
    out[used] = '\0';
    return out;
}

/* An expression as written: `$x` and `$(cmd)` first, as text. The flag keeps `~1` from
 * being taken for a tilde and treats backslashes as in double quotes. */
static bool shellArithmeticEvaluate(const char *expr, long long *out_value, const char *label) {
    if (!expr) {
        *out_value = 0;
        return true;
    }
    char *unquoted = shellArithmeticDropQuotes(expr);
    const char *text = unquoted ? unquoted : expr;
    char *expanded = NULL;
    if (strpbrk(text, "$`")) {
        bool failed_before = gShellExpansionFailed;
        gShellExpansionFailed = false;
        expanded = shellExpandWord(text, SHELL_WORD_FLAG_HERE_DOCUMENT, NULL, 0, NULL, NULL, NULL);
        bool failed = gShellExpansionFailed;
        gShellExpansionFailed = failed_before || failed;
        /* It has said why, and there is no expression to read: $(($((1/0))+1)). */
        if (!expanded || failed) {
            *out_value = 0;
            free(expanded);
            free(unquoted);
            return false;
        }
        text = expanded;
    }
    bool ok = shellArithmeticEvaluateText(text, 0, out_value, label);
    free(expanded);
    free(unquoted);
    return ok;
}

static bool shellLetEvaluateExpression(const char *text, long long *out_value) {
    if (!shellArithmeticEvaluateText(text, 0, out_value, "let")) {
        shellMarkArithmeticError();
        return false;
    }
    return true;
}
