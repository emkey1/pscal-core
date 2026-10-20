#include "core/utils.h"
#include "backend_ast/builtin.h"
#include "third_party/yyjson/yyjson.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define YYJSON_UNUSED_HANDLE (-1)

typedef enum {
    JSON_HANDLE_UNUSED = 0,
    JSON_HANDLE_DOC,
    JSON_HANDLE_VAL
} JsonHandleKind;

typedef struct {
    JsonHandleKind kind;
    yyjson_doc *doc;
    yyjson_val *val;
    size_t refcount;
    int doc_handle;
} JsonHandleEntry;

static JsonHandleEntry *jsonHandleTable = NULL;
static size_t jsonHandleCapacity = 0;
static pthread_mutex_t jsonHandleMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jsonHandleCond = PTHREAD_COND_INITIALIZER;

static void jsonResetEntry(JsonHandleEntry *entry) {
    if (!entry) return;
    entry->kind = JSON_HANDLE_UNUSED;
    entry->doc = NULL;
    entry->val = NULL;
    entry->refcount = 0;
    entry->doc_handle = YYJSON_UNUSED_HANDLE;
}

static size_t jsonFindFreeSlotLocked(void) {
    size_t slot = jsonHandleCapacity;
    for (size_t i = 0; i < jsonHandleCapacity; ++i) {
        if (jsonHandleTable[i].kind == JSON_HANDLE_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == jsonHandleCapacity) {
        size_t new_capacity = jsonHandleCapacity ? jsonHandleCapacity * 2 : 16;
        JsonHandleEntry *new_table = realloc(jsonHandleTable, sizeof(JsonHandleEntry) * new_capacity);
        if (!new_table) {
            return (size_t)-1;
        }
        for (size_t i = jsonHandleCapacity; i < new_capacity; ++i) {
            jsonResetEntry(&new_table[i]);
        }
        jsonHandleTable = new_table;
        slot = jsonHandleCapacity;
        jsonHandleCapacity = new_capacity;
    }
    return slot;
}

static int jsonAllocDocHandle(yyjson_doc *doc) {
    if (!doc) {
        return YYJSON_UNUSED_HANDLE;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t slot = jsonFindFreeSlotLocked();
    if (slot == (size_t)-1) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    JsonHandleEntry *entry = &jsonHandleTable[slot];
    entry->kind = JSON_HANDLE_DOC;
    entry->doc = doc;
    entry->val = NULL;
    entry->refcount = 0;
    entry->doc_handle = YYJSON_UNUSED_HANDLE;

    pthread_mutex_unlock(&jsonHandleMutex);
    return (int)slot;
}

static size_t jsonFindDocIndexLocked(yyjson_doc *doc) {
    for (size_t i = 0; i < jsonHandleCapacity; ++i) {
        JsonHandleEntry *entry = &jsonHandleTable[i];
        if (entry->kind == JSON_HANDLE_DOC && entry->doc == doc) {
            return i;
        }
    }
    return (size_t)-1;
}

static int jsonAllocValueHandle(yyjson_doc *doc, yyjson_val *val) {
    if (!doc || !val) {
        return YYJSON_UNUSED_HANDLE;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t doc_index = jsonFindDocIndexLocked(doc);
    if (doc_index == (size_t)-1) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    JsonHandleEntry *doc_entry = &jsonHandleTable[doc_index];
    if (doc_entry->doc == NULL) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    size_t slot = jsonFindFreeSlotLocked();
    if (slot == (size_t)-1) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return YYJSON_UNUSED_HANDLE;
    }

    JsonHandleEntry *entry = &jsonHandleTable[slot];
    entry->kind = JSON_HANDLE_VAL;
    entry->doc = doc;
    entry->val = val;
    entry->refcount = 0;
    entry->doc_handle = (int)doc_index;

    pthread_mutex_unlock(&jsonHandleMutex);
    return (int)slot;
}

static bool jsonAcquireDoc(int handle, yyjson_doc **out_doc) {
    if (handle < 0 || !out_doc) {
        return false;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool ok = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_DOC && entry->doc) {
            entry->refcount++;
            *out_doc = entry->doc;
            ok = true;
        }
    }
    if (!ok) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return false;
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return true;
}

static void jsonReleaseDoc(int handle) {
    if (handle < 0) {
        return;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_DOC && entry->refcount > 0) {
            entry->refcount--;
            pthread_cond_broadcast(&jsonHandleCond);
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
}

static bool jsonAcquireValue(int handle, yyjson_doc **out_doc, yyjson_val **out_val, int *out_doc_handle) {
    if (handle < 0 || !out_doc || !out_val) {
        return false;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool ok = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL && entry->doc && entry->val) {
            int doc_handle = entry->doc_handle;
            if (doc_handle >= 0 && (size_t)doc_handle < jsonHandleCapacity) {
                JsonHandleEntry *doc_entry = &jsonHandleTable[doc_handle];
                if (doc_entry->kind == JSON_HANDLE_DOC && doc_entry->doc == entry->doc) {
                    if (doc_entry->doc) {
                        entry->refcount++;
                        doc_entry->refcount++;
                        *out_doc = entry->doc;
                        *out_val = entry->val;
                        if (out_doc_handle) {
                            *out_doc_handle = doc_handle;
                        }
                        ok = true;
                    }
                }
            }
        }
    }
    if (!ok) {
        pthread_mutex_unlock(&jsonHandleMutex);
        return false;
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return true;
}

static void jsonReleaseValue(int handle) {
    if (handle < 0) {
        return;
    }

    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL && entry->refcount > 0) {
            entry->refcount--;
            int doc_handle = entry->doc_handle;
            if (doc_handle >= 0 && (size_t)doc_handle < jsonHandleCapacity) {
                JsonHandleEntry *doc_entry = &jsonHandleTable[doc_handle];
                if (doc_entry->kind == JSON_HANDLE_DOC && doc_entry->refcount > 0) {
                    doc_entry->refcount--;
                }
            }
            pthread_cond_broadcast(&jsonHandleCond);
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
}

static bool jsonReleaseValueHandle(int handle) {
    if (handle < 0) return false;
    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    bool released = false;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_VAL) {
            while (entry->refcount > 0) {
                pthread_cond_wait(&jsonHandleCond, &jsonHandleMutex);
            }
            jsonResetEntry(entry);
            pthread_cond_broadcast(&jsonHandleCond);
            released = true;
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return released;
}

static yyjson_doc *jsonDetachDocHandle(int handle) {
    if (handle < 0) return NULL;
    pthread_mutex_lock(&jsonHandleMutex);
    size_t idx = (size_t)handle;
    yyjson_doc *doc = NULL;
    if (idx < jsonHandleCapacity) {
        JsonHandleEntry *entry = &jsonHandleTable[idx];
        if (entry->kind == JSON_HANDLE_DOC && entry->doc) {
            doc = entry->doc;
            entry->doc = NULL;
            while (entry->refcount > 0) {
                pthread_cond_wait(&jsonHandleCond, &jsonHandleMutex);
            }
            for (size_t i = 0; i < jsonHandleCapacity; ++i) {
                JsonHandleEntry *val_entry = &jsonHandleTable[i];
                if (val_entry->kind == JSON_HANDLE_VAL && val_entry->doc == doc) {
                    val_entry->doc = NULL;
                    while (val_entry->refcount > 0) {
                        pthread_cond_wait(&jsonHandleCond, &jsonHandleMutex);
                    }
                    jsonResetEntry(val_entry);
                }
            }
            jsonResetEntry(entry);
            pthread_cond_broadcast(&jsonHandleCond);
        }
    }
    pthread_mutex_unlock(&jsonHandleMutex);
    return doc;
}

static const char *jsonTypeToString(yyjson_val *val) {
    switch (yyjson_get_type(val)) {
        case YYJSON_TYPE_NULL: return "null";
        case YYJSON_TYPE_BOOL: return "bool";
        case YYJSON_TYPE_NUM:
            return yyjson_is_int(val) ? "int" : "real";
        case YYJSON_TYPE_STR: return "string";
        case YYJSON_TYPE_ARR: return "array";
        case YYJSON_TYPE_OBJ: return "object";
        case YYJSON_TYPE_RAW: return "raw";
        default: return "unknown";
    }
}

static const char *jsonFindSuggestedArrayField(yyjson_val *val) {
    yyjson_obj_iter iter;
    yyjson_val *key = NULL;

    if (!val || !yyjson_is_obj(val)) {
        return NULL;
    }
    iter = yyjson_obj_iter_with(val);
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val *child = yyjson_obj_iter_get_val(key);
        if (child && yyjson_is_arr(child)) {
            return yyjson_get_str(key);
        }
    }
    return NULL;
}

static Value vmBuiltinYyjsonRead(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "YyjsonRead expects a single string argument.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    const char *json = AS_STRING(args[0]) ? AS_STRING(args[0]) : "";
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *)json, strlen(json), 0, NULL, &err);
    if (!doc) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-PARSE] toon_parse could not parse the input at position %zu: %s. hint: the text must be valid JSON; check for unquoted keys, trailing commas, or single quotes.", err.pos, err.msg ? err.msg : "unknown error");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = jsonAllocDocHandle(doc);
    if (handle == YYJSON_UNUSED_HANDLE) {
        yyjson_doc_free(doc);
        runtimeError(vm, "YyjsonRead: unable to allocate document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    return makeInt(handle);
}

static Value vmBuiltinYyjsonReadFile(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "YyjsonReadFile expects a single string argument.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    const char *path = AS_STRING(args[0]) ? AS_STRING(args[0]) : "";
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(path, 0, NULL, &err);
    if (!doc) {
        runtimeError(vm, "YyjsonReadFile failed at position %zu: %s", err.pos, err.msg ? err.msg : "unknown error");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = jsonAllocDocHandle(doc);
    if (handle == YYJSON_UNUSED_HANDLE) {
        yyjson_doc_free(doc);
        runtimeError(vm, "YyjsonReadFile: unable to allocate document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    return makeInt(handle);
}

/* D73 checked I/O, the hidden half of try_toon_parse_file: never a runtime
 * error for a missing file or bad JSON. A failure records ok = false and still
 * returns a usable document (JSON `null`), so toon_root / toon_close and every
 * accessor degrade instead of stopping the program. */
static Value vmBuiltinAetherTryToonReadFile(struct VM_s *vm, int arg_count, Value *args) {
    pscalAetherSetLastIoOk(false);
    if (arg_count != 1 || !isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "try_toon_parse_file expects a single Text path.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    const char *path = AS_STRING(args[0]) ? AS_STRING(args[0]) : "";
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(path, 0, NULL, &err);
    bool ok = doc != NULL;
    if (!doc) doc = yyjson_read("null", 4, 0);
    if (!doc) {
        runtimeError(vm, "try_toon_parse_file: unable to allocate a document.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = jsonAllocDocHandle(doc);
    if (handle == YYJSON_UNUSED_HANDLE) {
        yyjson_doc_free(doc);
        runtimeError(vm, "try_toon_parse_file: unable to allocate document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    pscalAetherSetLastIoOk(ok);
    return makeInt(handle);
}

static Value vmBuiltinYyjsonDocFree(struct VM_s *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonDocFree expects a single document handle.");
        return makeVoid();
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = jsonDetachDocHandle(handle);
    if (!doc) {
        runtimeError(vm, "YyjsonDocFree received an invalid document handle (%d).", handle);
        return makeVoid();
    }
    yyjson_doc_free(doc);
    return makeVoid();
}

static Value vmBuiltinYyjsonFreeValue(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonFreeValue expects a single value handle.");
        return makeVoid();
    }
    int handle = (int)AS_INTEGER(args[0]);
    if (!jsonReleaseValueHandle(handle)) {
    }
    return makeVoid();
}

static Value vmBuiltinYyjsonGetRoot(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetRoot expects a single document handle.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    if (!jsonAcquireDoc(handle, &doc)) {
        runtimeError(vm, "YyjsonGetRoot received an invalid document handle (%d).", handle);
        return makeInt(YYJSON_UNUSED_HANDLE);
    }

    Value result = makeInt(YYJSON_UNUSED_HANDLE);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        runtimeError(vm, "YyjsonGetRoot: document has no root value.");
        goto cleanup;
    }

    int value_handle = jsonAllocValueHandle(doc, root);
    if (value_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetRoot: unable to allocate value handle.");
        goto cleanup;
    }

    result = makeInt(value_handle);

cleanup:
    jsonReleaseDoc(handle);
    return result;
}

/* Resolve `key` under `val`. A dotted key ("meta.code") walks nested objects.
 * A missing key, an empty key, or a non-object anywhere along the path yields
 * NULL. */
static yyjson_val *jsonResolveKeyPath(yyjson_val *val, const char *key) {
    yyjson_val *child = val;
    if (!key) {
        key = "";
    }
    if (strchr(key, '.') != NULL) {
        char *path = strdup(key);
        if (path == NULL) {
            return NULL;
        }
        char *save = NULL;
        char *seg = strtok_r(path, ".", &save);
        while (seg != NULL) {
            child = (child != NULL && yyjson_is_obj(child)) ? yyjson_obj_get(child, seg) : NULL;
            seg = strtok_r(NULL, ".", &save);
        }
        free(path);
        return child;
    }
    return yyjson_is_obj(val) ? yyjson_obj_get(val, key) : NULL;
}

static Value vmBuiltinYyjsonGetKey(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !isPascalStringType(VALUE_TYPE(args[1]))) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-KEY-ARGS] toon_key expects (node: ToonNode, key: Text). hint: pass a node from toon_root/toon_at/toon_key, not a ToonDoc -- use toon_root(doc) first.");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt(YYJSON_UNUSED_HANDLE);
    }

    Value result = makeInt(YYJSON_UNUSED_HANDLE);
    const char *key = AS_STRING(args[1]) ? AS_STRING(args[1]) : "";
    /* Dotted paths ("meta.code") walk nested objects. A missing key, an empty
     * key, or a non-object anywhere along the path yields an absent (invalid)
     * handle that downstream accessors degrade on -- so deep reads need no
     * manual per-level staging, and toon_get_text_or(n, "a.b", d) just works. */
    yyjson_val *child = jsonResolveKeyPath(val, key);
    if (child == NULL) {
        goto cleanup;
    }

    int child_handle = jsonAllocValueHandle(doc, child);
    if (child_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "YyjsonGetKey: unable to allocate value handle.");
        goto cleanup;
    }

    result = makeInt(child_handle);

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetIndex(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-GET-INDEX-ARITY] YyjsonGetIndex expects (value_handle:int, index:int).");
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    int handle = (int)AS_INTEGER(args[0]);
    long long index = AS_INTEGER(args[1]);
    if (index < 0) {
        return makeInt(YYJSON_UNUSED_HANDLE);
    }
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt(YYJSON_UNUSED_HANDLE);
    }

    Value result = makeInt(YYJSON_UNUSED_HANDLE);
    if (!yyjson_is_arr(val)) {
        const char *actualType = jsonTypeToString(val);
        const char *suggestedField = jsonFindSuggestedArrayField(val);
        if (suggestedField && *suggestedField) {
            runtimeError(vm,
                         "[AETH-RUNTIME-TOON-GET-INDEX-ARRAY] toon_at expects an array ToonNode, but got %s. "
                         "hint: if this is an object root, extract its array field first, for example "
                         "toon_key(root, \"%s\").",
                         actualType,
                         suggestedField);
        } else {
            runtimeError(vm,
                         "[AETH-RUNTIME-TOON-GET-INDEX-ARRAY] toon_at expects an array ToonNode, but got %s. "
                         "hint: use toon_key(node, \"field\") to reach the array first.",
                         actualType);
        }
        goto cleanup;
    }

    yyjson_val *child = yyjson_arr_get(val, (size_t)index);
    if (!child) {
        goto cleanup;
    }

    int child_handle = jsonAllocValueHandle(doc, child);
    if (child_handle == YYJSON_UNUSED_HANDLE) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-GET-INDEX-ALLOC] YyjsonGetIndex: unable to allocate value handle.");
        goto cleanup;
    }

    result = makeInt(child_handle);

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonHasKey(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !isPascalStringType(VALUE_TYPE(args[1]))) {
        runtimeError(vm, "YyjsonHasKey expects (value_handle:int, key:string).");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt(0);
    }

    Value result = makeInt(0);
    {
        yyjson_val *child = jsonResolveKeyPath(val, AS_STRING(args[1]));
        result = makeInt(child ? 1 : 0);
    }

    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonHasIndex(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-HAS-INDEX-ARITY] YyjsonHasIndex expects (value_handle:int, index:int).");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    long long index = AS_INTEGER(args[1]);
    if (index < 0) {
        return makeInt(0);
    }
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt(0);
    }

    Value result = makeInt(0);
    if (!yyjson_is_arr(val)) {
        const char *actualType = jsonTypeToString(val);
        const char *suggestedField = jsonFindSuggestedArrayField(val);
        if (suggestedField && *suggestedField) {
            runtimeError(vm,
                         "[AETH-RUNTIME-TOON-HAS-INDEX-ARRAY] YyjsonHasIndex requires an array value handle, got %s. "
                         "If this is an object root, extract its array field first, for example "
                         "toon_key(root, \"%s\").",
                         actualType,
                         suggestedField);
        } else {
            runtimeError(vm,
                         "[AETH-RUNTIME-TOON-HAS-INDEX-ARRAY] YyjsonHasIndex requires an array value handle, got %s.",
                         actualType);
        }
        goto cleanup;
    }

    result = makeInt(yyjson_arr_get(val, (size_t)index) ? 1 : 0);

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetLength(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetLength expects a single value handle.");
        return makeInt(-1);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        /* An absent node (a missing key, a dotted miss, toon_at past the end)
           is an empty collection, like null: toon_len 0, never -1. */
        return makeInt(0);
    }

    Value result = makeInt(-1);
    if (yyjson_is_arr(val)) {
        result = makeInt((long long)yyjson_arr_size(val));
        goto cleanup;
    }
    if (yyjson_is_obj(val)) {
        result = makeInt((long long)yyjson_obj_size(val));
        goto cleanup;
    }
    if (yyjson_is_null(val)) {
        /* A JSON null is an empty collection for length purposes: toon_null() and
           a key that resolved to null degrade to 0 rather than crashing, matching
           the missing-key / type-mismatch degrade used by the value getters. */
        result = makeInt(0);
        goto cleanup;
    }

    runtimeError(vm, "YyjsonGetLength requires an array or object value handle.");

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetType(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonGetType expects a single value handle.");
        return makeString("");
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeString("");
    }

    Value result = makeString(jsonTypeToString(val));
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetString(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-GET-STRING-ARITY] YyjsonGetString expects a single value handle.");
        return makeString("");
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeString("");
    }

    Value result = makeString("");
    if (!yyjson_is_str(val)) {
        /* Type mismatch degrades to the default (empty), mirroring the missing-key
           / invalid-handle degrade from c30d16260: a defensible program must not
           crash because a field holds an unexpected type. */
        goto cleanup;
    }

    const char *str = yyjson_get_str(val);
    result = makeString(str ? str : "");

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetNumber(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-GET-NUMBER-ARITY] YyjsonGetNumber expects a single value handle.");
        return makeDouble(0.0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeDouble(0.0);
    }

    Value result = makeDouble(0.0);
    if (yyjson_is_real(val)) {
        result = makeDouble(yyjson_get_real(val));
        goto cleanup;
    }
    if (yyjson_is_int(val)) {
        result = makeDouble((double)yyjson_get_sint(val));
        goto cleanup;
    }

    /* Type mismatch degrades to the default (0.0), mirroring the missing-key /
       invalid-handle degrade from c30d16260. */

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-GET-INT-ARITY] toon_int_value expects a single ToonNode. hint: to read a keyed field use toon_get_int(node, key).");
        return makeInt64(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt64(0);
    }

    Value result = makeInt64(0);
    if (!yyjson_is_int(val)) {
        /* Type mismatch degrades to the default (0), mirroring the missing-key /
           invalid-handle degrade from c30d16260. */
        goto cleanup;
    }

    result = makeInt64(yyjson_get_sint(val));

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetBool(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "[AETH-RUNTIME-TOON-GET-BOOL-ARITY] YyjsonGetBool expects a single value handle.");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt(0);
    }

    Value result = makeInt(0);
    if (!yyjson_is_bool(val)) {
        /* Type mismatch degrades to the default (false), mirroring the missing-key
           / invalid-handle degrade from c30d16260. */
        goto cleanup;
    }

    result = makeInt(yyjson_get_bool(val) ? 1 : 0);

cleanup:
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonIsNull(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "YyjsonIsNull expects a single value handle.");
        return makeInt(0);
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return makeInt(0);
    }

    Value result = makeInt(yyjson_is_null(val) ? 1 : 0);
    jsonReleaseValue(handle);
    return result;
}

typedef enum {
    JSON_GET_OR_TEXT,
    JSON_GET_OR_INT,
    JSON_GET_OR_REAL,
    JSON_GET_OR_BOOL
} JsonGetOrKind;

/* The fallback, converted to the getter's result type. */
static Value jsonGetOrFallback(JsonGetOrKind kind, Value fallback) {
    switch (kind) {
        case JSON_GET_OR_TEXT:
            if (IS_CHAR(fallback)) {
                char ch = (char)AS_CHAR(fallback);
                return makeStringLen(&ch, 1);
            }
            if (isPascalStringType(VALUE_TYPE(fallback))) {
                return makeString(AS_STRING(fallback) ? AS_STRING(fallback) : "");
            }
            return makeString("");
        case JSON_GET_OR_INT:
            return makeInt64(IS_NUMERIC(fallback) ? (long long)AS_INTEGER(fallback) : 0);
        case JSON_GET_OR_REAL:
            if (IS_REAL(fallback)) {
                return makeDouble((double)AS_REAL(fallback));
            }
            return makeDouble(IS_INTLIKE(fallback) ? (double)AS_INTEGER(fallback) : 0.0);
        case JSON_GET_OR_BOOL:
            return makeBoolean(IS_INTLIKE(fallback) && AS_INTEGER(fallback) != 0);
    }
    return makeInt(0);
}

/* toon_get_<type>_or(node, key, fallback): the keyed child's value when it has
 * the getter's type, else the fallback -- for a missing key, a JSON null and a
 * wrong-typed value alike. An Int child satisfies the Real getter. The child is
 * resolved in place, so no value handle is allocated. */
static Value jsonGetOrCommon(struct VM_s *vm, int arg_count, Value *args,
                             JsonGetOrKind kind, const char *name) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || !isPascalStringType(VALUE_TYPE(args[1]))) {
        runtimeError(vm, "%s expects (node: ToonNode, key: Text, fallback).", name);
        return arg_count >= 3 ? jsonGetOrFallback(kind, args[2]) : jsonGetOrFallback(kind, makeInt(0));
    }
    int handle = (int)AS_INTEGER(args[0]);
    yyjson_doc *doc = NULL;
    yyjson_val *val = NULL;
    if (!jsonAcquireValue(handle, &doc, &val, NULL)) {
        return jsonGetOrFallback(kind, args[2]);
    }

    yyjson_val *child = jsonResolveKeyPath(val, AS_STRING(args[1]));
    Value result;
    if (child && kind == JSON_GET_OR_TEXT && yyjson_is_str(child)) {
        const char *str = yyjson_get_str(child);
        result = makeString(str ? str : "");
    } else if (child && kind == JSON_GET_OR_INT && yyjson_is_int(child)) {
        result = makeInt64(yyjson_get_sint(child));
    } else if (child && kind == JSON_GET_OR_REAL && yyjson_is_real(child)) {
        result = makeDouble(yyjson_get_real(child));
    } else if (child && kind == JSON_GET_OR_REAL && yyjson_is_int(child)) {
        result = makeDouble((double)yyjson_get_sint(child));
    } else if (child && kind == JSON_GET_OR_BOOL && yyjson_is_bool(child)) {
        result = makeBoolean(yyjson_get_bool(child) ? 1 : 0);
    } else {
        result = jsonGetOrFallback(kind, args[2]);
    }
    jsonReleaseValue(handle);
    return result;
}

static Value vmBuiltinYyjsonGetStringOr(struct VM_s *vm, int arg_count, Value *args) {
    return jsonGetOrCommon(vm, arg_count, args, JSON_GET_OR_TEXT, "toon_get_text_or");
}

static Value vmBuiltinYyjsonGetIntOr(struct VM_s *vm, int arg_count, Value *args) {
    return jsonGetOrCommon(vm, arg_count, args, JSON_GET_OR_INT, "toon_get_int_or");
}

static Value vmBuiltinYyjsonGetNumberOr(struct VM_s *vm, int arg_count, Value *args) {
    return jsonGetOrCommon(vm, arg_count, args, JSON_GET_OR_REAL, "toon_get_real_or");
}

static Value vmBuiltinYyjsonGetBoolOr(struct VM_s *vm, int arg_count, Value *args) {
    return jsonGetOrCommon(vm, arg_count, args, JSON_GET_OR_BOOL, "toon_get_bool_or");
}

void registerYyjsonReadBuiltin(void) {
    registerVmBuiltin("yyjsonread", vmBuiltinYyjsonRead, BUILTIN_TYPE_FUNCTION, "YyjsonRead", FX_PURE);
}

void registerAetherTryToonReadFileBuiltin(void) {
    registerVmBuiltin("aether_try_toon_read_file", vmBuiltinAetherTryToonReadFile,
                      BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
}

void registerYyjsonReadFileBuiltin(void) {
    registerVmBuiltin("yyjsonreadfile", vmBuiltinYyjsonReadFile, BUILTIN_TYPE_FUNCTION, "YyjsonReadFile", FX_IO);
}

void registerYyjsonDocFreeBuiltin(void) {
    registerVmBuiltin("yyjsondocfree", vmBuiltinYyjsonDocFree, BUILTIN_TYPE_PROCEDURE, "YyjsonDocFree", FX_PURE);
}

void registerYyjsonFreeValueBuiltin(void) {
    registerVmBuiltin("yyjsonfreevalue", vmBuiltinYyjsonFreeValue, BUILTIN_TYPE_PROCEDURE, "YyjsonFreeValue", FX_PURE);
}

void registerYyjsonGetRootBuiltin(void) {
    registerVmBuiltin("yyjsongetroot", vmBuiltinYyjsonGetRoot, BUILTIN_TYPE_FUNCTION, "YyjsonGetRoot", FX_PURE);
}

void registerYyjsonGetKeyBuiltin(void) {
    registerVmBuiltin("yyjsongetkey", vmBuiltinYyjsonGetKey, BUILTIN_TYPE_FUNCTION, "YyjsonGetKey", FX_PURE);
}

void registerYyjsonGetIndexBuiltin(void) {
    registerVmBuiltin("yyjsongetindex", vmBuiltinYyjsonGetIndex, BUILTIN_TYPE_FUNCTION, "YyjsonGetIndex", FX_PURE);
}

void registerYyjsonHasKeyBuiltin(void) {
    registerVmBuiltin("yyjsonhaskey", vmBuiltinYyjsonHasKey, BUILTIN_TYPE_FUNCTION, "YyjsonHasKey", FX_PURE);
}

void registerYyjsonHasIndexBuiltin(void) {
    registerVmBuiltin("yyjsonhasindex", vmBuiltinYyjsonHasIndex, BUILTIN_TYPE_FUNCTION, "YyjsonHasIndex", FX_PURE);
}

void registerYyjsonGetLengthBuiltin(void) {
    registerVmBuiltin("yyjsongetlength", vmBuiltinYyjsonGetLength, BUILTIN_TYPE_FUNCTION, "YyjsonGetLength", FX_PURE);
}

void registerYyjsonGetTypeBuiltin(void) {
    registerVmBuiltin("yyjsongettype", vmBuiltinYyjsonGetType, BUILTIN_TYPE_FUNCTION, "YyjsonGetType", FX_PURE);
}

void registerYyjsonGetStringBuiltin(void) {
    registerVmBuiltin("yyjsongetstring", vmBuiltinYyjsonGetString, BUILTIN_TYPE_FUNCTION, "YyjsonGetString", FX_PURE);
}

void registerYyjsonGetNumberBuiltin(void) {
    registerVmBuiltin("yyjsongetnumber", vmBuiltinYyjsonGetNumber, BUILTIN_TYPE_FUNCTION, "YyjsonGetNumber", FX_PURE);
}

void registerYyjsonGetIntBuiltin(void) {
    registerVmBuiltin("yyjsongetint", vmBuiltinYyjsonGetInt, BUILTIN_TYPE_FUNCTION, "YyjsonGetInt", FX_PURE);
}

void registerYyjsonGetBoolBuiltin(void) {
    registerVmBuiltin("yyjsongetbool", vmBuiltinYyjsonGetBool, BUILTIN_TYPE_FUNCTION, "YyjsonGetBool", FX_PURE);
}

void registerYyjsonIsNullBuiltin(void) {
    registerVmBuiltin("yyjsonisnull", vmBuiltinYyjsonIsNull, BUILTIN_TYPE_FUNCTION, "YyjsonIsNull", FX_PURE);
}

void registerYyjsonGetStringOrBuiltin(void) {
    registerVmBuiltin("yyjsongetstringor", vmBuiltinYyjsonGetStringOr, BUILTIN_TYPE_FUNCTION, "YyjsonGetStringOr", FX_PURE);
}

void registerYyjsonGetIntOrBuiltin(void) {
    registerVmBuiltin("yyjsongetintor", vmBuiltinYyjsonGetIntOr, BUILTIN_TYPE_FUNCTION, "YyjsonGetIntOr", FX_PURE);
}

void registerYyjsonGetNumberOrBuiltin(void) {
    registerVmBuiltin("yyjsongetnumberor", vmBuiltinYyjsonGetNumberOr, BUILTIN_TYPE_FUNCTION, "YyjsonGetNumberOr", FX_PURE);
}

void registerYyjsonGetBoolOrBuiltin(void) {
    registerVmBuiltin("yyjsongetboolor", vmBuiltinYyjsonGetBoolOr, BUILTIN_TYPE_FUNCTION, "YyjsonGetBoolOr", FX_PURE);
}
