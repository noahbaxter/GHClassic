// The memory card the game sees: a formatted 8 MB card in slot 1 with room,
// and nothing on disk. The save is save.bin's (save/save.cpp), so all the
// game still asks the card is whether it is there.
//
// libmc is asynchronous: a call starts a command and sceMcSync hands back its
// result. What still calls it: MCInit (0x2aa3a8), MCGetInfo (0x2aa400) for
// the menus' checks, MCGetDir (0x2aa810) for MemcardMgr's check_save_game,
// which no script uses, and MCFormat (0x2aab08), only for an unformatted
// card. The file calls were SaveData1's and LoadData1's.

#include "save/card.h"

#include "guest.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <iostream>

namespace gh2::card
{
    namespace
    {
        constexpr int32_t kGetInfo = 0x01;
        constexpr int32_t kGetDir = 0x0d;
        constexpr int32_t kFormat = 0x10;

        constexpr int32_t kSucceed = 0;
        constexpr int32_t kNoEntry = -4;

        constexpr int32_t kTypePs2 = 2;
        constexpr int32_t kFreeClusters = 0x2000;

        int32_t s_command = 0;
        int32_t s_result = 0;
        bool s_pending = false;

        void finish(R5900Context *ctx, int32_t value)
        {
            setReturnS32(ctx, value);
            ctx->pc = GPR_U32(ctx, 31);
        }

        void start(R5900Context *ctx, int32_t command, int32_t result)
        {
            s_command = command;
            s_result = result;
            s_pending = true;
            finish(ctx, 0);
        }

        void put(uint8_t *rdram, uint32_t pointer, int32_t value)
        {
            if (pointer != 0u)
                store<int32_t>(rdram, pointer, value);
        }

        void reset(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_pending = false;
            finish(ctx, 0);
        }

        // sceMcGetInfo(port, slot, int *type, int *free, int *format)
        void getInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const bool card = GPR_U32(ctx, 4) == 0u && GPR_U32(ctx, 5) == 0u;
            put(rdram, GPR_U32(ctx, 6), card ? kTypePs2 : 0);
            put(rdram, GPR_U32(ctx, 7), card ? kFreeClusters : 0);
            put(rdram, GPR_U32(ctx, 8), card ? 1 : 0);
            start(ctx, kGetInfo, card ? kSucceed : kNoEntry);
        }

        // sceMcSync(mode, int *command, int *result): 1 with a finished
        // command's result, -1 when none was started.
        void sync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_pending)
                return finish(ctx, -1);
            s_pending = false;
            put(rdram, GPR_U32(ctx, 5), s_command);
            put(rdram, GPR_U32(ctx, 6), s_result);
            finish(ctx, 1);
        }

        void getDir(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            start(ctx, kGetDir, kNoEntry);
        }

        void format(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            start(ctx, kFormat, kSucceed);
        }

        // Open, mkdir, close, read, write and delete: nothing reaches them.
        void fileCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[card] file call at 0x" << std::hex << ctx->pc << std::dec << "; the card holds no files"
                      << std::endl;
            start(ctx, 0, kNoEntry);
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        runtime.replaceFunction(addresses.sceMcInit, reset);
        runtime.replaceFunction(addresses.sceMcEnd, reset);
        runtime.replaceFunction(addresses.sceMcGetInfo, getInfo);
        runtime.replaceFunction(addresses.sceMcSync, sync);
        runtime.replaceFunction(addresses.sceMcGetDir, getDir);
        runtime.replaceFunction(addresses.sceMcFormat, format);
        for (const uint32_t address : addresses.sceMcFileCalls)
            runtime.replaceFunction(address, fileCall);
    }
}
