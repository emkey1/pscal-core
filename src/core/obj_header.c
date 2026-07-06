// src/core/obj_header.c
//
// See core/obj_header.h for the design rationale (VM 2.0 Phase 4 Stage A,
// sub-phase 4a, Docs/pscal_vm2_plan.md §5.10.3-5.10.4).
#include "core/obj_header.h"

#include <stdio.h>
#include <stdlib.h>

// VarType is a small, dense enum (core/types.h) with TYPE_UNICODE_STRING
// as its last member, so a flat array indexed by the enum value is
// simpler and faster than a hash table and keeps registration
// allocation-free. Sized off the enum's own last value (not a hardcoded
// number) so appending a new VarType after TYPE_UNICODE_STRING in the
// future grows this table automatically.
#define PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE ((size_t)TYPE_UNICODE_STRING + 1)

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
                "(heap pointer %p exceeds the 46-bit payload budget). Refusing to "
                "start rather than silently corrupt references.\n",
                probe);
        free(probe);
        abort();
    }
    free(probe);
}
