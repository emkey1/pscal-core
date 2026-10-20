// src/core/obj_header.c
//
// See core/obj_header.h for the design rationale (VM 2.0 Phase 4 Stage A,
// sub-phase 4a, Docs/pscal_vm2_plan.md §5.10.3-5.10.4).
#include "core/obj_header.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// VarType is a small, dense enum (core/var_type.h) with TYPE_CHANNEL as its
// last member (VM 2.0 Phase 5b checkpoint 5b-i appended it after TYPE_TASK,
// the previous last member), so a flat array indexed by the enum value is
// simpler and faster than a hash table and keeps registration
// allocation-free. Sized off the enum's own last value (not a hardcoded
// number) -- update this macro's referenced type, not just its value,
// whenever a future VarType is appended (TYPE_TASK's own addition was
// exactly this bug: the table was sized off TYPE_UNICODE_STRING, one short
// of TYPE_TASK's actual value, which made
// pscalObjRegisterDestructor(TYPE_TASK, ...) abort as "out of range" until
// caught by an actual TaskSpawn smoke test -- re-verify with an actual
// ChannelCreate smoke test after this change too, not just visual
// inspection).
#define PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE ((size_t)TYPE_MAP + 1)

static PscalObjDestroyFn g_obj_destructors[PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE];
static bool g_obj_destructor_registered[PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE];
static atomic_bool g_obj_canary_claimed = false;

void pscalObjRegisterDestructor(VarType type, PscalObjDestroyFn destroy) {
    if ((size_t)type >= PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE) {
        fprintf(stderr,
                "PSCAL VM 2.0: pscalObjRegisterDestructor: VarType %d is out of range "
                "(destructor table holds %zu entries).\n",
                (int)type, PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE);
        abort();
    }
    if (g_obj_destructor_registered[type]) {
        fprintf(stderr,
                "PSCAL VM 2.0: pscalObjRegisterDestructor: VarType %d already has a "
                "destructor registered -- double registration is a bug, not an update.\n",
                (int)type);
        abort();
    }
    g_obj_destructors[type] = destroy;
    g_obj_destructor_registered[type] = true;
}

void pscalObjHeaderInit(ObjHeader *header, VarType type) {
    header->type = type;
    atomic_init(&header->refcount, 1u);
}

ObjHeader *pscalObjRetain(ObjHeader *header) {
    if (!header) {
        return NULL;
    }
    // Relaxed is sufficient: a new reference is always handed out from a
    // location that already holds a valid reference, so no happens-before
    // relationship needs to be established by the increment itself.
    atomic_fetch_add_explicit(&header->refcount, 1u, memory_order_relaxed);
    return header;
}

void pscalObjRelease(ObjHeader *header) {
    if (!header) {
        return;
    }
    // Release on every decrement (so all of this thread's prior writes to
    // the object are visible to whichever thread's decrement ultimately
    // reaches zero), and an acquire fence only on the decrement that
    // actually hits zero (so this thread sees every other thread's writes
    // before running the destructor). This is the standard atomic-refcount
    // pattern -- see the C++ shared_ptr reference semantics this mirrors.
    uint32_t prev = atomic_fetch_sub_explicit(&header->refcount, 1u, memory_order_release);
    if (prev == 0u) {
        // Double-release: the refcount was already zero before this call,
        // meaning the object was almost certainly already destroyed by an
        // earlier (correct) release. The fetch_sub above just underflowed
        // the counter to UINT32_MAX -- restore it rather than leave it
        // corrupted, log loudly (this is always a real caller bug, never
        // a legitimate case), and refuse to touch the object further:
        // running the destructor here would be a use-after-free on top of
        // whatever already freed it the first time.
        atomic_fetch_add_explicit(&header->refcount, 1u, memory_order_relaxed);
        fprintf(stderr,
                "PSCAL VM 2.0: pscalObjRelease: double-release detected (refcount was "
                "already zero) -- ignoring to avoid a use-after-free; this indicates a "
                "real bug in the caller, not a case this function can fully recover "
                "from.\n");
        return;
    }
    if (prev != 1u) {
        return; // still referenced elsewhere
    }
    atomic_thread_fence(memory_order_acquire);

    VarType type = header->type;
    if ((size_t)type >= PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE || !g_obj_destructor_registered[type]) {
        fprintf(stderr,
                "PSCAL VM 2.0: pscalObjRelease: no destructor registered for VarType %d.\n",
                (int)type);
        abort();
    }
    g_obj_destructors[type](header);
}

// The destructor is trivial -- the box owns nothing beyond its own memory.
// Registered once, on the first box, through pthread_once: an atomic
// claim-once flag let a second thread create and release a box after the
// first had claimed the flag but before it had registered the destructor,
// and pscalObjRelease aborts on an unregistered type.
static void longDoubleBoxDestroy(ObjHeader *header) {
    free((LongDoubleBox *)header);
}

static pthread_once_t g_long_double_box_destructor_once = PTHREAD_ONCE_INIT;

static void registerLongDoubleBoxDestructor(void) {
    pscalObjRegisterDestructor(TYPE_LONG_DOUBLE, longDoubleBoxDestroy);
}

LongDoubleBox *pscalLongDoubleBoxCreate(long double value) {
    pthread_once(&g_long_double_box_destructor_once, registerLongDoubleBoxDestructor);
    LongDoubleBox *box = malloc(sizeof(LongDoubleBox));
    if (!box) {
        fprintf(stderr, "PSCAL VM 2.0: pscalLongDoubleBoxCreate: allocation failed.\n");
        abort();
    }
    pscalObjHeaderInit(&box->header, TYPE_LONG_DOUBLE);
    box->value = value;
    return box;
}

void pscalObjRunPointerWidthCanary(void) {
    // atomic_exchange claims the right to run the check; any thread that
    // loses the race returns immediately rather than re-running it. This
    // does not block a losing thread until the winner finishes, but that's
    // fine here: a failed canary aborts the whole process (every thread
    // goes down together), so there is no scenario where a loser
    // proceeds past a check the winner was about to fail.
    if (atomic_exchange_explicit(&g_obj_canary_claimed, true, memory_order_acq_rel)) {
        return; // already claimed by this thread or another
    }

    void *probe = malloc(1);
    if (!probe) {
        // Allocation failure here is a pre-existing, unrelated OOM
        // condition -- not this canary's concern to handle specially.
        return;
    }
    bool fits = pscalObjPointerFitsPayload(probe);
    if (!fits) {
        fprintf(stderr,
                "PSCAL VM 2.0: tagged-pointer address-space assumption violated "
                "(heap pointer %p exceeds the 50-bit payload budget). Refusing to "
                "start rather than silently corrupt references.\n",
                probe);
        free(probe);
        abort();
    }
    free(probe);
}
