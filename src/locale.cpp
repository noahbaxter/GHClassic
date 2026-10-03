// Locale::Localize (retail 0x2cbaf8) turns a token into the disc's text for
// it. Tokens in dta/locale.dta get ours instead: the file is the disc's
// locale.dta format, `(token "text")`, read by the game's own parser, so the
// strings are the game's and live as long as the parsed array.

#include "locale.h"

#include "dta.h"
#include "guest.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <iostream>
#include <string>
#include <unordered_map>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kString = 0x12u;

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_original = nullptr;
        bool s_parsed = false;
        bool s_parsing = false;
        std::unordered_map<std::string, uint32_t> s_text; // token -> guest char*
        std::unordered_map<uint32_t, uint32_t> s_bySymbol; // Symbols are interned; 0 is the disc's

        // A DataArray: nodes at +0, their count at +8; a node is a value and
        // a type. A string node's value is an array whose nodes are the chars.
        void read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t root = script::parse(rdram, ctx, runtime, kLocaleDta);
            if (root == 0u)
                return;
            const uint32_t entries = load<uint32_t>(rdram, root);
            for (int i = 0; i < load<int16_t>(rdram, root + 8u); ++i)
            {
                const uint32_t entry = load<uint32_t>(rdram, entries + 8u * static_cast<uint32_t>(i));
                const uint32_t nodes = load<uint32_t>(rdram, entry);
                if (load<uint32_t>(rdram, entries + 8u * static_cast<uint32_t>(i) + 4u) != script::kArray ||
                    load<int16_t>(rdram, entry + 8u) != 2 || load<uint32_t>(rdram, nodes + 4u) != script::kSymbol ||
                    load<uint32_t>(rdram, nodes + 12u) != kString)
                {
                    std::cerr << "[locale] entry " << i << " is not (token \"text\")" << std::endl;
                    continue;
                }
                const char *token = reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, nodes)));
                s_text[token] = load<uint32_t>(rdram, load<uint32_t>(rdram, nodes + 8u));
            }
        }

        // Locale::Localize(Symbol token, bool fail)
        void localize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // A resume lands mid-body, and the parser may localize.
            if (ctx->pc != s_addresses->localeLocalize || s_parsing)
                return s_original(rdram, ctx, runtime);
            if (!s_parsed)
            {
                const R5900Context saved = *ctx;
                s_parsing = true;
                read(rdram, ctx, runtime);
                s_parsing = false;
                s_parsed = true;
                *ctx = saved;
            }
            const uint32_t symbol = GPR_U32(ctx, 5);
            auto found = s_bySymbol.find(symbol);
            if (found == s_bySymbol.end())
            {
                const auto text =
                    symbol ? s_text.find(reinterpret_cast<const char *>(getMemPtr(rdram, symbol))) : s_text.end();
                found = s_bySymbol.emplace(symbol, text == s_text.end() ? 0u : text->second).first;
            }
            if (found->second == 0u)
                return s_original(rdram, ctx, runtime);
            SET_GPR_U32(ctx, 2, found->second);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installLocale(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_original = runtime.lookupFunction(addresses.localeLocalize);
        runtime.replaceFunction(addresses.localeLocalize, localize);
    }
}
