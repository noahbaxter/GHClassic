#include "content/locale.h"

#include "formats/dtb.h"
#include "guest.h"
#include "script.h"

namespace gh2::locale
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        std::string s_text;

        // {locale_table}: the array Locale::Localize looks tokens up in
        // (TheLocale's first word, 0x2cbaf8), with the reference a returned
        // array carries.
        script::Node localeTable(const script::Call &call)
        {
            const uint32_t table = load<uint32_t>(call.rdram, s_addresses->theLocale);
            if (table == 0u)
                return {};
            store<int16_t>(call.rdram, table + 0xau, static_cast<int16_t>(load<int16_t>(call.rdram, table + 0xau) + 1));
            return {table, script::kArray};
        }
    }

    void add(const std::string &token, const std::string &text)
    {
        s_text += "{push_back {locale_table} (" + token + " " + dtb::text({dtb::kString, 0, 0.0f, text}) + ")}\n";
    }

    void install(PS2Runtime &, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("locale_table", localeTable);
        script::runWhenUiReady(s_text);
    }
}
