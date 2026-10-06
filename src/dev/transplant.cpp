#include "dev/transplant.h"

#include "guest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/mesh_capture.h"
#include "render/texture_capture.h"
#include "settings/settings.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace gh2::transplant
{
    namespace
    {
        constexpr uint32_t kRamSize = 0x2000000u;
        // Below the executable is the kernel's, which is not this run's kind.
        constexpr uint32_t kFirst = 0x100000u;

        bool s_active = false;

        // A poll of the main loop: nothing, once the memory is another run's.
        template <typename Tag>
        struct Skipped
        {
            static void install(PS2Runtime &runtime, uint32_t address)
            {
                s_original = runtime.lookupFunction(address);
                runtime.replaceFunction(address, &run);
            }

            static void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                if (s_active)
                    ctx->pc = GPR_U32(ctx, 31);
                else
                    s_original(rdram, ctx, runtime);
            }

            inline static PS2Runtime::RecompiledFunction s_original = nullptr;
        };
        struct SystemPollTag;
        struct SynthPollTag;
        struct SynthEEPollTag;
    }

    bool load(uint8_t *rdram, const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> image(kRamSize);
        if (!in.read(image.data(), kRamSize))
        {
            std::cerr << "[transplant] " << path << " is not a " << kRamSize << " byte RAM image" << std::endl;
            return false;
        }
        readMeshesFromPackets();
        forgetTextureAddresses();
        std::memcpy(getMemPtr(rdram, kFirst), image.data() + kFirst, kRamSize - kFirst);
        s_active = true;
        // The frame is set beside retail's, which culls.
        settings::set(settings::kFrustumCull, 1);
        std::cerr << "[transplant] memory is now " << path << std::endl;
        return true;
    }

    bool active()
    {
        return s_active;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        Skipped<SystemPollTag>::install(runtime, addresses.systemPoll);
        Skipped<SynthPollTag>::install(runtime, addresses.synthPoll);
        Skipped<SynthEEPollTag>::install(runtime, addresses.synthEEPoll);
    }
}
