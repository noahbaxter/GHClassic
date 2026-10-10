# Working here

## Things to look at go in review/

Every image, video, crop, compare sheet or render made for a person to look at is moved, not copied, into `review/` at the main checkout's root, even from a worktree. The folder is flat, with no subfolders, and each file is named `YYYY-MM-DD_HHMM_<what-it-shows>.<ext>` so the folder sorts by time. `review/` is gitignored. `tools/ghc.py arm` puts its shot sheet there already, and so does `ghc.py scenarios --review`.

Never point anyone at `runs/`, `.planning/` or a scratch folder to find something. What only the tools read (raw frames, run output, reference captures) stays in `runs/` or `.planning/<topic>/tools/`, and gets deleted once it stops being useful. `ghc.py` clears runs older than three days.
