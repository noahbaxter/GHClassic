#include "save/profiles.h"

#include "host/input.h"
#include "save/gh2_stream.h"
#include "save/save.h"

#include <cstdlib>
#include <map>

namespace gh2::save
{
    namespace
    {
        constexpr const char *kSection = "profiles";
        constexpr const char *kGuest = "guest";

        // Each signed-in device's profile, as profileText has it, or kGuest:
        // this run's alone.
        std::map<std::string, std::string> s_signedIn;

        std::string signedInAs(const std::string &device)
        {
            const auto it = s_signedIn.find(device);
            return it != s_signedIn.end() ? it->second : std::string();
        }

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
            const std::string kept = signedInAs(device);
            const int profile = std::atoi(kept.c_str()) - 1;
            if (!profiles.here(profile) || kept != profileText(profiles, profile))
                return std::nullopt;
            return profile;
        }

        // Each controller that played as `was` plays as `now`.
        void remap(const std::string &was, const std::string &now)
        {
            for (auto &[device, kept] : s_signedIn)
                if (kept == was)
                    kept = now;
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
        const Profiles was = profiles();
        keepSection(kSection, [&](Section &section) { section.set(std::to_string(profile + 1), name); });
        if (was.here(profile))
            remap(profileText(was, profile), profileText(profiles(), profile));
    }

    void deleteProfile(int profile)
    {
        const Profiles was = profiles();
        if (!was.here(profile))
            return;
        remap(profileText(was, profile), kGuest);
        keepSection(kSection,
                    [&](Section &section)
                    {
                        std::erase_if(section.values,
                                      [&](const auto &value) { return value.first == std::to_string(profile + 1); });
                    });
        dropSections("profile " + std::to_string(profile + 1) + " ");
    }

    std::optional<int> profilePref(int profile, const std::string &key)
    {
        // The profiles first: their first read can add sections, moving this.
        if (!profiles().here(profile))
            return std::nullopt;
        const Section *section = keptSection("profile " + std::to_string(profile + 1) + " prefs");
        const std::string *value = section ? section->get(key) : nullptr;
        if (!value)
            return std::nullopt;
        return std::atoi(value->c_str());
    }

    void keepProfilePref(int profile, const std::string &key, int value)
    {
        if (profiles().here(profile))
            keepSection("profile " + std::to_string(profile + 1) + " prefs",
                        [&](Section &section) { section.set(key, std::to_string(value)); });
    }

    namespace
    {
        // A seat's player moved to another device in it (host/input.h's
        // takeHandovers) is still who they were: the device takes their
        // sign-in, profile or guest, and the one left lets it go. One not
        // signed in moves none. Only picking one changes who a seat is.
        void followHandovers()
        {
            for (const Handover &h : takeHandovers())
            {
                const auto from = s_signedIn.find(h.from);
                if (from == s_signedIn.end())
                    continue;
                s_signedIn[h.to] = from->second;
                s_signedIn.erase(h.from);
            }
        }
    }

    std::optional<int> seatProfile(int seat)
    {
        if (seat < 0 || seat >= kSeats)
            return std::nullopt;
        followHandovers();
        const Profiles now = profiles();
        const auto profile = deviceProfile(now, seatDevice(seat));
        for (int before = 0; before < seat; ++before)
            if (profile && deviceProfile(now, seatDevice(before)) == profile)
                return std::nullopt;
        return profile;
    }

    void keepSeatProfile(int seat, std::optional<int> profile)
    {
        followHandovers();
        const Profiles now = profiles();
        const std::string device = seat >= 0 && seat < kSeats ? seatDevice(seat) : std::string();
        if (device.empty() || (profile && !now.here(profile)))
            return;
        const std::string text = profile ? profileText(now, *profile) : kGuest;
        if (profile)
            remap(text, kGuest);
        s_signedIn[device] = text;
    }

    bool seatSignedIn(int seat)
    {
        followHandovers();
        return !signedInAs(seatDevice(seat)).empty();
    }

    void signOut(int seat)
    {
        followHandovers();
        s_signedIn.erase(seatDevice(seat));
    }
}
