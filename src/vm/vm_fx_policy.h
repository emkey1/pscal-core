#ifndef PSCAL_VM_FX_POLICY_H
#define PSCAL_VM_FX_POLICY_H

/* VM 2.0 Phase 6 (Docs/pscal_vm2_plan.md §6.3): the --deny sandbox and the
 * record/replay journal. Both are opt-in, process-wide, and checked at the
 * single CALL_BUILTIN/CALL_BUILTIN_PROC dispatch point in vm.c; when neither
 * is active the checks are one cheap comparison against a zero mask / null
 * FILE*, so this is a no-op for every program that doesn't ask for it. */

#include "core/effect_mask.h"
#include "core/types.h"
#include <stdbool.h>
#include <stddef.h>

struct VM_s;

/* Parses a comma-separated deny list ("net,proc", "io", "all") into a mask.
 * Unknown tokens are rejected (returns false) so a typo in --deny fails
 * loudly at startup instead of silently under-enforcing. */
bool pscalFxParseDenyList(const char *csv, EffectMask *out_mask);

/* CLI-set deny mask (e.g. --deny). Combined (OR'd) with the PSCAL_VM_DENY
 * environment variable, read lazily once on first use, following the
 * PSCAL_VM_SKIP_VERIFY convention (Phase 1e). */
void pscalFxSetDeniedMask(EffectMask mask);
EffectMask pscalFxEffectiveDeniedMask(void);

/* True if a deny-list, record, or replay policy is active. The one check the
 * hot CALL_BUILTIN/CALL_BUILTIN_PROC dispatch path pays when nothing is
 * configured: no mutex, no per-builtin lookup. */
bool pscalFxPolicyActive(void);

/* Record/replay journal. Returns false (with a stderr diagnostic) if the
 * journal file can't be opened -- callers should treat that as a fatal
 * startup error, not silently run unrecorded/unreplayed. Only one of
 * record/replay may be active at a time. */
bool pscalFxBeginRecord(const char *path);
bool pscalFxBeginReplay(const char *path);
bool pscalFxRecordActive(void);
bool pscalFxReplayActive(void);
void pscalFxEndSession(void);

/* Called from vm.c right after a live handler call for an effectful builtin
 * (mask != FX_PURE) when recording is active. Journals the call in order:
 * name, arg count, the return Value, and -- for every TYPE_POINTER argument
 * (a VAR parameter) -- the post-call pointee Value (the writeback).
 *
 * Not every Value is meaningfully replayable: a TYPE_FILE writeback (from
 * assign/reset/rewrite/close) is a live OS handle, not data -- substituting a
 * placeholder for it corrupts the variable and breaks every later operation
 * on it (found by adversarial testing against this exact call shape). A
 * TYPE_MEMORYSTREAM writeback (HttpRequest's out param, etc.) is genuine
 * payload data and gets its buffer captured directly rather than routed
 * through the generic Value codec (which has no MStream case). Any other
 * unserializable result/writeback marks the whole call "not substitutable";
 * pscalFxReplayCall then tells the caller to re-run the real handler for
 * that call instead of guessing -- correct output over cleverness. */
void pscalFxRecordCall(const char *name, int arg_count, const Value *args, const Value *result);

typedef enum {
    PSCAL_FX_REPLAY_MISMATCH = 0,  /* journal/call desync: caller must abort */
    PSCAL_FX_REPLAY_SUBSTITUTED,   /* *out_result and writebacks filled from the journal */
    PSCAL_FX_REPLAY_RUN_LIVE       /* name/arg_count matched but this call wasn't
                                     * substitutable when recorded -- caller must
                                     * invoke the real handler instead */
} PscalFxReplayOutcome;

/* Called from vm.c instead of invoking the handler when replay is active for
 * an effectful builtin. See PscalFxReplayOutcome for the three outcomes; on
 * PSCAL_FX_REPLAY_MISMATCH, mismatch_msg is filled with a diagnostic -- the
 * caller must abort the program cleanly, never guess. */
PscalFxReplayOutcome pscalFxReplayCall(struct VM_s *vm, const char *name, int arg_count, Value *args,
                                       Value *out_result, char *mismatch_msg, size_t mismatch_msg_size);

/* Shared CLI wiring so the 5 frontend main()s (each with its own hand-rolled
 * argv loop, no shared parser) don't each reimplement --deny/--fx-record/
 * --fx-replay. Caller recognizes one of these three flag spellings (they all
 * take a mandatory value) and hands the pair off here; this prints its own
 * diagnostic and returns false on a bad deny token or an unopenable journal
 * file, so the caller can just exit(EXIT_FAILURE) without its own message. */
bool pscalFxIsCliFlag(const char *arg);
bool pscalFxHandleCliFlag(const char *flag, const char *value);

#endif /* PSCAL_VM_FX_POLICY_H */
