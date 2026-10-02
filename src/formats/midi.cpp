#include "formats/midi.h"

#include <algorithm>

namespace gh2::midi
{
    namespace
    {
        struct Reader
        {
            const Bytes &b;
            size_t o = 0;
            bool ok = true;

            uint8_t u8()
            {
                if (o >= b.size())
                {
                    ok = false;
                    return 0u;
                }
                return b[o++];
            }

            uint32_t be(int n)
            {
                uint32_t v = 0u;
                for (int i = 0; i < n; ++i)
                    v = (v << 8) | u8();
                return v;
            }

            uint32_t vlq()
            {
                uint32_t v = 0u;
                for (int i = 0; i < 4 && ok; ++i)
                {
                    const uint8_t c = u8();
                    v = (v << 7) | (c & 0x7fu);
                    if (!(c & 0x80u))
                        break;
                }
                return v;
            }

            Bytes take(uint32_t n)
            {
                if (o + n > b.size())
                {
                    ok = false;
                    return {};
                }
                Bytes out(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
                o += n;
                return out;
            }
        };

        void putBe(Bytes &out, uint32_t v, int n)
        {
            for (int i = n - 1; i >= 0; --i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        void putVlq(Bytes &out, uint32_t v)
        {
            uint8_t parts[5];
            int n = 0;
            do
            {
                parts[n++] = static_cast<uint8_t>(v & 0x7fu);
                v >>= 7;
            } while (v);
            while (n--)
                out.push_back(static_cast<uint8_t>(parts[n] | (n ? 0x80u : 0u)));
        }
    }

    std::optional<File> parse(const Bytes &bytes)
    {
        Reader r{bytes};
        if (r.be(4) != 0x4d546864u || r.be(4) != 6u)
            return std::nullopt;
        File file;
        file.format = static_cast<uint16_t>(r.be(2));
        const uint32_t count = r.be(2);
        file.division = static_cast<uint16_t>(r.be(2));
        for (uint32_t t = 0; t < count && r.ok; ++t)
        {
            if (r.be(4) != 0x4d54726bu)
                return std::nullopt;
            const uint32_t length = r.be(4);
            const size_t end = r.o + length;
            Track track;
            uint32_t tick = 0u;
            uint8_t running = 0u;
            while (r.o < end && r.ok)
            {
                tick += r.vlq();
                Event e{tick};
                uint8_t status = r.u8();
                if (status == 0xffu)
                {
                    e.status = status;
                    e.meta = r.u8();
                    e.data = r.take(r.vlq());
                    if (e.meta == 0x2fu)
                        continue;
                    if (e.meta == 0x03u && track.name.empty())
                    {
                        track.name.assign(e.data.begin(), e.data.end());
                        continue;
                    }
                }
                else if (status == 0xf0u || status == 0xf7u)
                {
                    e.status = status;
                    e.data = r.take(r.vlq());
                }
                else
                {
                    if (status & 0x80u)
                        running = status;
                    else
                        --r.o; // running status: this was data
                    e.status = running;
                    const uint8_t kind = running & 0xf0u;
                    e.data = r.take(kind == 0xc0u || kind == 0xd0u ? 1u : 2u);
                }
                track.events.push_back(std::move(e));
            }
            r.o = end;
            file.tracks.push_back(std::move(track));
        }
        if (!r.ok)
            return std::nullopt;
        return file;
    }

    Bytes write(const File &file)
    {
        Bytes out;
        putBe(out, 0x4d546864u, 4);
        putBe(out, 6u, 4);
        putBe(out, file.format, 2);
        putBe(out, static_cast<uint32_t>(file.tracks.size()), 2);
        putBe(out, file.division, 2);
        for (const Track &track : file.tracks)
        {
            std::vector<Event> events = track.events;
            std::stable_sort(events.begin(), events.end(),
                             [](const Event &a, const Event &b) { return a.tick < b.tick; });
            Bytes body;
            if (!track.name.empty())
            {
                putVlq(body, 0u);
                body.insert(body.end(), {0xffu, 0x03u});
                putVlq(body, static_cast<uint32_t>(track.name.size()));
                body.insert(body.end(), track.name.begin(), track.name.end());
            }
            uint32_t tick = 0u;
            for (const Event &e : events)
            {
                putVlq(body, e.tick - tick);
                tick = e.tick;
                body.push_back(e.status);
                if (e.status == 0xffu)
                    body.push_back(e.meta);
                if (e.status == 0xffu || e.status == 0xf0u || e.status == 0xf7u)
                    putVlq(body, static_cast<uint32_t>(e.data.size()));
                body.insert(body.end(), e.data.begin(), e.data.end());
            }
            body.insert(body.end(), {0x00u, 0xffu, 0x2fu, 0x00u});
            putBe(out, 0x4d54726bu, 4);
            putBe(out, static_cast<uint32_t>(body.size()), 4);
            out.insert(out.end(), body.begin(), body.end());
        }
        return out;
    }

    Event text(uint32_t tick, const std::string &text)
    {
        return {tick, 0xffu, 0x01u, Bytes(text.begin(), text.end())};
    }
}
