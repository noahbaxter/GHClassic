#include "content/locale.h"

#include "formats/dtb.h"
#include "guest.h"
#include "script.h"

#include "ps2_runtime.h"

#include <map>
#include <set>
#include <vector>

namespace gh2::locale
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        std::string s_added; // (token "text") for each
        std::string s_game = "gh2";
        std::map<std::string, uint32_t> s_tables; // each game's, once built

        void hold(uint8_t *rdram, uint32_t array, int by)
        {
            store<int16_t>(rdram, array + 0xau, static_cast<int16_t>(load<int16_t>(rdram, array + 0xau) + by));
        }

        // The added tokens the table lacks go on its end, and it is sorted
        // again: Locale::Localize (0x2cbaf8) searches a sorted array by
        // halves and any other from its start (DataArray::FindArray,
        // 0x2aff10), and DataArray::Resize (0x2afbd8) leaves one unsorted.
        void extend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t table = load<uint32_t>(rdram, s_addresses->theLocale);
            const uint32_t added = table ? script::parse(rdram, ctx, runtime, s_added) : 0u;
            if (added == 0u)
                return;
            const auto token = [&](uint32_t node) -> uint32_t
            {
                if (load<uint32_t>(rdram, node + 4u) != script::kArray)
                    return 0u;
                const uint32_t entry = load<uint32_t>(rdram, node);
                return load<int16_t>(rdram, entry + 8u) > 0 ? load<uint32_t>(rdram, load<uint32_t>(rdram, entry)) : 0u;
            };
            std::set<uint32_t> has;
            const int count = load<int16_t>(rdram, table + 8u);
            for (int i = 0; i < count; ++i)
                has.insert(token(load<uint32_t>(rdram, table) + 8u * static_cast<uint32_t>(i)));
            std::vector<uint32_t> lacked;
            for (int i = 0; i < load<int16_t>(rdram, added + 8u); ++i)
            {
                const uint32_t node = load<uint32_t>(rdram, added) + 8u * static_cast<uint32_t>(i);
                if (has.insert(token(node)).second)
                    lacked.push_back(node);
            }
            runtime->callGuestFunction(rdram, ctx, s_addresses->dataArrayResize,
                                       {table, static_cast<uint32_t>(count) + static_cast<uint32_t>(lacked.size())});
            for (size_t i = 0; i < lacked.size(); ++i)
                runtime->callGuestFunction(
                    rdram, ctx, s_addresses->dataNodeAssign,
                    {load<uint32_t>(rdram, table) + 8u * (static_cast<uint32_t>(count) + static_cast<uint32_t>(i)), lacked[i]});
            script::release(rdram, ctx, runtime, added);
            runtime->callGuestFunction(rdram, ctx, s_addresses->dataArraySort, {table});
        }
    }

    void add(const std::string &token, const std::string &text)
    {
        s_added += "(" + token + " " + dtb::text({dtb::kString, 0, 0.0f, text}) + ")\n";
    }

    void enter(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (game == s_game)
            return;
        // A kept table has a reference of its own, and the Locale one more
        // to the table it has. The table the Locale built goes once kept.
        const uint32_t locale = s_addresses->theLocale;
        const auto keep = [&](const std::string &of)
        {
            const uint32_t built = load<uint32_t>(rdram, locale);
            const uint32_t kept = script::park(rdram, runtime, built);
            script::release(rdram, ctx, runtime, built);
            s_tables[of] = kept;
            return kept;
        };
        if (s_tables.count(s_game) == 0u)
            keep(s_game);
        else
            hold(rdram, load<uint32_t>(rdram, locale), -1);
        s_game = game;
        uint32_t table = 0u;
        if (const auto built = s_tables.find(game); built != s_tables.end())
            table = built->second;
        else
        {
            store<uint32_t>(rdram, locale, 0u);
            runtime->callGuestFunction(rdram, ctx, s_addresses->localeInit, {locale});
            extend(rdram, ctx, runtime);
            table = keep(game);
        }
        store<uint32_t>(rdram, locale, table);
        hold(rdram, table, 1);
    }

    void install(PS2Runtime &, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("ghc_locale_extend", [](const script::Call &call) -> script::Node {
            extend(call.rdram, call.ctx, call.runtime);
            return {};
        });
        script::runWhenUiReady("{ghc_locale_extend}");
    }
}
