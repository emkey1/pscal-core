#include "backend_ast/builtin.h"
#include "common/frontend_kind.h"
#include "core/utils.h"
#include "ext_builtins/registry.h"
#include "vm/vm_fx_policy.h"
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
  bool effectful;          /* emit derives from the live effect mask; used only with no backend */
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
     "Read an object child node by key; a missing key gives a missing node (every toon_is_* false, toon_type \"\").", "toon", false},
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
     "Length of a TOON array or object node; 0 for a null or absent node.", "toon", false},
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
    {"toon_get_text_or", "YyjsonGetStringOr", "function", "Text",
     "toon_get_text_or(node: ToonNode, key: Text, fallback: Text) -> Text",
     "Read a keyed child text value; the fallback when the key is missing, null or not text.", "toon", false},
    {"toon_get_int", NULL, "function", "Int",
     "toon_get_int(node: ToonNode, key: Text) -> Int",
     "Read a keyed child Int value.", "toon", false},
    {"toon_get_int_or", "YyjsonGetIntOr", "function", "Int",
     "toon_get_int_or(node: ToonNode, key: Text, fallback: Int) -> Int",
     "Read a keyed child Int value; the fallback when the key is missing, null or not an Int.", "toon", false},
    {"toon_get_real", NULL, "function", "Real",
     "toon_get_real(node: ToonNode, key: Text) -> Real",
     "Read a keyed child Real value.", "toon", false},
    {"toon_get_real_or", "YyjsonGetNumberOr", "function", "Real",
     "toon_get_real_or(node: ToonNode, key: Text, fallback: Real) -> Real",
     "Read a keyed child number as Real (an Int converts); the fallback when the key is missing, null or not a number.", "toon", false},
    {"toon_get_bool", NULL, "function", "Bool",
     "toon_get_bool(node: ToonNode, key: Text) -> Bool",
     "Read a keyed child Bool value.", "toon", false},
    {"toon_get_bool_or", "YyjsonGetBoolOr", "function", "Bool",
     "toon_get_bool_or(node: ToonNode, key: Text, fallback: Bool) -> Bool",
     "Read a keyed child Bool value; the fallback when the key is missing, null or not a Bool.", "toon", false},
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
     "Spawn an allow-listed worker builtin, named as Aether spells it (\"sleep\", \"dns_lookup\"), and return a task handle.", "task", true},
    {"task_queue", "thread_pool_submit", "function", "Int",
     "task_queue(target: Text, name: Text, arg) -> Int",
     "Queue an allow-listed worker builtin and return a task handle.", "task", true},
    {"task_wait", "WaitForThread", "function", "Int",
     "task_wait(handle: Int) -> Int",
     "Wait for a task handle to finish; returns 0 on success.", "task", true},
    {"task_lookup", "thread_lookup", "function", "Int",
     "task_lookup(name: Text) -> Int",
     "Look up a named queued worker handle.", "task", true},
    {"task_status", "thread_get_status", "function", "Bool",
     "task_status(handle: Int, drop_result: Bool = false) -> Bool",
     "True when the finished task succeeded. Reading it takes the status: read it once, after the task has finished and before task_wait; a second read is the run-time error AETH-RUNTIME-TASK-NO-STATUS. drop_result: true also discards the task's result.", "task", true},
    /* No fixed type: the payload is whatever the target builtin returned, so
     * Aether's type checker leaves the call untyped (types.c). */
    {"task_result", "thread_get_result", "function", "T",
     "task_result(handle: Int, consume: Bool = false) -> T (the target's result: Text for \"dns_lookup\")",
     "The target builtin's own result, of the type that builtin returns (Text for \"dns_lookup\"): bind it with that type. A target that returns nothing (\"sleep\") has no result, and reading it is the run-time error AETH-RUNTIME-TASK-NO-RESULT; wait with task_wait. A run-time error while the task is still running.", "task", true},
    {"task_set_name", "thread_set_name", "function", "Bool",
     "task_set_name(handle: Int, name: Text) -> Bool",
     "Rename a task, so task_lookup finds it by the new name; true when it was renamed.", "task", true},
    {"task_pause", "thread_pause", "function", "Bool",
     "task_pause(handle: Int) -> Bool",
     "Pause a running task; true on success.", "task", true},
    {"task_resume", "thread_resume", "function", "Bool",
     "task_resume(handle: Int) -> Bool",
     "Resume a paused task; true on success.", "task", true},
    {"task_cancel", "thread_cancel", "function", "Bool",
     "task_cancel(handle: Int) -> Bool",
     "Ask a task to stop; true when the request reached it. Join it with task_wait afterwards.", "task", true},
    {"task_stats", "thread_stats", "function", "untyped array",
     "task_stats() -> untyped array",
     "Worker-pool state records with no Aether type, so the result cannot be bound to a variable: count them with len(task_stats()); read their fields through task_stats_json().", "task", true},
    {"task_stats_json", "ThreadStatsJson", "function", "Text",
     "task_stats_json() -> Text",
     "Worker-pool status encoded as JSON text.", "task", true},
    {"ai_chat", "openaichatcompletions", "function", "Text",
     "ai_chat(model: Text, messages: Text, system: Text = \"\", apiKey: Text = \"\", endpoint: Text = \"\") -> Text",
     "Call the configured AI chat/completions backend; `system` goes first as a system message (JSON object fields there are merged into the request instead). Effectful and runtime-dependent.", "ai", true},
    {"has_toon", NULL, "function", "Bool", "has_toon() -> Bool",
     "Capability probe for the yyjson/TOON backend.", "capability", false},
    {"has_ai", NULL, "function", "Bool", "has_ai() -> Bool",
     "Capability probe for the OpenAI-compatible AI backend.", "capability", false},
    /* Aether's front end turns this call into a string literal (no engine builtin). */
    {"aether_version", NULL, "function", "Text", "aether_version() -> Text",
     "The compiler's version as `aether --version` prints it after \"Version:\": the language version and the release label, e.g. \"2026-10-20-13 (1.0-beta3)\".", "capability", false},
    {"has_builtin", "hasextbuiltin", "function", "Bool",
     "has_builtin(category: Text, function: Text) -> Bool",
     "Can this builtin be called? Takes Aether names (\"sqlite_open\"); a core builtin matches whatever the category.", "capability", false},
    {"builtins_json", "aetherbuiltinsjson", "function", "Text",
     "builtins_json(detail: Bool = false) -> Text",
     "JSON array with one object per Aether-visible builtin (name, kind, category, ...). detail=true adds usage metadata.", "capability", false},
    {"builtin_info", "aetherbuiltininfo", "function", "Text",
     "builtin_info(name: Text) -> Text",
     "Structured JSON metadata for one Aether-visible builtin.", "capability", false},

    /* --- Raw VM builtins with curated signatures (source: vm_builtin) --- */
    /* Files and environment (effectful; call inside fx). */
    {"get_current_dir", "getcurrentdir", "function", "Text",
     "get_current_dir() -> Text",
     "Current working directory.", "filesystem", true, "vm_builtin"},
    {"get_env", "getenv", "function", "Text",
     "get_env(name: Text) -> Text",
     "Environment variable value, or empty Text if unset.", "system", true, "vm_builtin"},
    {"get_env_int", "getenvint", "function", "Int",
     "get_env_int(name: Text, fallback: Int) -> Int",
     "Environment variable parsed as Int, or fallback.", "system", true, "vm_builtin"},
    /* Text helpers (pure). */
    {"trim", NULL, "function", "Text",
     "trim(s: Text) -> Text",
     "Strip leading and trailing whitespace.", "strings", false, "vm_builtin"},
    {"int_to_text", "IntToStr", "function", "Text",
     "int_to_text(n: Int) -> Text",
     "n in decimal, with a leading - when negative.", "conversion", false, "vm_builtin"},
    {"real_to_text", "realtostr", "function", "Text",
     "real_to_text(value: Real) -> Text",
     "Real to Text with 6 decimals.", "conversion", false, "vm_builtin"},
    {"format_float", "formatfloat", "function", "Text",
     "format_float(value: Real, precision: Int) -> Text",
     "Real to Text with the given decimal precision.", "conversion", false, "vm_builtin"},
    /* The modern standard surface (Aether D56-D61; ext_builtins/strings/stdlib.c
     * and aliases of older engine builtins). */
    {"upper", "aether_upper", "function", "Text",
     "upper(s: Text) -> Text",
     "s with its ASCII letters in upper case.", "strings", false},
    {"lower", "aether_lower", "function", "Text",
     "lower(s: Text) -> Text",
     "s with its ASCII letters in lower case.", "strings", false},
    {"replace", "aether_replace", "function", "Text",
     "replace(s: Text, old: Text, new: Text) -> Text",
     "Every non-overlapping occurrence of old replaced, left to right; s unchanged when old is empty.", "strings", false},
    {"contains", "aether_contains", "function", "Bool",
     "contains(s: Text, x: Text) -> Bool; contains(xs: T[], v: T) -> Bool",
     "Whether x occurs in s, or v is an element of xs.", "strings", false},
    {"starts_with", "aether_starts_with", "function", "Bool",
     "starts_with(s: Text, prefix: Text) -> Bool",
     "Whether s begins with prefix.", "strings", false},
    {"ends_with", "aether_ends_with", "function", "Bool",
     "ends_with(s: Text, suffix: Text) -> Bool",
     "Whether s ends with suffix.", "strings", false},
    {"index_of", "aether_index_of", "function", "Int",
     "index_of(s: Text, x: Text) -> Int; index_of(xs: T[], v: T) -> Int",
     "Zero-based index of the first occurrence, or -1.", "strings", false},
    {"repeat", "aether_repeat", "function", "Text",
     "repeat(s: Text, n: Int) -> Text",
     "s written n times; empty when n <= 0.", "strings", false},
    {"join", "aether_join", "function", "Text",
     "join(xs: Text[], sep: Text) -> Text",
     "The elements of xs with sep between them.", "strings", false},
    /* Character classes (D67): ASCII, true when t is non-empty and every
     * character of t is in the class. */
    {"is_digit", "aether_is_digit", "function", "Bool",
     "is_digit(t: Text) -> Bool",
     "True when t is non-empty and every character is an ASCII digit 0-9.", "strings", false},
    {"is_alpha", "aether_is_alpha", "function", "Bool",
     "is_alpha(t: Text) -> Bool",
     "True when t is non-empty and every character is an ASCII letter.", "strings", false},
    {"is_alnum", "aether_is_alnum", "function", "Bool",
     "is_alnum(t: Text) -> Bool",
     "True when t is non-empty and every character is an ASCII letter or digit.", "strings", false},
    {"is_space", "aether_is_space", "function", "Bool",
     "is_space(t: Text) -> Bool",
     "True when t is non-empty and every character is a space, tab, newline, carriage return, form feed or vertical tab.", "strings", false},
    {"is_upper", "aether_is_upper", "function", "Bool",
     "is_upper(t: Text) -> Bool",
     "True when t is non-empty and every character is an ASCII upper-case letter.", "strings", false},
    {"is_lower", "aether_is_lower", "function", "Bool",
     "is_lower(t: Text) -> Bool",
     "True when t is non-empty and every character is an ASCII lower-case letter.", "strings", false},
    {"same", "aether_same", "function", "Bool",
     "same(a: T, b: T) -> Bool",
     "True when a and b are the same record (identity); `==` compares records field by field.", "collection", false},
    {"count", "aether_count", "function", "Int",
     "count(xs: T[], v: T) -> Int; count(s: Text, x: Text) -> Int",
     "Elements equal to v, or non-overlapping occurrences of x in s.", "collection", false},
    {"sum", "aether_sum", "function", "T",
     "sum(xs: Int[]) -> Int; sum(xs: Real[]) -> Real",
     "The sum of the elements; 0 for an empty array.", "collection", false},
    {"min_of", "aether_min_of", "function", "T",
     "min_of(xs: T[]) -> T",
     "The smallest element of an Int[], Real[] or Text[]; a run-time error when empty.", "collection", false},
    {"max_of", "aether_max_of", "function", "T",
     "max_of(xs: T[]) -> T",
     "The largest element of an Int[], Real[] or Text[]; a run-time error when empty.", "collection", false},
    {"sorted", "aether_sorted", "function", "T[]",
     "sorted(xs: T[]) -> T[]",
     "A new array, ascending (stable), of an Int[], Real[] or Text[].", "collection", false},
    {"sorted_desc", "aether_sorted_desc", "function", "T[]",
     "sorted_desc(xs: T[]) -> T[]",
     "A new array, descending (stable).", "collection", false},
    {"sorted_by", "aether_sorted_by", "function", "R[]",
     "sorted_by(xs: R[], \"field\") -> R[]",
     "A new array of records, ascending by the named Int, Real or Text field (a string literal; stable).", "collection", false},
    {"sorted_by_desc", "aether_sorted_by_desc", "function", "R[]",
     "sorted_by_desc(xs: R[], \"field\") -> R[]",
     "A new array of records, descending by the named field (stable).", "collection", false},
    {"reversed", "aether_reversed", "function", "T[]",
     "reversed(xs: T[]) -> T[]; reversed(s: Text) -> Text",
     "A new array (or Text) in reverse order.", "collection", false},
    /* Maps (D68): Map[Text, T]. `m[k]`, `m[k] = v`, `{"k": v}` and
     * `loop k in m` are syntax; length(m) is the generic length. */
    {"has", "aether_map_has", "function", "Bool",
     "has(m: Map[Text, T], k: Text) -> Bool",
     "Whether the map holds key k (`k in m` is the same test).", "collection", false},
    {"get", "aether_map_get_or", "function", "T",
     "get(m: Map[Text, T], k: Text, default: T) -> T",
     "m[k] when the key is there, else default.", "collection", false},
    {"remove", "aether_map_remove", "procedure", "Void",
     "remove(m: Map[Text, T], k: Text) -> Void",
     "Removes key k from the map variable m; nothing happens when it is absent.", "collection", false},
    {"keys", "aether_map_keys", "function", "Text[]",
     "keys(m: Map[Text, T]) -> Text[]",
     "The keys in insertion order.", "collection", false},
    {"values", "aether_map_values", "function", "T[]",
     "values(m: Map[Text, T]) -> T[]",
     "The values in the order of keys(m).", "collection", false},
    {"try_parse_int", NULL, "function", "(Int, Bool)",
     "try_parse_int(t: Text) -> (Int, Bool)",
     "(value, ok): ok is false (value 0) unless t is a decimal Int, optionally signed and space-padded.", "conversion", false},
    {"try_parse_real", NULL, "function", "(Real, Bool)",
     "try_parse_real(t: Text) -> (Real, Bool)",
     "(value, ok): ok is false (value 0.0) unless t is a decimal Real.", "conversion", false},
    {"round_to", "aether_round_to", "function", "Real",
     "round_to(x: Real, digits: Int) -> Real",
     "x rounded to digits decimals, half away from zero (2.675 -> 2.68).", "conversion", false},
    {"read_file", "aether_read_file", "function", "Text",
     "read_file(path: Text) -> Text",
     "The whole file as Text; a run-time error when it cannot be read.", "filesystem", true},
    {"read_lines", "aether_read_lines", "function", "Text[]",
     "read_lines(path: Text) -> Text[]",
     "The file's lines without their line breaks.", "filesystem", true},
    {"try_read_file", NULL, "function", "(Text, Bool)",
     "try_read_file(path: Text) -> (Text, Bool)",
     "(text, ok): ok is false (text \"\") when the file cannot be read; never a run-time error.", "filesystem", true},
    {"try_read_lines", NULL, "function", "(Text[], Bool)",
     "try_read_lines(path: Text) -> (Text[], Bool)",
     "(lines, ok): ok is false (lines []) when the file cannot be read; never a run-time error.", "filesystem", true},
    {"try_toon_parse_file", NULL, "function", "(ToonDoc, Bool)",
     "try_toon_parse_file(path: Text) -> (ToonDoc, Bool)",
     "(doc, ok): ok is false when the file cannot be read or is not JSON; doc is then JSON null (close it as usual).", "filesystem", true},
    {"write_file", "aether_write_file", "procedure", "Void",
     "write_file(path: Text, text: Text) -> Void",
     "Create or truncate the file and write text.", "filesystem", true},
    {"append_file", "aether_append_file", "procedure", "Void",
     "append_file(path: Text, text: Text) -> Void",
     "Add text at the end of the file, creating it.", "filesystem", true},
    {"delete_file", "aether_delete_file", "function", "Bool",
     "delete_file(path: Text) -> Bool",
     "Delete a file; true when it was deleted.", "filesystem", true},
    {"rename_file", "aether_rename_file", "function", "Bool",
     "rename_file(from: Text, to: Text) -> Bool",
     "Rename a file; true when it was renamed.", "filesystem", true},
    {"file_exists", "fileexists", "function", "Bool",
     "file_exists(path: Text) -> Bool",
     "True if a file or directory exists at path.", "filesystem", true},
    {"read_line", "aether_read_line", "function", "Text",
     "read_line() -> Text",
     "The next stdin line without its line break; empty at the end of input.", "io", true},
    {"read_all_stdin", "aether_read_all_stdin", "function", "Text",
     "read_all_stdin() -> Text",
     "The rest of stdin, each line ending in a line break.", "io", true},
    {"args", "aether_args", "function", "Text[]",
     "args() -> Text[]",
     "The command-line arguments after the program.", "system", true},
    {"clock_ms", "aether_clock_ms", "function", "Int",
     "clock_ms() -> Int",
     "Milliseconds since 1970-01-01 00:00 UTC.", "time", true},
    {"now_text", "aether_now_text", "function", "Text",
     "now_text() -> Text",
     "Local date and time as \"YYYY-MM-DD HH:MM:SS\".", "time", true},
    {"mstream_new", "mstreamcreate", "function", "MStream",
     "mstream_new() -> MStream",
     "A new, empty memory stream.", "strings", false},
    {"mstream_from_text", "mstreamfromstring", "function", "MStream",
     "mstream_from_text(t: Text) -> MStream",
     "A new memory stream holding t's bytes.", "strings", false},
    {"mstream_text", "mstreambuffer", "function", "Text",
     "mstream_text(ms: MStream) -> Text",
     "The stream's bytes as Text.", "strings", false},
    {"mstream_write_byte", "mstreamappendbyte", "procedure", "Void",
     "mstream_write_byte(ms: MStream, b: Int) -> Void",
     "Write one byte (0..255) at the end of the stream.", "strings", false},
    {"mstream_write_text", "aether_mstream_append_text", "procedure", "Void",
     "mstream_write_text(ms: MStream, t: Text) -> Void",
     "Write t's bytes at the end of the stream.", "strings", false},
    {"mstream_length", "aether_mstream_length", "function", "Int",
     "mstream_length(ms: MStream) -> Int",
     "The number of bytes in the stream.", "strings", false},
    {"mstream_load", "aether_mstream_load", "function", "MStream",
     "mstream_load(path: Text) -> MStream",
     "A new memory stream holding the file's bytes.", "filesystem", true},
    {"mstream_save", "mstreamsavetofile", "procedure", "Void",
     "mstream_save(ms: MStream, path: Text) -> Void",
     "Write the stream's bytes to a file.", "filesystem", true},
    {"mstream_free", "mstreamfree", "procedure", "Void",
     "mstream_free(ms: MStream) -> Void",
     "Release a memory stream.", "strings", false},
    /* Nondeterminism (effectful). */
    {"random", NULL, "function", "Real",
     "random() -> Real in [0,1); random(n: Int) -> Int in [0,n)",
     "Pseudorandom value; seed with randomize().", "system", true, "vm_builtin"},
    {"randomize", NULL, "procedure", "Void",
     "randomize() -> Void",
     "Seed the pseudorandom generator.", "system", true, "vm_builtin"},
    /* HTTP (effectful; session + memory-stream model). */
    {"http_session", "httpsession", "function", "Int",
     "http_session() -> Int",
     "Open an HTTP session; returns a session id (-1 on failure).", "network", true, "vm_builtin"},
    {"http_close", "httpclose", "procedure", "Void",
     "http_close(session: Int) -> Void",
     "Close an HTTP session.", "network", true, "vm_builtin"},
    {"http_set_header", "httpsetheader", "procedure", "Void",
     "http_set_header(session: Int, name: Text, value: Text) -> Void",
     "Set a request header on a session.", "network", true, "vm_builtin"},
    {"http_request", "httprequest", "function", "Int",
     "http_request(session: Int, method: Text, url: Text, body: Text, out: MStream) -> Int",
     "Perform a request; writes the response body into out, returns the HTTP status, or -(error class) on a transport error (see http_error_code).", "network", true, "vm_builtin"},
    {"http_error_code", "httperrorcode", "function", "Int",
     "http_error_code(session: Int) -> Int",
     "Transport-level error code for the last request on a session.", "network", true, "vm_builtin"},
    {"http_last_error", "httplasterror", "function", "Text",
     "http_last_error(session: Int) -> Text",
     "Human-readable last error for a session.", "network", true, "vm_builtin"},
    {"http_get_header", "httpgetheader", "function", "Text",
     "http_get_header(session: Int, name: Text) -> Text",
     "Read a response header from the last request.", "network", true, "vm_builtin"},
    {"http_get_last_headers", "httpgetlastheaders", "function", "Text",
     "http_get_last_headers(session: Int) -> Text",
     "All response headers from the last request.", "network", true, "vm_builtin"},
    {"http_clear_headers", "httpclearheaders", "procedure", "Void",
     "http_clear_headers(session: Int) -> Void",
     "Clear request headers set on a session.", "network", true, "vm_builtin"},
    {"http_set_option", "httpsetoption", "procedure", "Void",
     "http_set_option(session: Int, key: Text, value: Int) -> Void; http_set_option(session: Int, key: Text, value: Text) -> Void",
     "Set a transport option: Int for timeout_ms, follow_redirects, verify_peer, ...; Text for user_agent, proxy, ca_path, ...", "network", true, "vm_builtin"},
    {"http_request_to_file", "httprequesttofile", "function", "Int",
     "http_request_to_file(session: Int, method: Text, url: Text, body: Text, out: Text) -> Int",
     "Like httprequest but writes the body to file path out; returns status.", "network", true, "vm_builtin"},
    {"http_request_async", "httprequestasync", "function", "Int",
     "http_request_async(session: Int, method: Text, url: Text, body: Text) -> Int",
     "Start an async request; returns an async id for httpisdone/httpawait.", "network", true, "vm_builtin"},
    {"http_is_done", "httpisdone", "function", "Bool",
     "http_is_done(id: Int) -> Bool",
     "Whether an async request has completed.", "network", true, "vm_builtin"},
    {"http_cancel", "httpcancel", "procedure", "Void",
     "http_cancel(id: Int) -> Void",
     "Cancel an in-flight async request.", "network", true, "vm_builtin"},
    {"http_await", "httpawait", "function", "Int",
     "http_await(id: Int, out: MStream) -> Int",
     "Block until an async request finishes; writes body to out, returns status.", "network", true, "vm_builtin"},
    {"http_try_await", "httptryawait", "function", "Int",
     "http_try_await(id: Int, out: MStream) -> Int",
     "Non-blocking await; returns status if done, else a pending indicator.", "network", true, "vm_builtin"},
    {"http_request_async_to_file", "httprequestasynctofile", "function", "Int",
     "http_request_async_to_file(session: Int, method: Text, url: Text, body: Text, out: Text) -> Int",
     "Start an async request writing the body to file path out.", "network", true, "vm_builtin"},
    {"http_get_async_progress", "httpgetasyncprogress", "function", "Int",
     "http_get_async_progress(id: Int) -> Int",
     "Bytes received so far for an async request.", "network", true, "vm_builtin"},
    {"http_get_async_total", "httpgetasynctotal", "function", "Int",
     "http_get_async_total(id: Int) -> Int",
     "Total expected bytes for an async request (-1 if unknown).", "network", true, "vm_builtin"},
    /* Sockets and DNS (effectful), under their snake_case names (D72); the
     * guide's Sockets table documents each one. */
    {"socket_create", "socketcreate", "function", "Int",
     "socket_create(type: Int, family: Int = 4) -> Int",
     "New socket: type 0 = TCP, 1 = UDP; family 4 = IPv4 (default), 6 = IPv6. -1 on failure.", "network", true, "vm_builtin"},
    {"socket_connect", "socketconnect", "function", "Int",
     "socket_connect(socket: Int, host: Text, port: Int) -> Int",
     "Resolve host and connect; 0 on success, -1 on failure.", "network", true, "vm_builtin"},
    {"socket_bind", "socketbind", "function", "Int",
     "socket_bind(socket: Int, port: Int) -> Int",
     "Bind on all local interfaces; 0 on success, -1 on failure.", "network", true, "vm_builtin"},
    {"socket_bind_addr", "socketbindaddr", "function", "Int",
     "socket_bind_addr(socket: Int, host: Text, port: Int) -> Int",
     "Bind to one local address; 0 on success, -1 on failure.", "network", true, "vm_builtin"},
    {"socket_listen", "socketlisten", "function", "Int",
     "socket_listen(socket: Int, backlog: Int) -> Int",
     "Listen for connections; 0 on success, -1 on failure.", "network", true, "vm_builtin"},
    {"socket_accept", "socketaccept", "function", "Int",
     "socket_accept(socket: Int) -> Int",
     "Block until a peer connects; the connected socket, or -1.", "network", true, "vm_builtin"},
    {"socket_peer_addr", "socketpeeraddr", "function", "Text",
     "socket_peer_addr(socket: Int) -> Text",
     "Remote IP of a connected socket.", "network", true, "vm_builtin"},
    {"socket_send", "socketsend", "function", "Int",
     "socket_send(socket: Int, data: Text) -> Int; socket_send(socket: Int, data: MStream) -> Int",
     "Send Text or an MStream; bytes sent, or -1.", "network", true, "vm_builtin"},
    {"socket_receive", "socketreceive", "function", "MStream",
     "socket_receive(socket: Int, maxlen: Int) -> MStream",
     "Block until data arrives; a 0-length result means the peer closed.", "network", true, "vm_builtin"},
    {"socket_set_blocking", "socketsetblocking", "procedure", "Void",
     "socket_set_blocking(socket: Int, blocking: Bool) -> Void",
     "Switch a socket between blocking and non-blocking mode.", "network", true, "vm_builtin"},
    {"socket_poll", "socketpoll", "function", "Int",
     "socket_poll(socket: Int, timeoutMs: Int, flags: Int) -> Int",
     "flags 1 = readable, 2 = writable; 0 on timeout, else the ready bits.", "network", true, "vm_builtin"},
    {"socket_close", "socketclose", "procedure", "Void",
     "socket_close(socket: Int) -> Void",
     "Close a socket.", "network", true, "vm_builtin"},
    {"socket_last_error", "socketlasterror", "function", "Int",
     "socket_last_error() -> Int",
     "Last socket error code (errno), 0 if the last call succeeded.", "network", true, "vm_builtin"},
    {"dns_lookup", "dnslookup", "function", "Text",
     "dns_lookup(hostname: Text) -> Text",
     "Forward lookup; the address, or \"\" on failure.", "network", true, "vm_builtin"},
    /* Filesystem mutation (effectful). */
    {"mkdir", NULL, "function", "Int",
     "mkdir(path: Text) -> Int",
     "Create a directory; returns 0 on success (a directory that already exists counts) or an errno.", "filesystem", true, "vm_builtin"},
    {"rmdir", NULL, "function", "Int",
     "rmdir(path: Text) -> Int",
     "Remove an empty directory; returns 0 on success or an errno.", "filesystem", true, "vm_builtin"},
    /* SQLite (effectful; integer db/stmt handles). */
    {"sqlite_open", "sqliteopen", "function", "Int",
     "sqlite_open(path: Text) -> Int",
     "Open a database and return its handle; handles count from 0, so the first open returns 0. A failed open stops the program with the SQLite message.", "sqlite", true, "vm_builtin"},
    {"sqlite_close", "sqliteclose", "function", "Int",
     "sqlite_close(db: Int) -> Int",
     "Close a database handle.", "sqlite", true, "vm_builtin"},
    {"sqlite_exec", "sqliteexec", "function", "Int",
     "sqlite_exec(db: Int, sql: Text) -> Int",
     "Execute SQL with no result rows; returns 0. A SQL error stops the program with SQLite's message (try_sqlite_exec reports it instead).", "sqlite", true, "vm_builtin"},
    {"sqlite_prepare", "sqliteprepare", "function", "Int",
     "sqlite_prepare(db: Int, sql: Text) -> Int",
     "Compile a statement; returns a stmt handle. A SQL error stops the program with SQLite's message (try_sqlite_prepare reports it instead).", "sqlite", true, "vm_builtin"},
    {"try_sqlite_exec", NULL, "function", "(Int, Bool)",
     "try_sqlite_exec(db: Int, sql: Text) -> (Int, Bool)",
     "(rc, ok): SQLite's result code and rc == 0; a SQL error never stops the program, and sqlite_errmsg(db) reads its message.", "sqlite", true},
    {"try_sqlite_prepare", NULL, "function", "(Int, Bool)",
     "try_sqlite_prepare(db: Int, sql: Text) -> (Int, Bool)",
     "(stmt, ok): a statement handle, or -1 and ok false when the SQL does not compile; sqlite_errmsg(db) reads why.", "sqlite", true},
    {"sqlite_finalize", "sqlitefinalize", "function", "Int",
     "sqlite_finalize(stmt: Int) -> Int",
     "Release a prepared statement.", "sqlite", true, "vm_builtin"},
    {"sqlite_step", "sqlitestep", "function", "Int",
     "sqlite_step(stmt: Int) -> Int",
     "Advance a statement; returns the step result code.", "sqlite", true, "vm_builtin"},
    {"sqlite_reset", "sqlitereset", "function", "Int",
     "sqlite_reset(stmt: Int) -> Int",
     "Reset a statement for re-execution.", "sqlite", true, "vm_builtin"},
    {"sqlite_column_count", "sqlitecolumncount", "function", "Int",
     "sqlite_column_count(stmt: Int) -> Int",
     "Number of result columns.", "sqlite", true, "vm_builtin"},
    {"sqlite_column_type", "sqlitecolumntype", "function", "Text",
     "sqlite_column_type(stmt: Int, column: Int) -> Text",
     "SQLite storage class of a result column: \"INTEGER\", \"FLOAT\", \"TEXT\", \"BLOB\" or \"NULL\".", "sqlite", true, "vm_builtin"},
    {"sqlite_column_name", "sqlitecolumnname", "function", "Text",
     "sqlite_column_name(stmt: Int, column: Int) -> Text",
     "Name of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlite_column_int", "sqlitecolumnint", "function", "Int",
     "sqlite_column_int(stmt: Int, column: Int) -> Int",
     "Int value of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlite_column_double", "sqlitecolumndouble", "function", "Real",
     "sqlite_column_double(stmt: Int, column: Int) -> Real",
     "Real value of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlite_column_text", "sqlitecolumntext", "function", "Text",
     "sqlite_column_text(stmt: Int, column: Int) -> Text",
     "Text value of a result column.", "sqlite", true, "vm_builtin"},
    {"sqlite_bind_text", "sqlitebindtext", "function", "Int",
     "sqlite_bind_text(stmt: Int, index: Int, value: Text) -> Int",
     "Bind Text to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlite_bind_int", "sqlitebindint", "function", "Int",
     "sqlite_bind_int(stmt: Int, index: Int, value: Int) -> Int",
     "Bind Int to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlite_bind_double", "sqlitebinddouble", "function", "Int",
     "sqlite_bind_double(stmt: Int, index: Int, value: Real) -> Int",
     "Bind Real to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlite_bind_null", "sqlitebindnull", "function", "Int",
     "sqlite_bind_null(stmt: Int, index: Int) -> Int",
     "Bind NULL to a 1-based parameter.", "sqlite", true, "vm_builtin"},
    {"sqlite_clear_bindings", "sqliteclearbindings", "function", "Int",
     "sqlite_clear_bindings(stmt: Int) -> Int",
     "Clear all bound parameters on a statement.", "sqlite", true, "vm_builtin"},
    {"sqlite_errmsg", "sqliteerrmsg", "function", "Text",
     "sqlite_errmsg(db: Int) -> Text",
     "Most recent error message for a database (after a failed sqlite_step, try_sqlite_exec or try_sqlite_prepare).", "sqlite", true, "vm_builtin"},
    {"sqlite_last_insert_rowid", "sqlitelastinsertrowid", "function", "Int",
     "sqlite_last_insert_rowid(db: Int) -> Int",
     "Rowid of the most recent insert.", "sqlite", true, "vm_builtin"},
    {"sqlite_changes", "sqlitechanges", "function", "Int",
     "sqlite_changes(db: Int) -> Int",
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

/* Equal ignoring case and underscores: the dispatch table knows
 * `threadpoolsubmit` where the listing says `thread_pool_submit`. */
static bool builtinNamesMatch(const char *a, const char *b) {
  for (;;) {
    while (*a == '_') a++;
    while (*b == '_') b++;
    if (!*a || !*b) return *a == *b;
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
}

/* The Aether spelling of an engine builtin (`thread_spawn_named` ->
 * `task_spawn`), so a run-time message names what the program wrote; NULL
 * when the listing has no Aether entry for it. The first entry wins. */
const char *pscalAetherNameForBuiltin(const char *backend_name) {
  if (!backend_name) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(kAetherBuiltinMeta) / sizeof(kAetherBuiltinMeta[0]); ++i) {
    const AetherBuiltinMeta *meta = &kAetherBuiltinMeta[i];
    if (meta->backend_name && builtinNamesMatch(meta->backend_name, backend_name)) {
      return meta->aether_name;
    }
  }
  return NULL;
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
  /* An alias is as effectful as the builtin it names (file_exists ->
   * fileexists); the alias itself is not registered. A front-end helper with
   * no engine builtin of its own (try_read_file, a declared fn over hidden
   * builtins) has nothing live to ask, so its entry's flag stands. */
  bool effectful = pscalBuiltinNameEffectMaskLive(meta->aether_name) != FX_PURE ||
                   (meta->backend_name &&
                    pscalBuiltinNameEffectMaskLive(meta->backend_name) != FX_PURE) ||
                   (!meta->backend_name && meta->effectful);
  if (!jsonTextAppend(json, effectful ? "true" : "false")) return false;
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

  /* Since D70 an Aether program writes a camelCase engine name (apiSend) in
   * lower case; the registered spelling itself is SCOPE-001. */
  char lowered[128];
  const char *shown = name;
  if (frontendIsAether() && strlen(name) < sizeof(lowered)) {
    size_t n = 0;
    for (; name[n]; ++n) lowered[n] = (char)tolower((unsigned char)name[n]);
    lowered[n] = '\0';
    shown = lowered;
  }

  if (!jsonTextAppend(json, "{")) return false;
  if (!jsonTextAppend(json, "\"name\":")) return false;
  if (!jsonTextAppendEscaped(json, shown)) return false;
  if (!jsonTextAppend(json, ",\"source\":\"vm_builtin\"")) return false;
  if (!jsonTextAppend(json, ",\"kind\":")) return false;
  if (!jsonTextAppendEscaped(json, builtinTypeName(type))) return false;
  if (!jsonTextAppend(json, ",\"category\":")) return false;
  if (!jsonTextAppendEscaped(json, category)) return false;
  if (!jsonTextAppend(json, ",\"effectful\":")) return false;
  if (!jsonTextAppend(json, pscalBuiltinNameEffectMaskLive(name) != FX_PURE ? "true" : "false")) return false;
  if (group && strcasecmp(group, kDefaultGroupName) != 0) {
    if (!jsonTextAppend(json, ",\"group\":")) return false;
    if (!jsonTextAppendEscaped(json, group)) return false;
  }
  if (detailed) {
    char usage[256];
    snprintf(usage, sizeof(usage), "%s(...)", shown);
    if (!jsonTextAppend(json, ",\"usage\":")) return false;
    if (!jsonTextAppendEscaped(json, usage)) return false;
  }
  return jsonTextAppend(json, "}");
}

/* Under Aether the listings name only what a program can call as spelled: no
 * engine name the front end rejects (copy, fileexists, sqliteopen: D55, D72),
 * and none with no handler in this build (the SDL placeholders of a build
 * without SDL). Both used to be listed as Aether-visible and were SCOPE-001. */
static bool vmBuiltinHiddenFromAether(int id, const char *name) {
  if (!frontendIsAether()) {
    return false;
  }
  return !getVmBuiltinHandlerById(id) || builtinNameRejectedByFrontend(name);
}

void pscalAetherForEachBuiltinName(void (*fn)(const char *name, const char *signature, void *ctx),
                                   void *ctx) {
  if (!fn) {
    return;
  }
  for (size_t i = 0; i < sizeof(kAetherBuiltinMeta) / sizeof(kAetherBuiltinMeta[0]); ++i) {
    fn(kAetherBuiltinMeta[i].aether_name, kAetherBuiltinMeta[i].signature, ctx);
  }
  size_t builtin_count = getVmBuiltinCount();
  for (size_t i = 0; i < builtin_count; ++i) {
    const char *name = getVmBuiltinNameById((int)i);
    if (!name || !*name || vmBuiltinHiddenFromAether((int)i, name)) {
      continue;
    }
    fn(name, NULL, ctx);
  }
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
    if (!name || !*name || vmBuiltinHiddenFromAether((int)i, name)) {
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
    if (id < 0 || vmBuiltinHiddenFromAether(id, name)) {
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

/* has_builtin's yes. Under Aether "can I call this?" (the guides' wording)
 * includes this run's --deny (D80): a builtin the policy refuses answers false,
 * because calling it would stop the program with AETH-RUNTIME-DENIED. It used
 * to answer true and the documented guard, `if has_builtin(...) { call }`,
 * then died in the call. `engine` is the registered name whose effect mask
 * the dispatch gate checks; halt and exit are never refused. */
static Value hasBuiltinAnswer(const char *engine) {
  if (frontendIsAether() && engine) {
    EffectMask mask = pscalBuiltinNameEffectMaskLive(engine);
    if ((mask & pscalFxEffectiveDeniedMask()) && !pscalFxBuiltinIsDenyExempt(engine)) {
      return makeBoolean(0);
    }
  }
  return makeBoolean(1);
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
  if (extBuiltinHasFunction(category, func)) {
    return hasBuiltinAnswer(func);
  }
  /* The front end's own spelling of a builtin (Aether's "file_exists",
   * "sqlite_open", "println"). Only the engine names were found before, so
   * the documented Aether names all answered false while the run-together
   * engine spellings D72 removed from the language answered true. */
  const char *engine = resolveBuiltinNameAlias(func);
  if (engine) {
    if (extBuiltinHasFunction(category, engine) || getVmBuiltinID(engine) >= 0) {
      return hasBuiltinAnswer(engine);
    }
    /* println and print compile to the writeln/write statements, which are
     * not engine functions. */
    if (strcasecmp(engine, "writeln") == 0 || strcasecmp(engine, "write") == 0) {
      return hasBuiltinAnswer(engine);
    }
    /* An alias whose engine builtin this build lacks (sqlite_open in a build
     * without SQLite): not callable. */
    return makeBoolean(0);
  }
  if (frontendIsAether()) {
    /* Builtins the Aether front end implements itself (has_toon,
     * aether_version): documented with no engine function behind them. */
    const AetherBuiltinMeta *meta = findAetherBuiltinMeta(func);
    if (meta && !meta->backend_name && !meta->source) {
      /* try_sqlite_exec/try_sqlite_prepare run on an SQLite engine builtin. */
      if (strncasecmp(func, "try_sqlite_", 11) == 0) {
        return getVmBuiltinID("aether_try_sqlite_exec") >= 0
                   ? hasBuiltinAnswer("aether_try_sqlite_exec") : makeBoolean(0);
      }
      return makeBoolean(1);
    }
  }
  /* Fall back to the core VM registry.
   *
   * Searching only the extended registry made this probe *inverted* for core
   * builtins rather than merely unhelpful: has_builtin("network",
   * "SocketCreate") answered false on a build where socket_create(0) returns a
   * working handle, so a guard written the obvious way reported a working
   * capability as missing and skipped the code that would have worked. Callers
   * ask "can I call this?", and for a core builtin the answer is always yes.
   *
   * The category is deliberately ignored on this path: core builtins are not
   * filed under one, so there is no category that could match. That makes
   * has_builtin("anything", "socketcreate") true, which is the honest reading --
   * the function is the question, and the category only ever narrowed the
   * extended registry. */
  return getVmBuiltinID(func) >= 0 ? hasBuiltinAnswer(func) : makeBoolean(0);
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

/* The documented Aether names (the table above) as engine names, for the
 * builtins that take a builtin name at run time (resolveBuiltinNameAlias).
 * The Aether front end registers its own alias table too; this one also
 * covers a cached program run by a bare VM. */
static const char *aetherMetaBackendName(const char *name) {
  const AetherBuiltinMeta *meta = findAetherBuiltinMeta(name);
  return meta ? meta->backend_name : NULL;
}

void registerExtBuiltinQueryBuiltins(void) {
  registerBuiltinNameAlias(FRONTEND_KIND_AETHER, aetherMetaBackendName);
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
