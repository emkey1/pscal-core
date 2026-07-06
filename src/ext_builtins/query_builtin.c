#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "ext_builtins/registry.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *kDefaultGroupName = "default";
static size_t countFunctionsAcrossGroups(const char *category);
static const char *getFunctionNameAcrossGroups(const char *category,
                                               size_t index,
                                               const char **out_group);

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} JsonText;

typedef struct {
  const char *aether_name;
  const char *backend_name;
  const char *kind;
  const char *return_type;
  const char *signature;
  const char *usage;
  const char *category;
  bool effectful;          /* informational; emit derives from pscalBuiltinNameIsEffectful() */
  const char *source;      /* NULL => "aether_alias"; raw VM builtins set "vm_builtin" */
} AetherBuiltinMeta;

static const AetherBuiltinMeta kAetherBuiltinMeta[] = {
    {"toon_parse", "YyjsonRead", "function", "ToonDoc",
     "toon_parse(payload: Text) -> ToonDoc",
     "Parse TOON/JSON text into a document handle.", "toon", true},
    {"toon_parse_file", "YyjsonReadFile", "function", "ToonDoc",
     "toon_parse_file(path: Text) -> ToonDoc",
     "Parse TOON/JSON from a file path into a document handle.", "toon", true},
    {"toon_root", "YyjsonGetRoot", "function", "ToonNode",
     "toon_root(doc: ToonDoc) -> ToonNode",
     "Get the root node from a TOON document handle.", "toon", false},
    {"toon_close", "YyjsonDocFree", "procedure", "Void",
     "toon_close(doc: ToonDoc) -> Void",
     "Release a TOON document handle.", "toon", true},
    {"toon_key", "YyjsonGetKey", "function", "ToonNode",
     "toon_key(node: ToonNode, key: Text) -> ToonNode",
     "Read an object child node by key.", "toon", false},
    {"toon_key_or", NULL, "function", "ToonNode",
     "toon_key_or(node: ToonNode, key: Text, fallback: ToonNode) -> ToonNode",
     "Read an object child node by key with a node fallback.", "toon", false},
    {"toon_null", NULL, "function", "ToonNode",
     "toon_null() -> ToonNode",
     "Construct a null ToonNode (toon_is_null is true, toon_len is 0).", "toon", false},
    {"toon_has_key", "YyjsonHasKey", "function", "Bool",
     "toon_has_key(node: ToonNode, key: Text) -> Bool",
     "Check whether an object node has a key.", "toon", false},
    {"toon_at", "YyjsonGetIndex", "function", "ToonNode",
     "toon_at(node: ToonNode, index: Int) -> ToonNode",
     "Read an array child node by index.", "toon", false},
    {"toon_has_at", "YyjsonHasIndex", "function", "Bool",
     "toon_has_at(node: ToonNode, index: Int) -> Bool",
     "Check whether an array node has an index.", "toon", false},
    {"toon_len", "YyjsonGetLength", "function", "Int",
     "toon_len(node: ToonNode) -> Int",
     "Length of a TOON array or object node.", "toon", false},
    {"toon_free", "YyjsonFreeValue", "procedure", "Void",
     "toon_free(node: ToonNode) -> Void",
     "Release one intermediate TOON node handle early.", "toon", true},
    {"toon_type", "YyjsonGetType", "function", "Text",
     "toon_type(node: ToonNode) -> Text",
     "Return the TOON node kind as text.", "toon", false},
    {"toon_text_value", "YyjsonGetString", "function", "Text",
     "toon_text_value(node: ToonNode) -> Text",
     "Read the current node as text.", "toon", false},
    {"toon_int_value", "YyjsonGetInt", "function", "Int",
     "toon_int_value(node: ToonNode) -> Int",
     "Read the current node as Int.", "toon", false},
    {"toon_real_value", "YyjsonGetNumber", "function", "Real",
     "toon_real_value(node: ToonNode) -> Real",
     "Read the current node as Real.", "toon", false},
    {"toon_bool_value", "YyjsonGetBool", "function", "Bool",
     "toon_bool_value(node: ToonNode) -> Bool",
     "Read the current node as Bool.", "toon", false},
    {"toon_null_value", "YyjsonIsNull", "function", "Bool",
     "toon_null_value(node: ToonNode) -> Bool",
     "Check whether the current node is null.", "toon", false},
    {"toon_get_text", NULL, "function", "Text",
     "toon_get_text(node: ToonNode, key: Text) -> Text",
     "Read a keyed child text value.", "toon", false},
    {"toon_get_text_or", NULL, "function", "Text",
     "toon_get_text_or(node: ToonNode, key: Text, fallback: Text) -> Text",
     "Read a keyed child text value with fallback.", "toon", false},
    {"toon_get_int", NULL, "function", "Int",
     "toon_get_int(node: ToonNode, key: Text) -> Int",
     "Read a keyed child Int value.", "toon", false},
    {"toon_get_int_or", NULL, "function", "Int",
     "toon_get_int_or(node: ToonNode, key: Text, fallback: Int) -> Int",
     "Read a keyed child Int value with fallback.", "toon", false},
    {"toon_get_real", NULL, "function", "Real",
     "toon_get_real(node: ToonNode, key: Text) -> Real",
     "Read a keyed child Real value.", "toon", false},
    {"toon_get_real_or", NULL, "function", "Real",
     "toon_get_real_or(node: ToonNode, key: Text, fallback: Real) -> Real",
     "Read a keyed child Real value with fallback.", "toon", false},
    {"toon_get_bool", NULL, "function", "Bool",
     "toon_get_bool(node: ToonNode, key: Text) -> Bool",
     "Read a keyed child Bool value.", "toon", false},
    {"toon_get_bool_or", NULL, "function", "Bool",
     "toon_get_bool_or(node: ToonNode, key: Text, fallback: Bool) -> Bool",
     "Read a keyed child Bool value with fallback.", "toon", false},
    {"toon_is_text", NULL, "function", "Bool", "toon_is_text(node: ToonNode) -> Bool",
     "Check whether a node is text.", "toon", false},
    {"toon_is_int", NULL, "function", "Bool", "toon_is_int(node: ToonNode) -> Bool",
     "Check whether a node is an integer.", "toon", false},
    {"toon_is_real", NULL, "function", "Bool", "toon_is_real(node: ToonNode) -> Bool",
     "Check whether a node is a real number.", "toon", false},
    {"toon_is_bool", NULL, "function", "Bool", "toon_is_bool(node: ToonNode) -> Bool",
     "Check whether a node is boolean.", "toon", false},
    {"toon_is_null", NULL, "function", "Bool", "toon_is_null(node: ToonNode) -> Bool",
     "Check whether a node is null.", "toon", false},
    {"toon_is_arr", NULL, "function", "Bool", "toon_is_arr(node: ToonNode) -> Bool",
     "Check whether a node is an array.", "toon", false},
    {"toon_is_obj", NULL, "function", "Bool", "toon_is_obj(node: ToonNode) -> Bool",
     "Check whether a node is an object.", "toon", false},
    {"print", "write", "procedure", "Void", "print(args...) -> Void",
     "Effectful console/file output; call inside fx.", "io", true},
    {"println", "writeln", "procedure", "Void", "println(args...) -> Void",
     "Effectful console/file output with newline; call inside fx.", "io", true},
    {"sleep", "delay", "procedure", "Void", "sleep(ms: Int) -> Void",
     "Blocking millisecond pause; effectful and must stay inside fx.", "time", true},
    {"string_len", "length", "function", "Int", "string_len(text: Text) -> Int",
     "Length of a Text value.", "text", false},
    {"len", "length", "function", "Int", "len(arrayOrText) -> Int",
     "Compact alias over length(...). Prefer length(...) in generated code unless brevity matters.", "collection", false},
    {"task_spawn", "thread_spawn_named", "function", "Int",
     "task_spawn(target: Text, name: Text, arg) -> Int",
     "Spawn an allow-listed worker builtin and return a task handle.", "task", true},
    {"task_queue", "thread_pool_submit", "function", "Int",
     "task_queue(target: Text, name: Text, arg) -> Int",
     "Queue an allow-listed worker builtin and return a task handle.", "task", true},
    {"task_wait", "WaitForThread", "function", "Int",
     "task_wait(handle: Int) -> Int",
     "Wait for a task handle to finish; returns 0 on success.", "task", true},
    {"task_lookup", "thread_lookup", "function", "Int",
     "task_lookup(name: Text) -> Int",
     "Look up a named queued worker handle.", "task", true},
    {"task_status", "thread_get_status", "function", "Int",
     "task_status(handle: Int, consume: Bool = false) -> Int",
     "Read a task success/status flag from the worker runtime.", "task", true},
    {"task_result", "thread_get_result", "function", "Int",
     "task_result(handle: Int, consume: Bool = false) -> Int",
     "Read a task result payload from the worker runtime.", "task", true},
    {"task_stats", "thread_stats", "function", "Array",
     "task_stats() -> Array",
     "Snapshot worker-pool state records.", "task", true},
    {"task_stats_json", "ThreadStatsJson", "function", "Text",
     "task_stats_json() -> Text",
     "Worker-pool status encoded as JSON text.", "task", true},
    {"ai_chat", "openaichatcompletions", "function", "Text",
     "ai_chat(model: Text, messages: Text, system: Text = \"\", apiKey: Text = \"\", endpoint: Text = \"\") -> Text",
     "Call the configured AI chat/completions backend; effectful and runtime-dependent.", "ai", true},
    {"has_toon", NULL, "function", "Bool", "has_toon() -> Bool",
     "Capability probe for the yyjson/TOON backend.", "capability", false},
    {"has_ai", NULL, "function", "Bool", "has_ai() -> Bool",
     "Capability probe for the OpenAI-compatible AI backend.", "capability", false},
    {"has_builtin", "hasextbuiltin", "function", "Bool",
     "has_builtin(category: Text, function: Text) -> Bool",
     "Capability probe for extended builtin categories/functions.", "capability", false},
    {"builtins_json", "aetherbuiltinsjson", "function", "Text",
     "builtins_json(detail: Bool = false) -> Text",
     "Structured JSON list of Aether-visible builtins. detail=true adds usage metadata.", "capability", false},
    {"builtin_info", "aetherbuiltininfo", "function", "Text",
     "builtin_info(name: Text) -> Text",
     "Structured JSON metadata for one Aether-visible builtin.", "capability", false},

    /* --- Raw VM builtins with curated signatures (source: vm_builtin) --- */
    /* Files and environment (effectful; call inside fx). */
    {"fileexists", NULL, "function", "Bool",
     "fileexists(path: Text) -> Bool",
     "True if a file or directory exists at path.", "filesystem", true, "vm_builtin"},
    {"getcurrentdir", NULL, "function", "Text",
     "getcurrentdir() -> Text",
     "Current working directory.", "filesystem", true, "vm_builtin"},
    {"getenv", NULL, "function", "Text",
     "getenv(name: Text) -> Text",
     "Environment variable value, or empty Text if unset.", "system", true, "vm_builtin"},
    {"getenvint", NULL, "function", "Int",
     "getenvint(name: Text, fallback: Int) -> Int",
     "Environment variable parsed as Int, or fallback.", "system", true, "vm_builtin"},
    {"paramcount", NULL, "function", "Int",
     "paramcount() -> Int",
     "Count of command-line arguments.", "system", true, "vm_builtin"},
    {"paramstr", NULL, "function", "Text",
     "paramstr(i: Int) -> Text",
     "i-th command-line argument; paramstr(0) is the program.", "system", true, "vm_builtin"},
    /* Text helpers (pure). */
    {"copy", NULL, "function", "Text",
     "copy(s: Text, start: Int, count: Int) -> Text",
     "Substring of length count from 1-based start.", "strings", false, "vm_builtin"},
    {"pos", NULL, "function", "Int",
     "pos(needle: Text, s: Text) -> Int",
     "1-based index of needle in s, or 0 if absent.", "strings", false, "vm_builtin"},
    {"trim", NULL, "function", "Text",
     "trim(s: Text) -> Text",
     "Strip leading and trailing whitespace.", "strings", false, "vm_builtin"},
    {"stringofchar", NULL, "function", "Text",
     "stringofchar(ch: Text, n: Int) -> Text",
     "Text of n copies of single-character ch.", "strings", false, "vm_builtin"},
    {"realtostr", NULL, "function", "Text",
     "realtostr(value: Real) -> Text",
     "Real to Text with 6 decimals.", "conversion", false, "vm_builtin"},
    {"formatfloat", NULL, "function", "Text",
     "formatfloat(value: Real, precision: Int) -> Text",
     "Real to Text with the given decimal precision.", "conversion", false, "vm_builtin"},
    /* Nondeterminism (effectful). */
    {"random", NULL, "function", "Real",
     "random() -> Real in [0,1); random(n: Int) -> Int in [0,n)",
     "Pseudorandom value; seed with randomize().", "system", true, "vm_builtin"},
    {"randomize", NULL, "procedure", "Void",
     "randomize() -> Void",
     "Seed the pseudorandom generator.", "system", true, "vm_builtin"},
    /* HTTP (effectful; session + memory-stream model). */
    {"httpsession", NULL, "function", "Int",
     "httpsession() -> Int",
     "Open an HTTP session; returns a session id (-1 on failure).", "network", true, "vm_builtin"},
    {"httpclose", NULL, "procedure", "Void",
     "httpclose(session: Int) -> Void",
     "Close an HTTP session.", "network", true, "vm_builtin"},
    {"httpsetheader", NULL, "procedure", "Void",
     "httpsetheader(session: Int, name: Text, value: Text) -> Void",
     "Set a request header on a session.", "network", true, "vm_builtin"},
    {"httprequest", NULL, "function", "Int",
     "httprequest(session: Int, method: Text, url: Text, body: Text, out: MStream) -> Int",
     "Perform a request; writes the response body into out, returns the HTTP status.", "network", true, "vm_builtin"},
    {"httperrorcode", NULL, "function", "Int",
     "httperrorcode(session: Int) -> Int",
     "Transport-level error code for the last request on a session.", "network", true, "vm_builtin"},
    {"httplasterror", NULL, "function", "Text",
     "httplasterror(session: Int) -> Text",
     "Human-readable last error for a session.", "network", true, "vm_builtin"},
    {"httpgetheader", NULL, "function", "Text",
     "httpgetheader(session: Int, name: Text) -> Text",
     "Read a response header from the last request.", "network", true, "vm_builtin"},
    {"httpgetlastheaders", NULL, "function", "Text",
     "httpgetlastheaders(session: Int) -> Text",
     "All response headers from the last request.", "network", true, "vm_builtin"},
    {"httpclearheaders", NULL, "procedure", "Void",
     "httpclearheaders(session: Int) -> Void",
     "Clear request headers set on a session.", "network", true, "vm_builtin"},
    {"httpsetoption", NULL, "procedure", "Void",
     "httpsetoption(session: Int, key: Text, value: Int) -> Void",
     "Set a transport option (value may be Int or Text).", "network", true, "vm_builtin"},
    {"httprequesttofile", NULL, "function", "Int",
     "httprequesttofile(session: Int, method: Text, url: Text, body: Text, out: Text) -> Int",
     "Like httprequest but writes the body to file path out; returns status.", "network", true, "vm_builtin"},
    {"httprequestasync", NULL, "function", "Int",
     "httprequestasync(session: Int, method: Text, url: Text, body: Text) -> Int",
     "Start an async request; returns an async id for httpisdone/httpawait.", "network", true, "vm_builtin"},
    {"httpisdone", NULL, "function", "Bool",
     "httpisdone(id: Int) -> Bool",
     "Whether an async request has completed.", "network", true, "vm_builtin"},
    {"httpcancel", NULL, "procedure", "Void",
     "httpcancel(id: Int) -> Void",
     "Cancel an in-flight async request.", "network", true, "vm_builtin"},
    {"httpawait", NULL, "function", "Int",
     "httpawait(id: Int, out: MStream) -> Int",
     "Block until an async request finishes; writes body to out, returns status.", "network", true, "vm_builtin"},
    {"httptryawait", NULL, "function", "Int",
     "httptryawait(id: Int, out: MStream) -> Int",
     "Non-blocking await; returns status if done, else a pending indicator.", "network", true, "vm_builtin"},
    {"httprequestasynctofile", NULL, "function", "Int",
     "httprequestasynctofile(session: Int, method: Text, url: Text, body: Text, out: Text) -> Int",
     "Start an async request writing the body to file path out.", "network", true, "vm_builtin"},
    {"httpgetasyncprogress", NULL, "function", "Int",
     "httpgetasyncprogress(id: Int) -> Int",
     "Bytes received so far for an async request.", "network", true, "vm_builtin"},
    {"httpgetasynctotal", NULL, "function", "Int",
     "httpgetasynctotal(id: Int) -> Int",
     "Total expected bytes for an async request (-1 if unknown).", "network", true, "vm_builtin"},
    /* Filesystem mutation (effectful). */
    {"mkdir", NULL, "function", "Int",
     "mkdir(path: Text) -> Int",
     "Create a directory; returns 0 on success or an errno.", "filesystem", true, "vm_builtin"},
    {"rmdir", NULL, "function", "Int",
     "rmdir(path: Text) -> Int",
     "Remove an empty directory; returns 0 on success or an errno.", "filesystem", true, "vm_builtin"},
    {"mstreamloadfromfile", NULL, "procedure", "Void",
     "mstreamloadfromfile(s: MStream, filename: Text) -> Void",
     "Load a file's bytes into a memory stream.", "filesystem", true, "vm_builtin"},
    {"mstreamsavetofile", NULL, "procedure", "Void",
     "mstreamsavetofile(s: MStream, filename: Text) -> Void",
     "Write a memory stream's bytes to a file.", "filesystem", true, "vm_builtin"},
    /* SQLite (effectful; integer db/stmt handles). */
    {"sqliteopen", NULL, "function", "Int",
     "sqliteopen(path: Text) -> Int",
     "Open a database; returns a db handle (-1 on failure).", "sqlite", true, "vm_builtin"},
    {"sqliteclose", NULL, "function", "Int",
     "sqliteclose(db: Int) -> Int",
     "Close a database handle.", "sqlite", true, "vm_builtin"},
    {"sqliteexec", NULL, "function", "Int",
     "sqliteexec(db: Int, sql: Text) -> Int",
     "Execute SQL with no result rows; returns a status code.", "sqlite", true, "vm_builtin"},
    {"sqliteprepare", NULL, "function", "Int",
     "sqliteprepare(db: Int, sql: Text) -> Int",
     "Compile a statement; returns a stmt handle.", "sqlite", true, "vm_builtin"},
    {"sqlitefinalize", NULL, "function", "Int",
     "sqlitefinalize(stmt: Int) -> Int",
     "Release a prepared statement.", "sqlite", true, "vm_builtin"},
    {"sqlitestep", NULL, "function", "Int",
     "sqlitestep(stmt: Int) -> Int",
     "Advance a statement; returns the step result code.", "sqlite", true, "vm_builtin"},
    {"sqlitereset", NULL, "function", "Int",
     "sqlitereset(stmt: Int) -> Int",
     "Reset a statement for re-execution.", "sqlite", true, "vm_builtin"},
    {"sqlitecolumncount", NULL, "function", "Int",
     "sqlitecolumncount(stmt: Int) -> Int",
     "Number of result columns.", "sqlite", true, "vm_builtin"},
    {"sqlitecolumntype", NULL, "function", "Int",
     "sqlitecolumntype(stmt: Int, column: Int) -> Int",
     "SQLite type code of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlitecolumnname", NULL, "function", "Text",
     "sqlitecolumnname(stmt: Int, column: Int) -> Text",
     "Name of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlitecolumnint", NULL, "function", "Int",
     "sqlitecolumnint(stmt: Int, column: Int) -> Int",
     "Int value of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlitecolumndouble", NULL, "function", "Real",
     "sqlitecolumndouble(stmt: Int, column: Int) -> Real",
     "Real value of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlitecolumntext", NULL, "function", "Text",
     "sqlitecolumntext(stmt: Int, column: Int) -> Text",
     "Text value of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlitebindtext", NULL, "function", "Int",
     "sqlitebindtext(stmt: Int, index: Int, value: Text) -> Int",
     "Bind Text to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlitebindint", NULL, "function", "Int",
     "sqlitebindint(stmt: Int, index: Int, value: Int) -> Int",
     "Bind Int to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlitebinddouble", NULL, "function", "Int",
     "sqlitebinddouble(stmt: Int, index: Int, value: Real) -> Int",
     "Bind Real to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlitebindnull", NULL, "function", "Int",
     "sqlitebindnull(stmt: Int, index: Int) -> Int",
     "Bind NULL to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqliteclearbindings", NULL, "function", "Int",
     "sqliteclearbindings(stmt: Int) -> Int",
     "Clear all bound parameters on a statement.", "sqlite", true, "vm_builtin"},
    {"sqliteerrmsg", NULL, "function", "Text",
     "sqliteerrmsg(db: Int) -> Text",
     "Most recent error message for a database.", "sqlite", true, "vm_builtin"},
    {"sqlitelastinsertrowid", NULL, "function", "Int",
     "sqlitelastinsertrowid(db: Int) -> Int",
     "Rowid of the most recent insert.", "sqlite", true, "vm_builtin"},
    {"sqlitechanges", NULL, "function", "Int",
     "sqlitechanges(db: Int) -> Int",
     "Rows changed by the most recent statement.", "sqlite", true, "vm_builtin"},
};

static void jsonTextInit(JsonText *text) {
  if (!text) {
    return;
  }
  text->data = NULL;
  text->length = 0;
  text->capacity = 0;
}

static void jsonTextFree(JsonText *text) {
  if (!text) {
    return;
  }
  free(text->data);
  text->data = NULL;
  text->length = 0;
  text->capacity = 0;
}

static bool jsonTextEnsure(JsonText *text, size_t extra) {
  if (!text) {
    return false;
  }
  size_t need = text->length + extra + 1;
  if (need <= text->capacity) {
    return true;
  }
  size_t new_capacity = text->capacity ? text->capacity * 2 : 256;
  while (new_capacity < need) {
    new_capacity *= 2;
  }
  char *next = realloc(text->data, new_capacity);
  if (!next) {
    return false;
  }
  text->data = next;
  text->capacity = new_capacity;
  return true;
}

static bool jsonTextAppendN(JsonText *text, const char *src, size_t len) {
  if (!text || !src) {
    return false;
  }
  if (!jsonTextEnsure(text, len)) {
    return false;
  }
  memcpy(text->data + text->length, src, len);
  text->length += len;
  text->data[text->length] = '\0';
  return true;
}

static bool jsonTextAppend(JsonText *text, const char *src) {
  return src ? jsonTextAppendN(text, src, strlen(src)) : false;
}

static bool jsonTextAppendFormat(JsonText *text, const char *fmt, ...) {
  va_list args;
  va_list args_copy;
  int needed;

  if (!text || !fmt) {
    return false;
  }
  va_start(args, fmt);
  va_copy(args_copy, args);
  needed = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);
  if (needed < 0) {
    va_end(args);
    return false;
  }
  if (!jsonTextEnsure(text, (size_t)needed)) {
    va_end(args);
    return false;
  }
  vsnprintf(text->data + text->length, text->capacity - text->length, fmt, args);
  va_end(args);
  text->length += (size_t)needed;
  return true;
}

static bool jsonTextAppendEscaped(JsonText *text, const char *src) {
  if (!text || !src) {
    return false;
  }
  if (!jsonTextAppend(text, "\"")) {
    return false;
  }
  for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
    switch (*p) {
      case '\\':
        if (!jsonTextAppend(text, "\\\\")) return false;
        break;
      case '"':
        if (!jsonTextAppend(text, "\\\"")) return false;
        break;
      case '\n':
        if (!jsonTextAppend(text, "\\n")) return false;
        break;
      case '\r':
        if (!jsonTextAppend(text, "\\r")) return false;
        break;
      case '\t':
        if (!jsonTextAppend(text, "\\t")) return false;
        break;
      default:
        if (*p < 0x20) {
          if (!jsonTextAppendFormat(text, "\\u%04x", (unsigned int)*p)) {
            return false;
          }
        } else {
          if (!jsonTextEnsure(text, 1)) return false;
          text->data[text->length++] = (char)*p;
          text->data[text->length] = '\0';
        }
        break;
    }
  }
  return jsonTextAppend(text, "\"");
}

static const char *builtinTypeName(BuiltinRoutineType type) {
  switch (type) {
    case BUILTIN_TYPE_FUNCTION:
      return "function";
    case BUILTIN_TYPE_PROCEDURE:
      return "procedure";
    default:
      return "unknown";
  }
}

static bool parseBoolLike(Value value, bool *out_value) {
  if (!out_value) {
    return false;
  }
  if (VALUE_TYPE(value) == TYPE_BOOLEAN) {
    *out_value = VAL_INT(value) != 0;
    return true;
  }
  if (IS_INTLIKE(value)) {
    *out_value = AS_INTEGER(value) != 0;
    return true;
  }
  return false;
}

static bool findExtBuiltinCategoryForFunction(const char *func,
                                              const char **out_category,
                                              const char **out_group) {
  if (!func) {
    return false;
  }
  size_t category_count = extBuiltinGetCategoryCount();
  for (size_t i = 0; i < category_count; ++i) {
    const char *category = extBuiltinGetCategoryName(i);
    if (!category || !extBuiltinHasFunction(category, func)) {
      continue;
    }
    if (out_category) {
      *out_category = category;
    }
    if (out_group) {
      *out_group = kDefaultGroupName;
    }
    size_t fn_count = countFunctionsAcrossGroups(category);
    for (size_t j = 0; j < fn_count; ++j) {
      const char *group = NULL;
      const char *name = getFunctionNameAcrossGroups(category, j, &group);
      if (name && strcasecmp(name, func) == 0) {
        if (out_group) {
          *out_group = group ? group : kDefaultGroupName;
        }
        return true;
      }
    }
    return true;
  }
  return false;
}

static const AetherBuiltinMeta *findAetherBuiltinMeta(const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(kAetherBuiltinMeta) / sizeof(kAetherBuiltinMeta[0]); ++i) {
    if (strcasecmp(kAetherBuiltinMeta[i].aether_name, name) == 0) {
      return &kAetherBuiltinMeta[i];
    }
  }
  return NULL;
}

static bool appendAetherMetaObject(JsonText *json,
                                   const AetherBuiltinMeta *meta,
                                   bool detailed) {
  if (!json || !meta) {
    return false;
  }
  if (!jsonTextAppend(json, "{")) return false;
  if (!jsonTextAppend(json, "\"name\":")) return false;
  if (!jsonTextAppendEscaped(json, meta->aether_name)) return false;
  if (!jsonTextAppend(json, ",\"source\":")) return false;
  if (!jsonTextAppendEscaped(json, meta->source ? meta->source : "aether_alias")) return false;
  if (meta->backend_name) {
    if (!jsonTextAppend(json, ",\"backend_name\":")) return false;
    if (!jsonTextAppendEscaped(json, meta->backend_name)) return false;
  }
  if (!jsonTextAppend(json, ",\"kind\":")) return false;
  if (!jsonTextAppendEscaped(json, meta->kind)) return false;
  if (!jsonTextAppend(json, ",\"category\":")) return false;
  if (!jsonTextAppendEscaped(json, meta->category)) return false;
  if (!jsonTextAppend(json, ",\"effectful\":")) return false;
  if (!jsonTextAppend(json, pscalBuiltinNameIsEffectful(meta->aether_name) ? "true" : "false")) return false;
  if (!jsonTextAppend(json, ",\"return_type\":")) return false;
  if (!jsonTextAppendEscaped(json, meta->return_type)) return false;
  if (detailed) {
    if (!jsonTextAppend(json, ",\"signature\":")) return false;
    if (!jsonTextAppendEscaped(json, meta->signature)) return false;
    if (!jsonTextAppend(json, ",\"usage\":")) return false;
    if (!jsonTextAppendEscaped(json, meta->usage)) return false;
  }
  return jsonTextAppend(json, "}");
}

static bool appendVmBuiltinObject(JsonText *json,
                                  const char *name,
                                  BuiltinRoutineType type,
                                  bool detailed) {
  const char *category = "vm";
  const char *group = NULL;

  if (!json || !name) {
    return false;
  }
  if (findExtBuiltinCategoryForFunction(name, &category, &group)) {
    if (!category) {
      category = "ext";
    }
  } else {
    category = "core";
  }

  if (!jsonTextAppend(json, "{")) return false;
  if (!jsonTextAppend(json, "\"name\":")) return false;
  if (!jsonTextAppendEscaped(json, name)) return false;
  if (!jsonTextAppend(json, ",\"source\":\"vm_builtin\"")) return false;
  if (!jsonTextAppend(json, ",\"kind\":")) return false;
  if (!jsonTextAppendEscaped(json, builtinTypeName(type))) return false;
  if (!jsonTextAppend(json, ",\"category\":")) return false;
  if (!jsonTextAppendEscaped(json, category)) return false;
  if (!jsonTextAppend(json, ",\"effectful\":")) return false;
  if (!jsonTextAppend(json, pscalBuiltinNameIsEffectful(name) ? "true" : "false")) return false;
  if (group && strcasecmp(group, kDefaultGroupName) != 0) {
    if (!jsonTextAppend(json, ",\"group\":")) return false;
    if (!jsonTextAppendEscaped(json, group)) return false;
  }
  if (detailed) {
    char usage[256];
    snprintf(usage, sizeof(usage), "%s(...)", name);
    if (!jsonTextAppend(json, ",\"usage\":")) return false;
    if (!jsonTextAppendEscaped(json, usage)) return false;
  }
  return jsonTextAppend(json, "}");
}

static bool appendAetherBuiltinsJson(JsonText *json, bool detailed) {
  bool first = true;
  if (!jsonTextAppend(json, "[")) {
    return false;
  }

  for (size_t i = 0; i < sizeof(kAetherBuiltinMeta) / sizeof(kAetherBuiltinMeta[0]); ++i) {
    if (!first && !jsonTextAppend(json, ",")) return false;
    if (!appendAetherMetaObject(json, &kAetherBuiltinMeta[i], detailed)) return false;
    first = false;
  }

  size_t builtin_count = getVmBuiltinCount();
  for (size_t i = 0; i < builtin_count; ++i) {
    const char *name = getVmBuiltinNameById((int)i);
    if (!name || !*name) {
      continue;
    }
    if (!first && !jsonTextAppend(json, ",")) return false;
    if (!appendVmBuiltinObject(json, name, getVmBuiltinTypeById((int)i), detailed)) return false;
    first = false;
  }

  return jsonTextAppend(json, "]");
}

static Value vmBuiltinAetherBuiltinsJson(struct VM_s *vm, int arg_count,
                                         Value *args) {
  bool detailed = false;
  JsonText json;
  (void)vm;

  if (arg_count > 1) {
    runtimeError(vm, "AetherBuiltinsJson expects zero or one boolean argument.");
    return makeString("");
  }
  if (arg_count == 1 && !parseBoolLike(args[0], &detailed)) {
    runtimeError(vm, "AetherBuiltinsJson optional argument must be boolean-like.");
    return makeString("");
  }

  jsonTextInit(&json);
  if (!appendAetherBuiltinsJson(&json, detailed)) {
    jsonTextFree(&json);
    return makeString("");
  }
  Value result = makeString(json.data ? json.data : "");
  jsonTextFree(&json);
  return result;
}

static Value vmBuiltinAetherBuiltinInfo(struct VM_s *vm, int arg_count,
                                        Value *args) {
  JsonText json;
  const char *name;

  if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
    runtimeError(vm, "AetherBuiltinInfo expects one string argument.");
    return makeString("");
  }
  name = AS_STRING(args[0]);
  jsonTextInit(&json);

  const AetherBuiltinMeta *meta = findAetherBuiltinMeta(name);
  if (meta) {
    if (!appendAetherMetaObject(&json, meta, true)) {
      jsonTextFree(&json);
      return makeString("");
    }
  } else {
    int id = getVmBuiltinID(name);
    if (id < 0) {
      jsonTextFree(&json);
      return makeString("null");
    }
    if (!appendVmBuiltinObject(&json, getVmBuiltinNameById(id), getVmBuiltinTypeById(id), true)) {
      jsonTextFree(&json);
      return makeString("");
    }
  }

  Value result = makeString(json.data ? json.data : "");
  jsonTextFree(&json);
  return result;
}

static Value vmBuiltinHasExtBuiltin(struct VM_s *vm, int arg_count,
                                    Value *args) {
  if (arg_count != 2) {
    runtimeError(vm, "HasExtBuiltin expects exactly 2 arguments.");
    return makeBoolean(0);
  }
  if (!isPascalStringType(VALUE_TYPE(args[0])) || !isPascalStringType(VALUE_TYPE(args[1]))) {
    runtimeError(vm, "HasExtBuiltin expects string arguments.");
    return makeBoolean(0);
  }
  const char *category = AS_STRING(args[0]);
  const char *func = AS_STRING(args[1]);
  int present = extBuiltinHasFunction(category, func);
  return makeBoolean(present);
}

static size_t countFunctionsAcrossGroups(const char *category) {
  size_t total = 0;
  if (extBuiltinHasGroup(category, NULL)) {
    total += extBuiltinGetFunctionCount(category, NULL);
  }
  size_t group_count = extBuiltinGetGroupCount(category);
  for (size_t i = 0; i < group_count; ++i) {
    const char *group = extBuiltinGetGroupName(category, i);
    if (!group) {
      continue;
    }
    total += extBuiltinGetFunctionCount(category, group);
  }
  return total;
}

static const char *getFunctionNameAcrossGroups(const char *category,
                                               size_t index,
                                               const char **out_group) {
  if (extBuiltinHasGroup(category, NULL)) {
    size_t default_count = extBuiltinGetFunctionCount(category, NULL);
    if (index < default_count) {
      if (out_group) {
        *out_group = kDefaultGroupName;
      }
      return extBuiltinGetFunctionName(category, NULL, index);
    }
    index -= default_count;
  }

  size_t group_count = extBuiltinGetGroupCount(category);
  for (size_t i = 0; i < group_count; ++i) {
    const char *group = extBuiltinGetGroupName(category, i);
    if (!group) {
      continue;
    }
    size_t group_size = extBuiltinGetFunctionCount(category, group);
    if (index < group_size) {
      if (out_group) {
        *out_group = group;
      }
      return extBuiltinGetFunctionName(category, group, index);
    }
    index -= group_size;
  }
  return NULL;
}

static Value vmBuiltinExtBuiltinCategoryCount(struct VM_s *vm, int arg_count,
                                              Value *args) {
  (void)args;
  if (arg_count != 0) {
    runtimeError(vm, "ExtBuiltinCategoryCount expects no arguments.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetCategoryCount();
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinCategoryName(struct VM_s *vm, int arg_count,
                                             Value *args) {
  if (arg_count != 1 || !IS_INTLIKE(args[0])) {
    runtimeError(vm,
                 "ExtBuiltinCategoryName expects a single integer argument.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[0]);
  if (idx < 0) {
    return makeString("");
  }
  const char *name = extBuiltinGetCategoryName((size_t)idx);
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

static Value vmBuiltinExtBuiltinGroupCount(struct VM_s *vm, int arg_count,
                                          Value *args) {
  if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
    runtimeError(vm,
                 "ExtBuiltinGroupCount expects a single string argument.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetGroupCount(AS_STRING(args[0]));
  if (extBuiltinHasGroup(AS_STRING(args[0]), NULL)) {
    ++count; /* include the default bucket */
  }
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinGroupName(struct VM_s *vm, int arg_count,
                                         Value *args) {
  if (arg_count != 2 || !isPascalStringType(VALUE_TYPE(args[0])) || !IS_INTLIKE(args[1])) {
    runtimeError(vm,
                 "ExtBuiltinGroupName expects a string category and integer index.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[1]);
  if (idx < 0) {
    return makeString("");
  }
  const char *category = AS_STRING(args[0]);
  size_t default_groups = extBuiltinHasGroup(category, NULL) ? 1 : 0;
  if ((size_t)idx < default_groups) {
    return makeString(kDefaultGroupName);
  }
  size_t adj_index = (size_t)idx - default_groups;
  const char *name = extBuiltinGetGroupName(category, adj_index);
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

static Value vmBuiltinExtBuiltinFunctionCount(struct VM_s *vm, int arg_count,
                                              Value *args) {
  if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
    runtimeError(vm,
                 "ExtBuiltinFunctionCount expects a single string argument.");
    return makeInt(0);
  }
  size_t count = countFunctionsAcrossGroups(AS_STRING(args[0]));
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinFunctionName(struct VM_s *vm, int arg_count,
                                             Value *args) {
  if (arg_count != 2 || !isPascalStringType(VALUE_TYPE(args[0])) || !IS_INTLIKE(args[1])) {
    runtimeError(
        vm,
        "ExtBuiltinFunctionName expects a string category and integer index.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[1]);
  if (idx < 0) {
    return makeString("");
  }
  const char *name =
      getFunctionNameAcrossGroups(AS_STRING(args[0]), (size_t)idx, NULL);
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

static Value vmBuiltinExtBuiltinGroupFunctionCount(struct VM_s *vm,
                                                   int arg_count,
                                                   Value *args) {
  if (arg_count != 2 || !isPascalStringType(VALUE_TYPE(args[0])) || !isPascalStringType(VALUE_TYPE(args[1]))) {
    runtimeError(vm, "ExtBuiltinGroupFunctionCount expects two string arguments.");
    return makeInt(0);
  }
  size_t count = extBuiltinGetFunctionCount(AS_STRING(args[0]), AS_STRING(args[1]));
  if (!count && strcasecmp(AS_STRING(args[1]), kDefaultGroupName) == 0) {
    count = extBuiltinGetFunctionCount(AS_STRING(args[0]), NULL);
  }
  return makeInt((long long)count);
}

static Value vmBuiltinExtBuiltinGroupFunctionName(struct VM_s *vm,
                                                  int arg_count,
                                                  Value *args) {
  if (arg_count != 3 || !isPascalStringType(VALUE_TYPE(args[0])) || !isPascalStringType(VALUE_TYPE(args[1])) ||
      !IS_INTLIKE(args[2])) {
    runtimeError(vm, "ExtBuiltinGroupFunctionName expects category, group, and index.");
    return makeString("");
  }
  long long idx = AS_INTEGER(args[2]);
  if (idx < 0) {
    return makeString("");
  }
  const char *group = AS_STRING(args[1]);
  const char *category = AS_STRING(args[0]);
  const char *name = NULL;
  if (strcasecmp(group, kDefaultGroupName) == 0) {
    name = extBuiltinGetFunctionName(category, NULL, (size_t)idx);
  } else {
    name = extBuiltinGetFunctionName(category, group, (size_t)idx);
  }
  if (!name) {
    return makeString("");
  }
  return makeString(name);
}

void registerExtBuiltinQueryBuiltins(void) {
  registerVmBuiltin("hasextbuiltin", vmBuiltinHasExtBuiltin,
                    BUILTIN_TYPE_FUNCTION, "HasExtBuiltin", FX_PURE);
  registerVmBuiltin("extbuiltincategorycount",
                    vmBuiltinExtBuiltinCategoryCount, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinCategoryCount", FX_PURE);
  registerVmBuiltin("extbuiltincategoryname",
                    vmBuiltinExtBuiltinCategoryName, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinCategoryName", FX_PURE);
  registerVmBuiltin("extbuiltingroupcount", vmBuiltinExtBuiltinGroupCount,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupCount", FX_PURE);
  registerVmBuiltin("extbuiltingroupname", vmBuiltinExtBuiltinGroupName,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupName", FX_PURE);
  registerVmBuiltin("extbuiltinfunctioncount",
                    vmBuiltinExtBuiltinFunctionCount, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinFunctionCount", FX_PURE);
  registerVmBuiltin("extbuiltinfunctionname",
                    vmBuiltinExtBuiltinFunctionName, BUILTIN_TYPE_FUNCTION,
                    "ExtBuiltinFunctionName", FX_PURE);
  registerVmBuiltin("extbuiltingroupfunctioncount",
                    vmBuiltinExtBuiltinGroupFunctionCount,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupFunctionCount", FX_PURE);
  registerVmBuiltin("extbuiltingroupfunctionname",
                    vmBuiltinExtBuiltinGroupFunctionName,
                    BUILTIN_TYPE_FUNCTION, "ExtBuiltinGroupFunctionName", FX_PURE);
  registerVmBuiltin("aetherbuiltinsjson", vmBuiltinAetherBuiltinsJson,
                    BUILTIN_TYPE_FUNCTION, "AetherBuiltinsJson", FX_PURE);
  registerVmBuiltin("builtins_json", vmBuiltinAetherBuiltinsJson,
                    BUILTIN_TYPE_FUNCTION, "builtins_json", FX_PURE);
  registerVmBuiltin("aetherbuiltininfo", vmBuiltinAetherBuiltinInfo,
                    BUILTIN_TYPE_FUNCTION, "AetherBuiltinInfo", FX_PURE);
  registerVmBuiltin("builtin_info", vmBuiltinAetherBuiltinInfo,
                    BUILTIN_TYPE_FUNCTION, "builtin_info", FX_PURE);
}
