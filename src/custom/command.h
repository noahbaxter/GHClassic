#pragma once

// {custom ...}, the script's questions about custom songs and the list's
// sort and instrument (ui/dta/quickplay*.dta), and the locale's names for
// the playing song's sections.
//
//   {custom ours <song>}                  TRUE if it is a custom song
//   {custom has <song> <difficulty>}      TRUE if charted in the part it
//                                         plays; every one is for a song
//                                         not ours
//   {custom nearest <song> <difficulty>}  that one if charted, else the
//                                         nearest that is, harder first
//   {custom base <song>}                  its files for the part it
//                                         plays, songs/<id>/<id>; it is
//                                         the song being started
//   Each with a TRUE after: the part its lead, as quickplay plays on
//   guitar, whatever the instrument picked (Face-Off's).
//   {custom tag <song> <key>}             its song.ini's tag, rich text
//                                         taken out; "" if it has none;
//                                         name, as the list shows it
//   {custom length <song>}                its length, m:ss
//   {custom best <song>}                  its best on its hardest
//                                         difficulty played, as played
//                                         now: score, stars, percent
//                                         or FC; else NOT PLAYED
//   {custom art <song>}                   TRUE once its album art is
//                                         read off its disk, which this
//                                         starts, and the art of the
//                                         20 songs either side of it:
//                                         ui/image/ghc_art_<song>.png
//   {custom ready <song>}                 TRUE once what its preview
//                                         plays is read off its disk,
//                                         which this starts (audio.h
//                                         stemsReady)
//   {custom level <song> <instrument>}    guitar, rhythm, bass, drums,
//                                         vocals or keys: its difficulty in
//                                         song.ini, "" if it has none
//                                         or not the part
//   {custom track <song> <instrument>}    TRUE if its chart has a track
//                                         for guitar (any), lead, rhythm,
//                                         bass, drums or vocals
//   {custom sort [sort_<name>]}           the list's sort (catalog.h's
//                                         Sort); with one, TRUE if it
//                                         is that now and was not
//   {custom instrument [guitar|bass]}     the one played; with one, set
//                                         to that
//   {custom scan [<seen> <expected>]}     how far the scan of the song
//                                         folders is, "" once done; with
//                                         two numbers, said to be that far
//   {custom section <n>}                  the song being played's nth
//                                         section's name

namespace gh2::custom
{
    void installCommand();
}
