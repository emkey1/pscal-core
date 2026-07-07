#include "vm/vm_fx_policy.h"
#include "core/cache.h"
#include "core/utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char kFxJournalMagic[4] = {'P', 'S', 'F', 'X'};
#define FX_JOURNAL_VERSION 1

static EffectMask g_fx_cli_denied_mask = FX_PURE;
static bool g_fx_env_checked = false;
static EffectMask g_fx_env_denied_mask = FX_PURE;

static FILE *g_fx_record_file = NULL;
static FILE *g_fx_replay_file = NULL;

bool pscalFxParseDenyList(const char *csv, EffectMask *out_mask) {
    if (!out_mask) return false;
    *out_mask = FX_PURE;
    if (!csv || !*csv) return true;

    char buf[256];
    size_t len = strlen(csv);
    if (len >= sizeof(buf)) return false;
    memcpy(buf, csv, len + 1);

    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    while (tok) {
        while (*tok == ' ') tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && tok[tlen - 1] == ' ') { tok[--tlen] = '\0'; }
        if (*tok) {
            if (strcasecmp(tok, "io") == 0) *out_mask |= FX_IO;
            else if (strcasecmp(tok, "net") == 0) *out_mask |= FX_NET;
            else if (strcasecmp(tok, "proc") == 0) *out_mask |= FX_PROC;
            else if (strcasecmp(tok, "clock") == 0) *out_mask |= FX_CLOCK;
            else if (strcasecmp(tok, "random") == 0) *out_mask |= FX_RANDOM;
            else if (strcasecmp(tok, "all") == 0) *out_mask |= FX_ALL_KNOWN;
            else return false;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    return true;
}

void pscalFxSetDeniedMask(EffectMask mask) {
    g_fx_cli_denied_mask = mask;
}

bool pscalFxPolicyActive(void) {
    return pscalFxEffectiveDeniedMask() != FX_PURE || g_fx_record_file != NULL || g_fx_replay_file != NULL;
}

EffectMask pscalFxEffectiveDeniedMask(void) {
    if (!g_fx_env_checked) {
        g_fx_env_checked = true;
        const char *env = getenv("PSCAL_VM_DENY");
        if (env && *env) {
            EffectMask parsed = FX_PURE;
            if (pscalFxParseDenyList(env, &parsed)) {
                g_fx_env_denied_mask = parsed;
            } else {
                fprintf(stderr, "Warning: ignoring unrecognized PSCAL_VM_DENY value '%s'.\n", env);
            }
        }
    }
    return g_fx_cli_denied_mask | g_fx_env_denied_mask;
}

bool pscalFxBeginRecord(const char *path) {
    if (!path || !*path) return false;
    if (g_fx_record_file || g_fx_replay_file) return false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: could not open '%s' for fx record journal: %s\n", path, strerror(errno));
        return false;
    }
    if (fwrite(kFxJournalMagic, 1, sizeof(kFxJournalMagic), f) != sizeof(kFxJournalMagic) ||
        fputc(FX_JOURNAL_VERSION, f) == EOF) {
        fclose(f);
        return false;
    }
    g_fx_record_file = f;
    return true;
}

bool pscalFxBeginReplay(const char *path) {
    if (!path || !*path) return false;
    if (g_fx_record_file || g_fx_replay_file) return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: could not open '%s' for fx replay journal: %s\n", path, strerror(errno));
        return false;
    }
    char magic[4];
    int version = -1;
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, kFxJournalMagic, sizeof(magic)) != 0 ||
        (version = fgetc(f)) != FX_JOURNAL_VERSION) {
        fprintf(stderr, "Error: '%s' is not a valid fx replay journal (bad magic/version).\n", path);
        fclose(f);
        return false;
    }
    g_fx_replay_file = f;
    return true;
}

bool pscalFxRecordActive(void) { return g_fx_record_file != NULL; }
bool pscalFxReplayActive(void) { return g_fx_replay_file != NULL; }

void pscalFxEndSession(void) {
    if (g_fx_record_file) { fclose(g_fx_record_file); g_fx_record_file = NULL; }
    if (g_fx_replay_file) { fclose(g_fx_replay_file); g_fx_replay_file = NULL; }
}

/* Raw length-prefixed byte blob, used only for MStream buffer content (see
 * the header comment on pscalFxRecordCall for why MStream needs this instead
 * of the generic Value codec). 4-byte little-endian length, not a varint --
 * this is a private, host-local journal format, not the portable PSB3
 * container, so simplicity wins. */
static bool writeRawBlob(FILE *f, const unsigned char *data, uint32_t len) {
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (len > 0 && fwrite(data, 1, len, f) != len) return false;
    return true;
}

/* Allocates len+1 bytes and NUL-terminates after the payload: MStream buffers
 * are conventionally kept NUL-terminated in this codebase (every real
 * network write-callback appends '\0' after growing the buffer, see
 * builtin_network_api.c) because MStreamBuffer() hands the buffer straight to
 * makeString()/strdup(). A replayed buffer missing that terminator is a
 * heap-buffer-overflow waiting for the first strdup -- found via ASan. */
static bool readRawBlob(FILE *f, unsigned char **out_data, uint32_t *out_len) {
    uint8_t hdr[4];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (len > (64u * 1024 * 1024)) return false;
    unsigned char *buf = (unsigned char *)malloc((size_t)len + 1);
    if (!buf) return false;
    if (len > 0 && fread(buf, 1, len, f) != len) {
        free(buf);
        return false;
    }
    buf[len] = '\0';
    *out_data = buf;
    *out_len = len;
    return true;
}

/* Writeback value kinds, tagged so replay knows how to reapply each one. */
#define FX_WB_KIND_GENERIC 0  /* generic Value via pscalCacheWriteValueFramed */
#define FX_WB_KIND_MSTREAM 1  /* raw buffer bytes, replayed into an existing MStream */

/* Best-effort probe: does this Value serialize via the generic codec? Used to
 * decide "substitutable" without corrupting the real journal on failure --
 * writes to a scratch temp file that's discarded either way. */
static bool valueIsGenericSerializable(const Value *v) {
    FILE *scratch = tmpfile();
    if (!scratch) return false;
    bool ok = pscalCacheWriteValueFramed(scratch, v);
    fclose(scratch);
    return ok;
}

/* MStream out-params (HttpRequest's Contents arg, etc.) are passed by
 * ordinary value, not by address -- MStream is already reference-counted
 * shared state (Value.mstream is a pointer into a shared heap struct), so
 * the compiler doesn't need to take the argument's address for the callee to
 * mutate what the caller sees. This is unlike TYPE_FILE, which *is* passed by
 * address (TYPE_POINTER) because each Value owns its FILE* independently.
 * Resolve either shape to the live MStream* so record/replay can treat both
 * uniformly. Returns NULL if arg i isn't an MStream in either shape. */
static MStream *fxResolveMStreamArg(Value *args, int idx) {
    if (VALUE_TYPE(args[idx]) == TYPE_MEMORYSTREAM) {
        return PSCAL_VALUE_PTR(args[idx], MStream);
    }
    if (VALUE_TYPE(args[idx]) == TYPE_POINTER && AS_POINTER(args[idx])) {
        Value *pointee = (Value *)AS_POINTER(args[idx]);
        if (VALUE_TYPE(*pointee) == TYPE_MEMORYSTREAM) {
            return PSCAL_VALUE_PTR(*pointee, MStream);
        }
    }
    return NULL;
}

/* Journal record layout:
 *   name:STRING, arg_count:INTEGER, substitutable:BOOLEAN
 *   -- if substitutable is false, nothing else is written for this call; the
 *      real handler ran live at record time and must run live again on
 *      replay (see PSCAL_FX_REPLAY_RUN_LIVE).
 *   -- if substitutable is true:
 *      result_present:BOOLEAN, [result:ANY]
 *      writeback_count:INTEGER,
 *      { arg_index:INTEGER, kind:INTEGER, payload } * writeback_count
 *        where payload is a Value (kind=GENERIC) or a length-prefixed raw
 *        blob (kind=MSTREAM).
 * Deliberately does NOT record input argument values -- Docs/pscal_vm2_plan.md
 * §6.3 only asks for "return Value + VAR-parameter writebacks", and the
 * program's own deterministic state already supplies the inputs on replay. */
void pscalFxRecordCall(const char *name, int arg_count, const Value *args, const Value *result) {
    if (!g_fx_record_file || !name) return;
    FILE *f = g_fx_record_file;

    bool has_result = result != NULL;
    bool substitutable = !has_result || valueIsGenericSerializable(result);

    /* Pre-pass: decide substitutability and each writeback's kind before
     * committing anything to the real journal file. */
    int wb_indices[256];
    int wb_kinds[256];
    int wb_count = 0;
    if (substitutable) {
        for (int i = 0; i < arg_count && wb_count < 256; ++i) {
            if (!args) continue;
            if (fxResolveMStreamArg((Value *)args, i)) {
                wb_indices[wb_count] = i;
                wb_kinds[wb_count] = FX_WB_KIND_MSTREAM;
                wb_count++;
                continue;
            }
            if (VALUE_TYPE(args[i]) != TYPE_POINTER || !AS_POINTER(args[i])) continue;
            const Value *pointee = (const Value *)AS_POINTER(args[i]);
            if (valueIsGenericSerializable(pointee)) {
                wb_indices[wb_count] = i;
                wb_kinds[wb_count] = FX_WB_KIND_GENERIC;
                wb_count++;
            } else {
                substitutable = false;
                break;
            }
        }
    }

    Value name_val = makeString(name);
    Value count_val = makeInt(arg_count);
    pscalCacheWriteValueFramed(f, &name_val);
    pscalCacheWriteValueFramed(f, &count_val);
    freeValue(&name_val);
    freeValue(&count_val);

    Value substitutable_val = makeBoolean(substitutable ? 1 : 0);
    pscalCacheWriteValueFramed(f, &substitutable_val);
    freeValue(&substitutable_val);
    if (!substitutable) {
        fflush(f);
        return;
    }

    Value present_val = makeBoolean(has_result ? 1 : 0);
    pscalCacheWriteValueFramed(f, &present_val);
    freeValue(&present_val);
    if (has_result) {
        pscalCacheWriteValueFramed(f, result);
    }

    Value wb_count_val = makeInt(wb_count);
    pscalCacheWriteValueFramed(f, &wb_count_val);
    freeValue(&wb_count_val);

    for (int w = 0; w < wb_count; ++w) {
        int i = wb_indices[w];
        Value idx_val = makeInt(i);
        Value kind_val = makeInt(wb_kinds[w]);
        pscalCacheWriteValueFramed(f, &idx_val);
        pscalCacheWriteValueFramed(f, &kind_val);
        freeValue(&idx_val);
        freeValue(&kind_val);
        if (wb_kinds[w] == FX_WB_KIND_MSTREAM) {
            MStream *ms = fxResolveMStreamArg((Value *)args, i);
            writeRawBlob(f, ms ? ms->buffer : NULL, ms ? (uint32_t)ms->size : 0);
        } else {
            const Value *pointee = (const Value *)AS_POINTER(args[i]);
            pscalCacheWriteValueFramed(f, pointee);
        }
    }
    fflush(f);
}

PscalFxReplayOutcome pscalFxReplayCall(struct VM_s *vm, const char *name, int arg_count, Value *args,
                                       Value *out_result, char *mismatch_msg, size_t mismatch_msg_size) {
    (void)vm;
    if (!g_fx_replay_file || !name || !out_result) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size, "replay journal not open");
        }
        return PSCAL_FX_REPLAY_MISMATCH;
    }
    FILE *f = g_fx_replay_file;

    Value journaled_name = { 0 };
    Value journaled_count = { 0 };
    if (!pscalCacheReadValueFramed(f, &journaled_name) || VALUE_TYPE(journaled_name) != TYPE_STRING ||
        !pscalCacheReadValueFramed(f, &journaled_count) || VALUE_TYPE(journaled_count) != TYPE_INTEGER) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size,
                     "journal exhausted or corrupt while expecting call to '%s'", name);
        }
        freeValue(&journaled_name);
        freeValue(&journaled_count);
        return PSCAL_FX_REPLAY_MISMATCH;
    }

    bool name_ok = AS_STRING(journaled_name) && strcasecmp(AS_STRING(journaled_name), name) == 0;
    bool count_ok = (int)VAL_INT(journaled_count) == arg_count;
    if (!name_ok || !count_ok) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size,
                     "journal expected call to '%s' (%d args), program called '%s' (%d args)",
                     AS_STRING(journaled_name) ? AS_STRING(journaled_name) : "?",
                     (int)VAL_INT(journaled_count), name, arg_count);
        }
        freeValue(&journaled_name);
        freeValue(&journaled_count);
        return PSCAL_FX_REPLAY_MISMATCH;
    }
    freeValue(&journaled_name);
    freeValue(&journaled_count);

    Value substitutable_val = { 0 };
    if (!pscalCacheReadValueFramed(f, &substitutable_val) || VALUE_TYPE(substitutable_val) != TYPE_BOOLEAN) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading substitutable flag for '%s'", name);
        }
        freeValue(&substitutable_val);
        return PSCAL_FX_REPLAY_MISMATCH;
    }
    bool substitutable = VAL_INT(substitutable_val) != 0;
    freeValue(&substitutable_val);
    if (!substitutable) {
        return PSCAL_FX_REPLAY_RUN_LIVE;
    }

    Value present_val = { 0 };
    if (!pscalCacheReadValueFramed(f, &present_val) || VALUE_TYPE(present_val) != TYPE_BOOLEAN) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading result presence for '%s'", name);
        }
        freeValue(&present_val);
        return PSCAL_FX_REPLAY_MISMATCH;
    }
    bool has_result = VAL_INT(present_val) != 0;
    freeValue(&present_val);

    Value result = { 0 };
    SET_VALUE_TYPE(&result, TYPE_NIL);
    if (has_result && !pscalCacheReadValueFramed(f, &result)) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading result value for '%s'", name);
        }
        return PSCAL_FX_REPLAY_MISMATCH;
    }

    Value wb_count_val = { 0 };
    if (!pscalCacheReadValueFramed(f, &wb_count_val) || VALUE_TYPE(wb_count_val) != TYPE_INTEGER) {
        if (mismatch_msg && mismatch_msg_size) {
            snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading writeback count for '%s'", name);
        }
        freeValue(&result);
        return PSCAL_FX_REPLAY_MISMATCH;
    }
    int writeback_count = (int)VAL_INT(wb_count_val);
    freeValue(&wb_count_val);

    for (int w = 0; w < writeback_count; ++w) {
        Value idx_val = { 0 };
        Value kind_val = { 0 };
        if (!pscalCacheReadValueFramed(f, &idx_val) || VALUE_TYPE(idx_val) != TYPE_INTEGER ||
            !pscalCacheReadValueFramed(f, &kind_val) || VALUE_TYPE(kind_val) != TYPE_INTEGER) {
            if (mismatch_msg && mismatch_msg_size) {
                snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading writeback %d header for '%s'", w, name);
            }
            freeValue(&idx_val);
            freeValue(&kind_val);
            freeValue(&result);
            return PSCAL_FX_REPLAY_MISMATCH;
        }
        int idx = (int)VAL_INT(idx_val);
        int kind = (int)VAL_INT(kind_val);
        freeValue(&idx_val);
        freeValue(&kind_val);

        bool idx_in_range = idx >= 0 && idx < arg_count;
        if (kind == FX_WB_KIND_MSTREAM) {
            unsigned char *bytes = NULL;
            uint32_t len = 0;
            if (!readRawBlob(f, &bytes, &len)) {
                if (mismatch_msg && mismatch_msg_size) {
                    snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading mstream writeback %d for '%s'", w, name);
                }
                freeValue(&result);
                return PSCAL_FX_REPLAY_MISMATCH;
            }
            MStream *ms = idx_in_range ? fxResolveMStreamArg(args, idx) : NULL;
            if (!ms) {
                free(bytes);
                if (mismatch_msg && mismatch_msg_size) {
                    snprintf(mismatch_msg, mismatch_msg_size,
                             "journal mstream writeback %d targets arg %d, which is not a live MStream in '%s'", w, idx, name);
                }
                freeValue(&result);
                return PSCAL_FX_REPLAY_MISMATCH;
            }
            free(ms->buffer);
            ms->buffer = bytes;
            ms->size = (int)len;
            ms->capacity = (int)len + 1; /* readRawBlob allocates len+1 for the NUL terminator */
        } else {
            bool idx_valid = idx_in_range && VALUE_TYPE(args[idx]) == TYPE_POINTER && AS_POINTER(args[idx]);
            Value wb_val = { 0 };
            if (!pscalCacheReadValueFramed(f, &wb_val)) {
                if (mismatch_msg && mismatch_msg_size) {
                    snprintf(mismatch_msg, mismatch_msg_size, "journal truncated reading writeback %d value for '%s'", w, name);
                }
                freeValue(&result);
                return PSCAL_FX_REPLAY_MISMATCH;
            }
            if (!idx_valid) {
                if (mismatch_msg && mismatch_msg_size) {
                    snprintf(mismatch_msg, mismatch_msg_size,
                             "journal writeback %d targets arg %d, which is not a VAR parameter in '%s'", w, idx, name);
                }
                freeValue(&wb_val);
                freeValue(&result);
                return PSCAL_FX_REPLAY_MISMATCH;
            }
            Value *slot = (Value *)AS_POINTER(args[idx]);
            freeValue(slot);
            *slot = wb_val;
        }
    }

    *out_result = result;
    return PSCAL_FX_REPLAY_SUBSTITUTED;
}

bool pscalFxIsCliFlag(const char *arg) {
    return arg && (strcmp(arg, "--deny") == 0 ||
                   strcmp(arg, "--fx-record") == 0 ||
                   strcmp(arg, "--fx-replay") == 0);
}

bool pscalFxHandleCliFlag(const char *flag, const char *value) {
    if (!flag || !value) {
        fprintf(stderr, "Error: %s requires an argument.\n", flag ? flag : "(null)");
        return false;
    }
    if (strcmp(flag, "--deny") == 0) {
        EffectMask mask = FX_PURE;
        if (!pscalFxParseDenyList(value, &mask)) {
            fprintf(stderr, "Error: --deny: unrecognized token in '%s' (expected io,net,proc,clock,random,all).\n", value);
            return false;
        }
        pscalFxSetDeniedMask(mask);
        return true;
    }
    if (strcmp(flag, "--fx-record") == 0) {
        return pscalFxBeginRecord(value);
    }
    if (strcmp(flag, "--fx-replay") == 0) {
        return pscalFxBeginReplay(value);
    }
    fprintf(stderr, "Error: unrecognized fx policy flag '%s'.\n", flag);
    return false;
}
