#include "ui/text_fit.h"

#include "guest.h"
#include "milo/layout.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <iostream>
#include <tuple>

namespace gh2::text_fit
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
    }

    namespace
    {
        // Where each character of `line` ends, as RndText::GetStringWidth
        // (0x1dbca0) has it: each one's advance after the one before it
        // (the first after none), times the size.
        std::vector<float> ends(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t font,
                                const std::string &line, float size)
        {
            const auto byte = [](char x) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(x))); };
            std::vector<float> out;
            char before = 0;
            for (const char c : line)
            {
                runtime->callGuestFunction(rdram, ctx, s_addresses->rndFontCharAdvance, {font, byte(before), byte(c)});
                out.push_back((out.empty() ? 0.0f : out.back()) + ctx->f[0] * size);
                before = c;
            }
            return out;
        }

        uint32_t fontOf(uint8_t *rdram, uint32_t text)
        {
            return text != 0u ? load<uint32_t>(rdram, text + ::milo::text::kFont) : 0u;
        }

        float width(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t text, const std::string &line,
                    float size)
        {
            const uint32_t font = fontOf(rdram, text);
            const auto at = font != 0u ? ends(rdram, ctx, runtime, font, line, size) : std::vector<float>{};
            return at.empty() ? 0.0f : at.back();
        }
    }

    std::string fit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t text, const std::string &line,
                    float size, float room)
    {
        const uint32_t font = fontOf(rdram, text);
        if (font == 0u || line.empty())
            return line;
        const std::vector<float> at = ends(rdram, ctx, runtime, font, line, size);
        if (at.back() <= room)
            return line;
        // Windows-1252's ellipsis, where the font has it.
        const uint32_t chars = load<uint32_t>(rdram, font + ::milo::font::kChars);
        const bool own = chars != 0u && std::strchr(reinterpret_cast<const char *>(getMemPtr(rdram, chars)), '\x85');
        const std::string ellipsis = own ? "\x85" : "...";
        const float tail = ends(rdram, ctx, runtime, font, ellipsis, size).back();
        size_t n = at.size();
        while (n > 0u && at[n - 1u] + tail > room)
            --n;
        while (n > 0u && line[n - 1u] == ' ')
            --n;
        return line.substr(0u, n) + ellipsis;
    }

    std::string upper(std::string latin1)
    {
        // à to þ are À to Þ a row up, but for ÷; ÿ's capital is past
        // Latin-1.
        for (char &c : latin1)
        {
            const auto u = static_cast<unsigned char>(c);
            c = static_cast<char>(u == 0xFF ? 'Y' : u >= 0xE0 && u != 0xF7 ? u - 0x20 : std::toupper(u));
        }
        return latin1;
    }

    namespace
    {
        // `line` broken between words onto at most `lines` lines of `room`,
        // as fit measures them, the last cut to fit.
        std::vector<std::string> wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t text,
                                      const std::string &line, float size, float room, size_t lines)
        {
            std::vector<std::string> words;
            for (size_t at = 0u; at < line.size();)
            {
                const size_t end = std::min(line.find(' ', at), line.size());
                if (end > at)
                    words.push_back(line.substr(at, end - at));
                at = end + 1u;
            }
            // As many words as fit a line on it, a word wider than a line
            // alone; the last line takes what is left, cut to fit.
            const auto fits = [&](const std::string &s) { return fit(rdram, ctx, runtime, text, s, size, room) == s; };
            std::vector<std::string> out;
            size_t w = 0u;
            while (w < words.size() && out.size() + 1u < lines)
            {
                std::string next = words[w++];
                while (w < words.size() && fits(next + " " + words[w]))
                    next += " " + words[w++];
                out.push_back(fit(rdram, ctx, runtime, text, next, size, room));
            }
            if (w < words.size())
            {
                std::string rest = words[w++];
                while (w < words.size())
                    rest += " " + words[w++];
                out.push_back(fit(rdram, ctx, runtime, text, rest, size, room));
            }
            return out;
        }

        //   {text_fit upper <text>}                  in capitals, for a type
        //                                            with no small letters
        //                                            (the receipt's)
        //   {text_fit fit_text <text> <Text> <size> <width>}  cut to that
        //                                            width with an ellipsis,
        //                                            measured in a Text's
        //                                            font at its own size
        //                                            times that, as
        //                                            get_string_width
        //   {text_fit width <text> <Text> <size>}    its width, as fit_text
        //                                            measures it
        //   {text_fit marquee <text> <Text> <size> <width> <step>}  that at
        //                                            the nth step of
        //                                            scrolling along it, where
        //                                            it is too wide; as it is
        //                                            where not
        //   {text_fit wrap <text> <label> <size> <width> <lines> <n>}  its
        //                                            nth line broken between
        //                                            words onto at most that
        //                                            many, "" past the last;
        //                                            n -1, how many lines it
        //                                            takes
        script::Node textFitCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "upper")
            {
                // The capitals the receipt's font lacks as their letters,
                // and © and ® spelled out.
                std::string text;
                for (const char c : upper(call.text(2)))
                    switch (static_cast<unsigned char>(c))
                    {
                    case 0xC6: text += "AE"; break;
                    case 0xC7: text += 'C'; break;
                    case 0xD0: text += 'D'; break;
                    case 0xD8: text += 'O'; break;
                    case 0xDE: text += "TH"; break;
                    case 0xDF: text += "SS"; break;
                    case 0xA9: text += "(C)"; break;
                    case 0xAE: text += "(R)"; break;
                    default: text += c;
                    }
                return call.word(text);
            }
            if (op != "fit_text" && op != "marquee" && op != "wrap" && op != "width")
            {
                std::cerr << "[text_fit] usage: {text_fit upper <text>}, {text_fit width <text> <Text> <size>}, "
                             "{text_fit fit_text|marquee|wrap <text> <Text|label> <size> <width> ...}"
                          << std::endl;
                return {};
            }
            // A BandLabel's type's font is the label's own RndText
            // (UILabel::DrawShowing, 0x217ec8), the label being its
            // Hmx::Object less where a BandLabel keeps that
            // (BandLabel::BandLabel, 0x28dc10); a Text's own Hmx::Object is
            // the last 0x30 of its 0x1b0 (RndText::NewObject, 0x365fc0).
            constexpr uint32_t kLabelObject = 0x1a0u, kLabelText = 0x140u, kTextObject = 0x180u;
            const bool ownText = op != "wrap";
            const uint32_t object = call.object(3);
            const uint32_t text = object == 0u ? 0u
                                  : ownText    ? object - kTextObject
                                               : load<uint32_t>(call.rdram, object - kLabelObject + kLabelText);
            // A Text's width as get_string_width has it is its own size
            // (RndText::SetSize, 0x1da940) times the font's.
            constexpr uint32_t kTextSize = 0x150u;
            const float size = call.number(4) * (ownText && text != 0u ? load<float>(call.rdram, text + kTextSize) : 1.0f);
            const float room = call.number(5);
            if (op == "width")
                return script::floatNode(width(call.rdram, call.ctx, call.runtime, text, call.text(2), size));
            std::string out;
            if (op == "marquee")
            {
                // Held kMarqueeHold steps at its start, then a character a
                // step until its end shows, held there as long, and round
                // again; where it fits, as it is.
                constexpr size_t kMarqueeHold = 12u;
                const std::string line = call.text(2);
                const auto fits = [&](size_t from)
                {
                    const std::string rest = line.substr(from);
                    return fit(call.rdram, call.ctx, call.runtime, text, rest, size, room) == rest;
                };
                // The last line marqueed, and the first start whose rest fits.
                static std::tuple<std::string, uint32_t, float, float> s_of;
                static size_t s_last = 0u;
                if (std::tuple(line, text, size, room) != s_of)
                {
                    s_of = {line, text, size, room};
                    s_last = 0u;
                    while (s_last < line.size() && !fits(s_last))
                        ++s_last;
                }
                const size_t step = static_cast<size_t>(std::max(0.0f, call.number(6))) % (s_last + 2u * kMarqueeHold);
                const size_t from = step < kMarqueeHold ? 0u : std::min(step - kMarqueeHold, s_last);
                out = fit(call.rdram, call.ctx, call.runtime, text, line.substr(from), size, room);
            }
            else if (ownText)
                out = fit(call.rdram, call.ctx, call.runtime, text, call.text(2), size, room);
            else
            {
                // Asked for a line at a time, and a printout tries each text
                // at a few widths, so the last few texts' lines are kept.
                using Key = std::tuple<std::string, uint32_t, float, float, size_t>;
                static std::deque<std::pair<Key, std::vector<std::string>>> s_wrapped;
                const Key key{call.text(2), text, size, room, static_cast<size_t>(std::max(1.0f, call.number(6)))};
                auto it = std::find_if(s_wrapped.begin(), s_wrapped.end(), [&](const auto &w) { return w.first == key; });
                if (it == s_wrapped.end())
                {
                    if (s_wrapped.size() == 32u)
                        s_wrapped.pop_back();
                    s_wrapped.emplace_front(
                        key, wrap(call.rdram, call.ctx, call.runtime, text, std::get<0>(key), size, room, std::get<4>(key)));
                    it = s_wrapped.begin();
                }
                const std::vector<std::string> &lines = it->second;
                if (call.number(7) < 0.0f)
                    return {static_cast<uint32_t>(lines.size()), script::kInt};
                const size_t n = static_cast<size_t>(call.number(7));
                out = n < lines.size() ? lines[n] : "";
            }
            return call.word(out);
        }
    }

    void install(const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("text_fit", textFitCommand);
    }
}
