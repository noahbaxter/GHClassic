#include "content/setlist_window.h"

#include <algorithm>
#include <random>

namespace gh2::setlist_window
{
    namespace
    {
        // The first row of a window with `row` half way down it.
        size_t windowAround(const std::vector<size_t> &tiers, size_t row)
        {
            size_t frames = 0u, r = row;
            while (r > 0u && frames < kWindowFrames / 2u)
            {
                frames += tiers[r - 1u] != tiers[r] ? 2u : 1u;
                --r;
            }
            return r;
        }
    }

    size_t windowFrom(const std::vector<size_t> &tiers, size_t first)
    {
        size_t frames = 0u, r = first;
        for (; r < tiers.size(); ++r)
        {
            frames += r == first || tiers[r - 1u] != tiers[r] ? 2u : 1u;
            if (frames > kWindowFrames)
                break;
        }
        return r - first;
    }

    std::optional<size_t> rowOf(const Window &window, const std::string &song)
    {
        const auto it = std::find(window.songs.begin(), window.songs.end(), song);
        if (it == window.songs.end())
            return std::nullopt;
        return static_cast<size_t>(it - window.songs.begin());
    }

    void moveTo(Window &window, size_t at)
    {
        window.first = windowAround(window.tiers, at);
        window.at = window.songs[at];
        window.atRow = at - window.first;
    }

    void liveRows(Window &window, const setlists::Live &live, std::vector<bool> &active)
    {
        std::vector<std::string> liveHeaders;
        const auto rows = live.rows();
        for (const setlists::LiveRow &row : *rows)
        {
            if (liveHeaders.empty() || liveHeaders.back() != row.header)
                liveHeaders.push_back(row.header);
            window.tiers.push_back(liveHeaders.size() - 1u);
            window.songs.push_back(row.song);
            active.push_back(true);
        }
        // Rows found or gone since: the song the list is on where it was
        // on the screen.
        if (rows != window.from && !window.at.empty())
            if (const auto at = rowOf(window, window.at))
                window.first = *at - std::min(*at, window.atRow);
        window.from = rows;
        window.tierNames = liveHeaders;
    }

    std::optional<size_t> scroll(Window &window, size_t row)
    {
        const size_t at = window.first + row;
        const bool nearTop = row < kWindowEdge && window.first > 0u;
        const bool nearEnd = row + kWindowEdge >= window.count && window.first + window.count < window.songs.size();
        if (at >= window.songs.size() || (!nearTop && !nearEnd))
            return std::nullopt;
        moveTo(window, at);
        return window.atRow;
    }

    std::optional<size_t> jump(Window &window, size_t tier)
    {
        const auto it = std::find(window.tiers.begin(), window.tiers.end(), tier);
        if (it == window.tiers.end())
            return std::nullopt;
        moveTo(window, static_cast<size_t>(it - window.tiers.begin()));
        return window.atRow;
    }

    std::optional<size_t> random(Window &window)
    {
        if (window.songs.size() < 2u)
            return std::nullopt;
        static std::mt19937 s_random{std::random_device{}()};
        const auto on = rowOf(window, window.at);
        size_t at = std::uniform_int_distribution<size_t>(0u, window.songs.size() - 2u)(s_random);
        if (on && at >= *on)
            ++at;
        moveTo(window, at);
        return window.atRow;
    }
}
