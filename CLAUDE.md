# Working here

## Things to look at go in review/

Every image, video, crop, compare sheet or render made for a person to look at is moved, not copied, into the main checkout's `review/<branch>/`, even from a worktree: the folder of the branch the work is on, with `/` as `-` (`review/feature-dancer/`). Each file is named `YYYY-MM-DD_HHMM_<what-it-shows>.<ext>` so a folder sorts by time; a set made together (a compare series, frames of one check) is one `YYYY-MM-DD_HHMM_<what>/` folder inside it. Nothing goes loose in `review/` itself. `review/` is gitignored.

`tools/ghc.py arm`, `record` and `scenarios --review` already write into the branch's folder. `tools/ghc.py review` lists the folders, each with its branch's last commit and whether it is checked out, landed or gone. When a branch lands or is dropped, `tools/ghc.py review archive --landed` moves its folder into `review/archive/`, keeping the stills and shrinking each video to 540 lines. Make one video of a thing, not several sizes of it.

Anything that moves (animation, transitions, timing, sound against picture) is reviewed as video: `tools/ghc.py record <scenario>`, with `(record on name)` and `(record off)` steps cutting it into clips. Stills of it come out of the .mp4 with ffmpeg. Shot sheets are for a screen that holds still.

Never point anyone at `runs/`, `.planning/` or a scratch folder to find something. What only the tools read (raw frames, run output, reference captures) stays in `runs/` or `.planning/<topic>/tools/`, and gets deleted once it stops being useful. `ghc.py` clears runs older than three days.
