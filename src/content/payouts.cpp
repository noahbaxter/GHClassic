#include "content/payouts.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace gh2
{
    namespace
    {
        constexpr size_t kEasy = 1u, kMedium = 2u, kExpert = 4u; // a table's rows, past its key
        constexpr int64_t kStep = 5;                             // every award a multiple of it
        constexpr int64_t kCareers = 3;                          // that start with cash: all but Easy's

        std::string keyOf(const dtb::Node &node)
        {
            return node.type == dtb::kArray && !node.nodes.empty() ? node.nodes[0].text : node.text;
        }

        dtb::Node *child(dtb::Node &parent, const std::string &key)
        {
            for (dtb::Node &n : parent.nodes)
                if (n.type == dtb::kArray && keyOf(n) == key)
                    return &n;
            return nullptr;
        }

        // Every (price n) under `node`.
        int64_t prices(const dtb::Node &node)
        {
            if (node.type != dtb::kArray)
                return 0;
            if (keyOf(node) == "price" && node.nodes.size() > 1u)
                return node.nodes[1].integer;
            int64_t sum = 0;
            for (const dtb::Node &n : node.nodes)
                sum += prices(n);
            return sum;
        }

        int64_t sum(const dtb::Node &row)
        {
            int64_t s = 0;
            for (size_t i = 1u; i < row.nodes.size(); ++i)
                s += row.nodes[i].integer;
            return s;
        }

        int64_t stepped(double v, bool down)
        {
            const double steps = v / static_cast<double>(kStep);
            return static_cast<int64_t>(down ? std::floor(steps) : std::llround(steps)) * kStep;
        }

        // `row`'s awards each `by` its own, and their sum.
        int64_t scale(dtb::Node &row, const dtb::Node &from, double by, bool down)
        {
            for (size_t i = 1u; i < row.nodes.size() && i < from.nodes.size(); ++i)
                row.nodes[i].integer = static_cast<int32_t>(stepped(from.nodes[i].integer * by, down));
            return sum(row);
        }
    }

    bool scalePayouts(dtb::Node &campaign, const dtb::Node &store)
    {
        dtb::Node *cash = child(campaign, "cash");
        const dtb::Node *order = dtb::find(campaign, "order");
        dtb::Node *stars = cash ? child(*cash, "star_awards") : nullptr;
        dtb::Node *status = cash ? child(*cash, "status_awards") : nullptr;
        if (!order || !stars || !status || stars->nodes.size() <= kExpert || status->nodes.size() <= kExpert)
            return false;
        int64_t songs = 0;
        for (size_t i = 1u; i < order->nodes.size(); ++i)
            if (!order->nodes[i].nodes.empty())
                songs += static_cast<int64_t>(order->nodes[i].nodes.size()) - 1;
        const dtb::Node discStars = stars->nodes[kMedium], discStatus = status->nodes[kMedium];
        const int64_t perSong = sum(discStars), venues = sum(discStatus), discRow = songs * perSong + venues;
        const dtb::Node *starting = dtb::find(*cash, "starting");
        const int64_t started = kCareers * (starting && starting->nodes.size() > 1u ? starting->nodes[1].integer : 0);
        const int64_t cost = prices(store);
        if (songs <= 0 || perSong <= 0 || venues <= 0 || cost <= started || status->nodes[kEasy].nodes.size() < 2u)
            return false;
        // A difficulty's quarter, rounded up so the four are never short.
        const int64_t row = (cost - started + 4 * kStep - 1) / (4 * kStep) * kStep;

        // Medium's, Hard's and Expert's: the stars scaled, and the venues
        // sharing out the rest, the last of them taking what rounding left.
        const double by = static_cast<double>(row) / static_cast<double>(discRow);
        for (size_t d = kMedium; d <= kExpert; ++d)
        {
            const int64_t left = row - songs * scale(stars->nodes[d], discStars, by, false);
            dtb::Node &awards = status->nodes[d];
            int64_t paid = scale(awards, discStatus, static_cast<double>(left) / static_cast<double>(venues), true);
            for (size_t i = awards.nodes.size(); i-- > 1u;)
                if (awards.nodes[i].integer != 0)
                {
                    awards.nodes[i].integer += static_cast<int32_t>(left - paid);
                    break;
                }
        }
        // Easy's: its stars, and the rest on its first venue.
        const int64_t easy = songs * scale(stars->nodes[kEasy], discStars, static_cast<double>(row) / static_cast<double>(songs * perSong), true);
        status->nodes[kEasy].nodes[1].integer = static_cast<int32_t>(row - easy);
        return true;
    }

    bool openEasyStore(dtb::Node &script)
    {
        static const std::optional<dtb::Node> shown = dtb::parse(
            "{$this enable cm_store.btn} {cm_cash.lbl set_showing 1} {cm_cash_title.lbl set_showing 1}");
        if (!shown)
            return false;
        if (script.type == dtb::kCommand && script.nodes.size() > 2u && script.nodes[0].text == "new" &&
            script.nodes[2].text == "career_panel")
        {
            dtb::Node *enter = child(script, "enter");
            if (enter)
                enter->nodes.insert(enter->nodes.end(), shown->nodes.begin(), shown->nodes.end());
            return enter != nullptr;
        }
        for (dtb::Node &n : script.nodes)
            if (openEasyStore(n))
                return true;
        return false;
    }
}
