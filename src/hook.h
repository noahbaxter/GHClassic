#pragma once

#include "ps2_runtime.h"

#include <cstdint>

namespace gh2
{
    // Runs native code on entry to a guest function, then the function itself.
    //
    // Only the entry is ours: when the guest function yields mid-body, the
    // runtime resumes it at the yielding PC inside the original, so nothing
    // may be done after the call. Each Tag gives one hook its own storage.
    //
    // callGuestFunction from an entry keeps only $ra and pc (ps2_runtime.cpp):
    // it loads its arguments into $a0-$a3 and $t0-$t3, and the callee may
    // change any register the EE's ABI leaves to the caller ($at, $v0-$v1,
    // $a0-$a3, $t0-$t9, hi/lo, $f0-$f19). So an entry that calls into the
    // guest puts back whatever arguments the original still reads.
    template <typename Tag>
    struct EntryHook
    {
        using Callback = void (*)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

        static void install(PS2Runtime &runtime, uint32_t address, Callback onEntry)
        {
            s_original = runtime.lookupFunction(address);
            s_onEntry = onEntry;
            runtime.replaceFunction(address, &run);
        }

    private:
        static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_onEntry(rdram, ctx, runtime);
            s_original(rdram, ctx, runtime);
        }

        inline static PS2Runtime::RecompiledFunction s_original = nullptr;
        inline static Callback s_onEntry = nullptr;
    };
}
