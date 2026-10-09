#pragma once

// Standard MIDI files on the host: read into tracks of events at absolute
// ticks, and written back.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::midi
{
    using Bytes = std::vector<uint8_t>;

    struct Event
    {
        uint32_t tick = 0;
        uint8_t status = 0; // 0xff meta, 0xf0/0xf7 sysex, else a channel message
        uint8_t meta = 0;   // a meta event's type
        Bytes data;         // after the status (and a meta's type), lengths dropped
    };

    struct Track
    {
        std::string name; // its 0x03 meta, kept out of events
        std::vector<Event> events; // by tick, end of track dropped
    };

    struct File
    {
        uint16_t format = 1;
        uint16_t division = 480;
        std::vector<Track> tracks;
    };

    std::optional<File> parse(const Bytes &bytes);

    // Each track's events stably by tick, named, and ended at its last.
    Bytes write(const File &file);

    // A text event (meta 0x01).
    Event text(uint32_t tick, const std::string &text);

    // A file's tempos (meta 0x51 on its first track), as a tick's time
    // in seconds and back: 120 beats a minute until set. A tempo of 0 is
    // none.
    class TempoMap
    {
    public:
        explicit TempoMap(const File &file);

        double seconds(uint32_t tick) const;

        // The tick nearest a time.
        uint32_t tick(double seconds) const;

    private:
        // Where a tempo starts, its time there and its seconds a tick.
        struct Span
        {
            uint32_t tick = 0u;
            double at = 0.0, perTick = 0.0;
        };

        std::vector<Span> m_spans;
    };
}
