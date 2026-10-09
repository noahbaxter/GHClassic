#pragma once

// A scenario: a DTA file of steps run on the game thread from the first UI
// poll, to put the game straight into a state and check it.
//
//   {gamecfg set mode quickplay}       a command, evaluated
//   (wait_screen sel_character_new_screen)  until current and not in transition
//   (wait 2)                           seconds
//   (wait_until {char_single are_chars_loaded})  until not 0, checked each poll
//   (print {game get_character} ...)   each value, to stderr
//   (expect {player0 score} 158678)    fails the run unless the value matches
//   (expect {taskmgr seconds} 33.47 0.1)  or, numbers, is within the third
//   (press green)                      a fret's colour, up, down, left or right,
//                                      pressed and let go as a player would
//   (clock 100)                        the UI clock runs from there, 1/64 s a poll
//   (freeze 103 2.5 3.9)               holds TaskMgr's UI seconds (once a clock
//                                      reaches them), seconds and beat there,
//                                      with no camera shake and no hair
//   (still)                            no camera shake and no hair, the clocks
//                                      left running
//   (shot metal_1)                    the next frame, as shot_metal_1 (--shots)
//   (poke {dir find a.btn} 364 7)      the word at that offset of the object, an int or a float
//   (peek {dir find a.btn} 364)        prints that word, as an int and as a float
//   (cam balcony {world find a.cam})   prints that Cam's world transform and y fov under that name
//   (dump menu)                      the next frame's draws, as draws_menu.json (--shots)
//   (transplant "../x/frame.ram" frame)  the retail game's memory in place of this one's
//                                      (dev/transplant.h), then its frame as a shot and a dump
//   (needs gh1 gh2x)                   skips the run unless those games' discs
//                                      are mounted (content/games.h)
//   (quit)                           closes the app
//   #include common/first_boot.dta     that file's steps, from beside this one
//
// A wait_screen or wait_until that takes more than 30 s, or the seconds
// given after its screen or condition, fails the run and quits, as does the
// UI going 8 s without a poll, after naming what each thread waits on and,
// for one still running, the code and scripts on its stack.
// {ghc_log ...} prints values and their types from any script.

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::scenario
{
    // False if the file cannot be read.
    bool load(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
