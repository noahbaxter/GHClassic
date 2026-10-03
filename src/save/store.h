#pragma once

// The player's progress: [section] headers over key = value lines, as in
// settings.ini, deflated into save.bin. Each game owns its sections whole and
// rewrites them on every save; sections nobody owns ride along untouched, so
// a mod keeps its own fields in sections of its own.

#include <string>
#include <utility>
#include <vector>

namespace gh2::save
{
    struct Section
    {
        std::string name;
        std::vector<std::pair<std::string, std::string>> values;

        const std::string *get(const std::string &key) const;
        void set(const std::string &key, const std::string &value);
    };

    struct Store
    {
        std::vector<Section> sections;

        const Section *find(const std::string &name) const;
        // Appended when missing.
        Section &section(const std::string &name);
        template <typename Pred>
        void removeIf(Pred pred)
        {
            std::erase_if(sections, [&](const Section &s) { return pred(s.name); });
        }

        std::string text() const;
        static Store parse(const std::string &text);
    };

    enum class ReadResult
    {
        kOk,
        kMissing,
        kCorrupt,
    };

    ReadResult readFile(const std::string &path, Store &out);
    // Beside the old file and renamed over it. False when it could not be.
    bool writeFile(const std::string &path, const Store &store);
}
