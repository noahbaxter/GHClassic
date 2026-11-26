#include "save/best.h"

#include <sstream>

namespace gh2::save
{
    std::optional<Best> parseBest(const std::string &text)
    {
        std::istringstream in(text);
        Best best;
        int fullCombo = 0;
        if (!(in >> best.score >> best.stars >> best.percent >> best.streak >> fullCombo))
            return std::nullopt;
        best.fullCombo = fullCombo != 0;
        // Saves from before no_fail have no sixth.
        int failed = 0;
        if (in >> failed)
            best.failed = failed != 0;
        return best;
    }

    std::string bestText(const Best &best)
    {
        return std::to_string(best.score) + ' ' + std::to_string(best.stars) + ' ' + std::to_string(best.percent) + ' ' +
               std::to_string(best.streak) + ' ' + (best.fullCombo ? '1' : '0') + ' ' + (best.failed ? '1' : '0');
    }

    Best better(const Best &a, const Best &b)
    {
        // A run that finished clean outranks one that failed, whatever its score.
        Best best = a.failed != b.failed ? (a.failed ? b : a) : (b.score > a.score ? b : a);
        best.fullCombo = a.fullCombo || b.fullCombo;
        return best;
    }

    std::string bestKey(const std::string &song, const std::string &engine, const std::string &part,
                        const std::string &difficulty, const std::string &controller)
    {
        return song + '.' + engine + '.' + part + '.' + difficulty + '.' + controller;
    }

    std::string bestSection(std::optional<int> profile, const std::string &mode)
    {
        const std::string name = mode.empty() ? kBestSection : mode + ' ' + kBestSection;
        return profile ? "profile " + std::to_string(*profile + 1) + ' ' + name : name;
    }
}
