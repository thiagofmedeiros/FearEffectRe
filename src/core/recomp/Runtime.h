// What generated code is written against.
#pragma once

#include "psx/Machine.h"

// Charges the cycles of a straight-line run of instructions and lets the
// machine deliver interrupts and scheduled events when the slice is used up.
// `addr` is where the block starts: c.pc is set to it before the machine takes
// over, so every guest register is in `c` and a save state can resume there.
#define FE_BLOCK(n, addr)                                                     \
    do {                                                                      \
        if ((c.cycles -= (n) * fe::kCyclesPerInstruction) <= 0) [[unlikely]] { \
            c.pc = (addr);                                                    \
            c.m->onSlice(c);                                                  \
        }                                                                     \
    } while (0)

// Direct call to a recompiled function. The hook lets differential tests
// observe calls instead of executing them.
//
// A function normally comes back with c.pc equal to the return address the
// call site put in ra. Hand-written routines that use ra as a continuation
// come back somewhere else; the interpreter then carries on from there until
// execution reaches the call site's return address.
//
// With `slowCalls` set (a differential test's call hook, or functions that
// must run in the interpreter because a cheat patched their code), calls go
// through the machine's dispatcher instead.
#define FE_CALL(fn, addr, ret)                                                  \
    do {                                                                        \
        if (c.m->slowCalls) [[unlikely]] {                                      \
            c.m->call(c, addr);                                                 \
            if (c.pc != (ret) && !c.m->hooks.call) c.m->resume(c, ret);         \
        } else {                                                                \
            fn(c);                                                              \
            if (c.pc != (ret)) [[unlikely]] c.m->resume(c, ret);                \
        }                                                                       \
    } while (0)

#define FE_TAIL(fn, addr)                                            \
    do {                                                             \
        if (c.m->slowCalls) [[unlikely]] c.m->call(c, addr);         \
        else fn(c);                                                  \
    } while (0)

#define FE_RETURNED(ret)                                                     \
    do {                                                                     \
        if (c.pc != (ret) && !c.m->hooks.call) [[unlikely]] c.m->resume(c, ret); \
    } while (0)
