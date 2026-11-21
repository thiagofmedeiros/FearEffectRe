// What generated code is written against.
#pragma once

#include "psx/Machine.h"

// Charges the cycles of a straight-line run of instructions and lets the
// machine deliver interrupts and scheduled events when the slice is used up.
#define FE_BLOCK(n)                                   \
    do {                                              \
        if ((c.cycles -= (n) * fe::kCyclesPerInstruction) <= 0) c.m->onSlice(c);  \
    } while (0)

// Direct call to a recompiled function. The hook lets differential tests
// observe calls instead of executing them.
//
// A function normally comes back with c.pc equal to the return address the
// call site put in ra. Hand-written routines that use ra as a continuation
// come back somewhere else; the interpreter then carries on from there until
// execution reaches the call site's return address.
#define FE_CALL(fn, addr, ret)                                       \
    do {                                                             \
        if (c.m->hooks.call) [[unlikely]] c.m->hooks.call(c, addr);  \
        else {                                                       \
            fn(c);                                                   \
            if (c.pc != (ret)) [[unlikely]] c.m->resume(c, ret);     \
        }                                                            \
    } while (0)

#define FE_TAIL(fn, addr)                                            \
    do {                                                             \
        if (c.m->hooks.call) [[unlikely]] c.m->hooks.call(c, addr);  \
        else fn(c);                                                  \
    } while (0)

#define FE_RETURNED(ret)                                                     \
    do {                                                                     \
        if (c.pc != (ret) && !c.m->hooks.call) [[unlikely]] c.m->resume(c, ret); \
    } while (0)
