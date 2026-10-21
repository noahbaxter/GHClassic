#pragma once

// GH2's beatmatch: a track's gems, and the watchers that judge them.

#include "guest/object.h"

#include <cmath>

namespace gh2::guest
{
    // GameGem: one gem of a track.
    struct GameGem : Object
    {
        static constexpr uint32_t kSize = 0x10u;
        using Object::Object;

        float ms() const { return get<float>(0x00u); }
        int32_t tick() const { return get<int32_t>(0x04u); }
        uint8_t slots() const { return get<uint8_t>(0x0cu); } // its frets, a bit each
        bool hopo() const { return (get<uint8_t>(0x0du) & 1u) != 0u; } // WillBeNoStrum's
        uint8_t players() const { return get<uint8_t>(0x0eu); } // whose, a bit each, or none for all
    };

    // GameGemList: a track's gems, in a vector at +8.
    struct GameGemList : Object
    {
        using Object::Object;

        Vector<GameGem> gems() const { return member<Vector<GameGem>>(0x08u); }
        int size() const { return gems().size(); }
        GameGem operator[](int i) const { return gems()[i]; }

        // ClosestMarkerIdx (0x24d1c8): the gem nearest `ms`, a tie the earlier's.
        int closest(float ms) const
        {
            const Vector<GameGem> all = gems();
            const int count = all.size();
            int i = 0;
            while (i < count && all[i].ms() < ms)
                ++i;
            if (i == 0)
                return 0;
            if (i == count)
                return count - 1;
            return std::fabs(ms - all[i].ms()) < std::fabs(ms - all[i - 1].ms()) ? i : i - 1;
        }
    };

    // MultiGemInfo: a gem as SongParser::OnGemEnd (0x245ee8) fills it in,
    // before it is a GameGem.
    struct MultiGemInfo : Object
    {
        using Object::Object;

        uint32_t slots() const { return get<uint32_t>(0x04u); }
        int32_t tick() const { return get<int32_t>(0x10u); }
        int32_t ticks() const { return get<int32_t>(0x14u); } // its length
        void setNoSustain(bool none) const { set<int32_t>(0x1cu, none ? 1 : 0); }
    };

    // GameGemInfoList: an int a gem in the array at +4, 1 once it is hit.
    struct GameGemInfoList : Object
    {
        using Object::Object;

        bool played(int i) const
        {
            return load<int32_t>(rdram, get<uint32_t>(0x04u) + static_cast<uint32_t>(i) * 4u) != 0;
        }
    };

    // TrackWatcherParent: the BeatMatcher, as its watchers see it.
    struct TrackWatcherParent : Object
    {
        using Object::Object;

        Virtual getNow() const { return virtualAt(0x00u, 0x10u); } // BeatMatcher::GetNow (0x38ccc0)
    };

    // TrackWatcherImpl (ctor, retail 0x248100): judges one player's track.
    struct TrackWatcherImpl : Object
    {
        using Object::Object;

        int32_t player() const { return get<int32_t>(0x00u); } // for a gem's player bits
        GameGemList gems() const { return pointee<GameGemList>(0x04u); }
        bool played(int i) const { return pointee<GameGemInfoList>(0x08u).played(i); }
        TrackWatcherParent parent() const { return pointee<TrackWatcherParent>(0x0cu); }
        float slop() const { return get<float>(0x10u); } // the hit window each side, ms
        int32_t lastHit() const { return get<int32_t>(0x14u); }
        int32_t captureEnd() const { return get<int32_t>(0x18u); } // gems before it are not judged
        float sync() const { return get<float>(0x28u); } // ms added to now
        int32_t lastPassed() const { return get<int32_t>(0x4cu); } // CheckForPasses': every gem to it is done with
        void setLastMissMs(float ms) const { set<float>(0x54u, ms); } // SendMiss sends none within 50 ms of it
        uint32_t vtable() const { return get<uint32_t>(0x78u); }

        // Playable's (0x249088) opposite: hit already, or another player's.
        bool done(int i) const
        {
            const uint8_t players = gems()[i].players();
            return played(i) || (players != 0u && ((players >> player()) & 1u) == 0u);
        }
    };

    // GuitarTrackWatcherImpl (ctor, 0x24d400).
    struct GuitarTrackWatcherImpl : TrackWatcherImpl
    {
        using TrackWatcherImpl::TrackWatcherImpl;

        uint32_t held() const { return get<uint32_t>(0x84u); } // frets held, a bit each
        int32_t hopoGem() const { return get<int32_t>(0x88u); } // last hit by a hammer-on or pull-off
        void setHopoLockedGem(int32_t gem) const { set<int32_t>(0x8cu, gem); } // one no longer hammered, or -1
        void setLastHitMs(float ms) const { set<float>(0x94u, ms); } // TryToHopo hits nothing at it
        void setPendingAfterHopo(int32_t gem) const { set<int32_t>(0xa4u, gem); }
        float leniency() const { return get<float>(0xa8u); } // 60 ms
    };

    // BeatMatcher: its tracks' watchers (+0x38) and the one it plays (+0xa0).
    struct BeatMatcher : Object
    {
        using Object::Object;

        float now() const { return get<float>(0xa4u); } // the song's ms this frame
        float lastSwing() const { return get<float>(0xc4u); } // when it last took a strum
        void setLastSwing(float ms) const { set<float>(0xc4u, ms); }

        // The watcher its swings go to, as TrackWatcher::Swing (0x247f68)
        // finds it: the track's TrackWatcher holds its impl at +0. 0 before
        // it has any.
        uint32_t swingWatcher() const
        {
            const uint32_t watchers = get<uint32_t>(0x38u);
            const uint32_t track = watchers ? load<uint32_t>(rdram, watchers + 4u * get<uint32_t>(0xa0u)) : 0u;
            const uint32_t impl = track ? load<uint32_t>(rdram, track) : 0u;
            return impl ? Object(rdram, impl).virtualAt(0x78u, 0x38u).self : 0u;
        }
    };
}
