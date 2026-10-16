#include "content/locale.h"

#include "formats/dtb.h"
#include "guest.h"
#include "script.h"

#include "ps2_runtime.h"

#include <array>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace gh2::locale
{
    namespace
    {
        constexpr uint32_t kString = 0x12u;

        const Addresses *s_addresses = nullptr;
        std::string s_added; // (token "text") or (token {expression}) for each
        std::string s_game = "gh2";
        std::map<std::string, uint32_t> s_tables; // each game's, once built
        // Each other game's text where it differs from GH2's, by token: a
        // char* in its kept table.
        std::map<std::string, std::unordered_map<std::string, uint32_t>> s_differs;
        std::array<std::string, kOwners> s_owners = {"gh2", "gh2", "gh2"};
        std::array<std::set<std::string>, kOwners> s_ownTokens;

        // A kept table's (token "text") entries: the token's chars and the
        // text's. A string node's value is an array whose first word is its
        // chars.
        std::unordered_map<std::string, uint32_t> entries(uint8_t *rdram, uint32_t table)
        {
            std::unordered_map<std::string, uint32_t> out;
            for (int i = 0; i < load<int16_t>(rdram, table + 8u); ++i)
            {
                const uint32_t node = load<uint32_t>(rdram, table) + 8u * static_cast<uint32_t>(i);
                if (load<uint32_t>(rdram, node + 4u) != script::kArray)
                    continue;
                const uint32_t entry = load<uint32_t>(rdram, node);
                const uint32_t nodes = load<uint32_t>(rdram, entry);
                if (load<int16_t>(rdram, entry + 8u) < 2 || load<uint32_t>(rdram, nodes + 4u) != script::kSymbol ||
                    load<uint32_t>(rdram, nodes + 12u) != kString)
                    continue;
                out.emplace(reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, nodes))),
                            load<uint32_t>(rdram, load<uint32_t>(rdram, nodes + 8u)));
            }
            return out;
        }

        // Where a game's text differs from GH2's, once both are kept.
        void differ(uint8_t *rdram, const std::string &game)
        {
            if (game == "gh2" || s_differs.count(game) != 0u || s_tables.count(game) == 0u || s_tables.count("gh2") == 0u)
                return;
            const auto theirs = entries(rdram, s_tables[game]), ours = entries(rdram, s_tables["gh2"]);
            auto &differs = s_differs[game];
            for (const auto &[token, text] : theirs)
                if (const auto it = ours.find(token);
                    it == ours.end() || std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, text)),
                                                    reinterpret_cast<const char *>(getMemPtr(rdram, it->second))) != 0)
                    differs.emplace(token, text);
        }

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

    void addExpression(const std::string &token, const std::string &expression)
    {
        s_added += "(" + token + " " + expression + ")\n";
    }

    namespace
    {
        // A kept table has a reference of its own, and the Locale one more
        // to the table it has. The table the Locale built goes once kept.
        uint32_t keep(const std::string &of, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t built = load<uint32_t>(rdram, s_addresses->theLocale);
            const uint32_t kept = script::park(rdram, runtime, built);
            script::release(rdram, ctx, runtime, built);
            s_tables[of] = kept;
            return kept;
        }

        // A table read afresh into the Locale and kept: its own then.
        uint32_t read(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            store<uint32_t>(rdram, s_addresses->theLocale, 0u);
            runtime->callGuestFunction(rdram, ctx, s_addresses->localeInit, {s_addresses->theLocale});
            extend(rdram, ctx, runtime);
            const uint32_t table = keep(game, rdram, ctx, runtime);
            differ(rdram, game);
            return table;
        }

        // The table the Locale has, kept if it is not, and let go by it.
        void putAway(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_tables.count(s_game) == 0u)
                keep(s_game, rdram, ctx, runtime);
            else
                hold(rdram, load<uint32_t>(rdram, s_addresses->theLocale), -1);
        }

        void putBack(uint32_t table, uint8_t *rdram)
        {
            store<uint32_t>(rdram, s_addresses->theLocale, table);
            hold(rdram, table, 1);
        }
    }

    void enter(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (game == s_game)
            return;
        putAway(rdram, ctx, runtime);
        s_game = game;
        const auto built = s_tables.find(game);
        putBack(built != s_tables.end() ? built->second : read(game, rdram, ctx, runtime), rdram);
    }

    void build(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (game == s_game || s_tables.count(game) != 0u)
            return;
        putAway(rdram, ctx, runtime);
        read(game, rdram, ctx, runtime);
        putBack(s_tables[s_game], rdram);
        for (const auto &[other, table] : s_tables)
            differ(rdram, other);
    }

    void own(Owner owner, const std::string &game)
    {
        s_owners[owner] = game;
    }

    void ownTokens(Owner owner, const std::set<std::string> &tokens)
    {
        s_ownTokens[owner] = tokens;
    }

    uint32_t owned(const char *token)
    {
        if (s_game != "gh2" || token == nullptr)
            return 0u;
        Owner owner = kScreen;
        for (const Owner other : {kVenue, kCharacter})
            if (s_ownTokens[other].count(token) != 0u)
                owner = other;
        if (s_owners[owner] == "gh2")
            return 0u;
        const auto differs = s_differs.find(s_owners[owner]);
        if (differs == s_differs.end())
            return 0u;
        const auto text = differs->second.find(token);
        return text != differs->second.end() ? text->second : 0u;
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
