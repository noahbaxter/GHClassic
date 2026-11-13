#include "save/profiles.h"

#include "host/input.h"
#include "save/gh2_stream.h"
#include "save/save.h"

#include <cstdlib>

namespace gh2::save
{
    namespace
    {
        constexpr const char *kSection = "profiles";
        constexpr const char *kGuest = "guest";

        // A save from before profiles: one for each band, as many as there
        // is room for, the first taking every best kept then. Once: a band
        // made later is no profile.
        void fromBands()
        {
            keepSection(kSection, [](Section &section) { section.set("version", "1"); });
            int made = 0;
            for (int slot = 0; slot < gh2::kProfiles && made < kProfiles; ++slot)
                if (const Section *band = keptSection("band " + std::to_string(slot + 1)))
                    if (const std::string *name = band->get("name"); name && !name->empty())
                        nameProfile(made++, *name);
            // Copied first: keeping a section can move the one read.
            if (const Section *bests = keptSection(bestSection(std::nullopt)); bests && made > 0)
                keepSection(bestSection(0), [values = bests->values](Section &section) { section.values = values; });
        }

        // As state.ini keeps a profile: its number from 1 and its name.
        std::string profileText(const Profiles &profiles, int profile)
        {
            return std::to_string(profile + 1) + ' ' + profiles.names[static_cast<size_t>(profile)];
        }

        // What `device` last played as: a profile or a guest (none).
        std::optional<int> deviceProfile(const Profiles &profiles, const std::string &device)
        {
            const std::string kept = device.empty() ? std::string() : state(kSection, device);
            const int profile = std::atoi(kept.c_str()) - 1;
            if (!profiles.here(profile) || kept != profileText(profiles, profile))
                return std::nullopt;
            return profile;
        }
    }

    std::optional<int> Profiles::free() const
    {
        for (int profile = 0; profile < kProfiles; ++profile)
            if (!here(profile))
                return profile;
        return std::nullopt;
    }

    Profiles profiles()
    {
        if (!keptSection(kSection))
            fromBands();
        Profiles profiles;
        if (const Section *section = keptSection(kSection))
            for (int profile = 0; profile < kProfiles; ++profile)
                if (const std::string *name = section->get(std::to_string(profile + 1)))
                    profiles.names[static_cast<size_t>(profile)] = *name;
        return profiles;
    }

    void nameProfile(int profile, const std::string &name)
    {
        if (profile < 0 || profile >= kProfiles)
            return;
        keepSection(kSection, [&](Section &section) { section.set(std::to_string(profile + 1), name); });
    }

    std::optional<int> seatProfile(int seat)
    {
        if (seat < 0 || seat >= kSeats)
            return std::nullopt;
        const Profiles now = profiles();
        const auto profile = deviceProfile(now, seatDevice(seat));
        for (int before = 0; before < seat; ++before)
            if (profile && deviceProfile(now, seatDevice(before)) == profile)
                return std::nullopt;
        return profile;
    }

    void keepSeatProfile(int seat, std::optional<int> profile)
    {
        const Profiles now = profiles();
        const std::string device = seat >= 0 && seat < kSeats ? seatDevice(seat) : std::string();
        if (device.empty() || (profile && !now.here(profile)))
            return;
        const std::string text = profile ? profileText(now, *profile) : kGuest;
        if (profile)
            for (const auto &[other, kept] : stateSection(kSection))
                if (other != device && kept == text)
                    keepState(kSection, other, "");
        keepState(kSection, device, text);
    }
}
