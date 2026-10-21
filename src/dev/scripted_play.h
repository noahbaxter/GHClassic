#pragma once

// scripted_play: a guitarist for scenarios, playing player 0's guitar track
// at set song times to check the note engine's rules (note_engine.h) the way
// a player meets them. Each action goes in as GuitarController::OnMsg
// (retail 0x236798, 0x236908) sends a pad's: frets to BeatMatcher's
// FretButtonDown and FretButtonUp, a fret pressed with none at or above it
// held a hammer-on (NonStrumSwing), the highest of several let go a pull-off,
// and the strum bar a Swing on the highest fret held.
//
// Times are the watcher's, the song's ms plus its sync, as gems are judged.
// An action goes in on the frame nearest its time; the log says how near.
//
//   {ghc_play now}                    the time now
//   {ghc_play gems}                   how many gems the track has
//   {ghc_play hopos}                  how many of them are HOPOs
//   {ghc_play gem <i> ms|frets|hopo|played}
//   {ghc_play find lone|hammer|run <ms>}  the first such gem from then on, or
//                                     -1. lone: one fret, no HOPO, 400 ms from
//                                     any other gem. hammer: a one-fret HOPO,
//                                     with 150 ms clear each side, after a
//                                     one-fret gem. run: the first of ten gems
//                                     100 ms or more apart, 170 ms after the
//                                     gem before, which has passed by then
//   {ghc_play strum <i> <offset>}     at the gem's ms plus offset: every fret let
//                                     go, the gem's pressed, then a strum; its
//                                     frets stay held
//   {ghc_play press <i> <offset>}     the gem's frets pressed then
//   {ghc_play hammer <i> <offset>}    its fret hammered on then, or pulled off to
//                                     where it is below the frets held
//   {ghc_play release <ms>}           every fret let go then
//   {ghc_play play <i> <count>}       that many gems from it strummed on time,
//                                     each with only its own frets held
//   {ghc_play mark <ms> <var> score|streak}  $var set to player 0's then, in
//                                     order with the actions at that ms
//   {ghc_play scoring crowd|stars <difficulty>}  the live config's crowd range
//                                     low end, or top star threshold
//   {ghc_play idle}                   TRUE once every action has gone in
//   {ghc_play worst}                  the furthest a strum or fret press has
//                                     gone in from its time this song, ms: past
//                                     a frame's half, the game stalled
//   {ghc_play dump}                   every gem to the log

#include "addresses.h"

class PS2Runtime;

namespace gh2::scripted_play
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
