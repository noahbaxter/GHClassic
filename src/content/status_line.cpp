#include "content/status_line.h"

#include "guest.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace gh2::status_line
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_getStatusProgress = nullptr;

        void getStatusProgress(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const auto call = [&](uint32_t function, std::initializer_list<uint32_t> with)
            {
                return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, with));
            };
            const auto text = [&](uint32_t symbol)
            {
                return std::string(reinterpret_cast<const char *>(
                    getMemPtr(rdram, call(s_addresses->localeLocalize, {s_addresses->theLocale, symbol, 1u}))));
            };
            const std::string format = text(script::symbol(rdram, ctx, runtime, "status_progress"));
            const uint32_t state = call(s_addresses->campaignGetCampaign, {args.a[0], args.a[1]});
            const auto beat = static_cast<int32_t>(call(s_addresses->campaignStateGetSongsBeat, {state}));
            // Only the one shape: the status, the songs beaten, the songs.
            std::string specs;
            for (size_t at = format.find('%'); at != std::string::npos && at + 1u < format.size(); at = format.find('%', at + 2u))
                specs += format[at + 1u];
            if (specs != "sii" || beat == 0)
            {
                args.restore(ctx);
                SET_GPR_U32(ctx, 31, returnTo);
                return s_getStatusProgress(rdram, ctx, runtime);
            }
            const auto songs = static_cast<int32_t>(call(s_addresses->campaignStateGetNumSongs, {state}));
            const std::string status = text(call(s_addresses->campaignStateGetStatusSym, {state}));
            char line[160];
            std::snprintf(line, sizeof line, format.c_str(), status.c_str(), beat, songs);
            SET_GPR_U32(ctx, 2, script::symbol(rdram, ctx, runtime, line));
            ctx->pc = returnTo;
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_getStatusProgress = runtime.lookupFunction(addresses.campaignGetStatusProgress);
        runtime.replaceFunction(addresses.campaignGetStatusProgress, &getStatusProgress);
    }
}
