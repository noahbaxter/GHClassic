#pragma once

// A custom song's singer's mouth, from what is sung: GH2's singer lip-syncs
// to songs/<song>/<song>.voc, FaceFX's curves of mouth shapes (visemes) by
// the second; GH1's singer opens its mouth where its song's events say
// (gh1/face.h). One set of shapes makes both.
//
// A .voc, little-endian: "FACE", a u32 and a u16; the tool's name, a u16,
// its project's name, 12 bytes; the track's name, a u16, the file's size
// (u32), a u16; then a count of curves, each 8 bytes, its name, 8 bytes and
// its keys (each 2 bytes, its second and weight as floats, 8 bytes); then a
// tail. A name is a u32 length and its letters. GH2's own carry the 15
// mouth curves (kVisemes), Blink, and the head's and eyes'.

#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace gh2::custom
{
    // A syllable sung from `start` to `end`, in seconds: its letters ("hel",
    // "lo") as best they say the mouth's shapes.
    struct Sung
    {
        double start = 0.0, end = 0.0;
        std::string text;
    };

    enum class Viseme
    {
        Eat,
        If,
        Ox,
        Oat,
        Earth,
        Size,
        Church,
        Fave,
        Though,
        Bump,
        New,
        Told,
        Roar,
        Wet,
        Cage,
    };
    // Each Viseme's curve name.
    inline constexpr const char *kVisemes[] = {"Eat",  "If",     "Ox",   "Oat",  "Earth", "Size", "Church", "Fave",
                                               "Though", "Bump", "New",  "Told", "Roar",  "Wet",  "Cage"};
    static_assert(std::size(kVisemes) == size_t(Viseme::Cage) + 1u);

    // A mouth shape held from `start` to `end`, at `weight` (0 to 1).
    struct Shape
    {
        double start = 0.0, end = 0.0;
        Viseme viseme = Viseme::Eat;
        float weight = 0.0f;
    };

    // Each syllable's shapes: the consonant it starts on, its vowel held,
    // the consonant it ends on.
    std::vector<Shape> mouthShapes(const std::vector<Sung> &sung);

    // GH2's blink track (songs/_blinktrack/_blinktrack.voc, its singer's
    // blinks and nothing else) with `shapes` as its mouth curves; the track
    // as it is where it doesn't read as a .voc.
    std::vector<uint8_t> withMouth(const std::vector<uint8_t> &blinkTrack, const std::vector<Shape> &shapes);

    // When the mouth is open, for GH1's singer: the shapes' spans, those
    // under a twentieth of a second apart as one.
    std::vector<std::pair<float, float>> mouthOpen(const std::vector<Shape> &shapes);

    // When a .voc's mouth is open: where any mouth curve is past a third of
    // its weight, as mouthOpen's spans; none where it doesn't read.
    std::vector<std::pair<float, float>> vocMouthOpen(const std::vector<uint8_t> &voc);

    // A .voc's curves, by name, with their keys' count (tests).
    std::vector<std::pair<std::string, size_t>> vocCurves(const std::vector<uint8_t> &voc);
}
