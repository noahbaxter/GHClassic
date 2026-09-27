// SYNTH_S, GH2's IOP synth module, with nothing played. The EE's SynthEE
// batches every synth command into one NOWAIT RPC per poll; this server
// takes the batch and drops it.

#include "null_synth.h"

#include "ps2_runtime.h"
#include "ps2x/iop/iop_subsystem.h"

#include <array>
#include <memory>

namespace gh2
{
    namespace
    {
        // The SID SynthEE binds in CtlClientInit (retail 0x22db68), looping
        // until the bind finds a server.
        constexpr uint32_t kSynthSid = 0x75433178u;

        class NullSynth final : public ps2x::iop::IopService
        {
        public:
            [[nodiscard]] std::string_view name() const override
            {
                return "gh2 null synth";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // Dormant until the game loads SYNTH_S.IRX, as the real module is.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return kModuleAliases;
            }

            void reset() override
            {
            }

            // CtlClientPoll (0x22dc68) is the only caller: function 0, NOWAIT,
            // a send buffer of packed {cmd, len, data} records, no receive
            // buffer and no end callback. Nothing is read back, so handling
            // the call is the whole job.
            [[nodiscard]] ps2x::iop::RpcResult handleRpc(const ps2x::iop::RpcRequest &request) override
            {
                ps2x::iop::RpcResult result;
                result.handled = request.sid == kSynthSid;
                result.resultAddress = request.receive.address;
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{kSynthSid};
            inline static constexpr std::array<std::string_view, 1> kModuleAliases{"synth_s"};
        };
    }

    void installNullSynth(PS2Runtime &runtime)
    {
        runtime.iop().addService(std::make_unique<NullSynth>());
    }
}
