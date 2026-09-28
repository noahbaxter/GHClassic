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
