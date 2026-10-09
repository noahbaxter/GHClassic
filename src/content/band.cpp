#include "content/band.h"

#include "content/campaigns.h"
#include "guest.h"
#include "hook.h"
#include "script.h"
#include "settings/settings.h"

#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

namespace gh2::band
{
    namespace
    {
        std::map<std::pair<std::string, std::string>, std::string> s_own; // by game and member
        std::unordered_map<std::string, uint32_t> s_symbols;

        std::string game()
        {
            switch (settings::get(settings::kBand))
            {
            case settings::kBandGh1:
                return "gh1";
            case settings::kBandGh2:
                return "gh2";
            default:
                return campaigns::active();
            }
        }

        // AddLoadChar(list<Character *> &, const char *object, const char
        // *character) (0x128778) loads a guitarist's outfit too, into
        // guitarist<n>.
        struct LoadTag;
        void onAddLoadChar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const char *object = reinterpret_cast<const char *>(getMemPtr(rdram, GPR_U32(ctx, 5)));
            const char *character = reinterpret_cast<const char *>(getMemPtr(rdram, GPR_U32(ctx, 6)));
            if (s_own.empty() || std::strncmp(object, "guitarist", 9u) == 0)
                return;
            const auto own = s_own.find({game(), character});
            if (own == s_own.end())
                return;
            auto symbol = s_symbols.find(own->second);
            if (symbol == s_symbols.end())
            {
                const R5900Context saved = *ctx;
                const uint32_t made = script::symbol(rdram, ctx, runtime, own->second);
                *ctx = saved;
                symbol = s_symbols.emplace(own->second, made).first;
            }
            SET_GPR_U32(ctx, 6, symbol->second);
        }

        // settings::Band's words.
        constexpr const char *kFrom[] = {"venue", "gh2", "gh1"};

        script::Node bandCommand(const script::Call &call)
        {
            if (call.symbol(1) == "from" && call.size() > 2)
                for (int i = 0; i < 3; ++i)
                    if (call.symbol(2) == kFrom[i])
                        settings::set(settings::kBand, i);
            return {script::symbol(call.rdram, call.ctx, call.runtime, kFrom[settings::get(settings::kBand)]),
                    script::kSymbol};
        }
    }

    void add(const std::string &game, const std::string &member, const std::string &own)
    {
        s_own[{game, member}] = own;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::addCommand("band", bandCommand);
        EntryHook<LoadTag>::install(runtime, addresses.addLoadChar, onAddLoadChar);
    }
}
