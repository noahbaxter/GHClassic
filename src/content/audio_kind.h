#pragma once

// What audio a file's first bytes say it is, whatever it is named: the
// kinds content/audio_file.h decodes, or what else it is, for saying why
// not. The first 128 bytes are enough.

#include <cstdint>
#include <cstring>
#include <string>

namespace gh2::audio_file
{
    // "vorbis", "opus", "mp3" or "wav" (AIFF too); else "" with `what` naming it where
    // it is known.
    inline std::string kind(const uint8_t *head, size_t size, std::string *what = nullptr)
    {
        const auto at = [&](size_t o, const char *text)
        {
            const size_t n = std::strlen(text);
            return size >= o + n && std::memcmp(head + o, text, n) == 0;
        };
        const auto other = [&](const char *name)
        {
            if (what)
                *what = name;
            return std::string();
        };
        if (at(0u, "OggS"))
        {
            // The first page holds the codec's id header.
            const std::string page(reinterpret_cast<const char *>(head), size < 128u ? size : 128u);
            if (page.find("OpusHead") != std::string::npos)
                return "opus";
            if (page.find("vorbis") != std::string::npos)
                return "vorbis";
            return other(page.find("FLAC") != std::string::npos ? "Ogg FLAC" : "Ogg of another codec");
        }
        // dr_wav reads AIFF as well.
        if ((at(0u, "RIFF") && at(8u, "WAVE")) || (at(0u, "FORM") && (at(8u, "AIFF") || at(8u, "AIFC"))))
            return "wav";
        // An ID3 tag first, or straight into a frame's sync bits; a layer
        // of 0 is AAC's ADTS, not MPEG audio.
        if (at(0u, "ID3") || (size >= 2u && head[0] == 0xffu && (head[1] & 0xe0u) == 0xe0u && (head[1] & 0x06u) != 0u))
            return "mp3";
        if (size >= 2u && head[0] == 0xffu && (head[1] & 0xf6u) == 0xf0u)
            return other("AAC");
        if (at(4u, "ftyp"))
            return other("MPEG-4 audio");
        if (at(0u, "fLaC"))
            return other("FLAC");
        return other("not audio GH Classic knows");
    }
}
