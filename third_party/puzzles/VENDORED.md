# Simon Tatham's Portable Puzzle Collection, vendored

Source: https://git.tartarus.org/simon/puzzles.git at commit `616da16` (2026-09-22), MIT (see
LICENCE).

Kept, unmodified:
- `solo.c` (Sudoku: generator, solver, difficulty grading, drawing, input)
- the shared core it needs: `midend.c`, `drawing.c`, `misc.c`, `malloc.c`, `random.c`,
  `divvy.c`, `dsf.c`, `version.c`, `version.h`, `puzzles.h`

Not kept: the other puzzles, the desktop and web front ends, printing (`ps.c`), the build
system, documentation and icons.

inx-pro's own file here: `generated-games.h`, the list of puzzles that upstream's build
generates. Every file is compiled with `COMBINED` defined, so each puzzle links under its own
name (`solo`) instead of `thegame` and more can be added side by side.

## How it is built

`src/games/puzzles/Puzzle*.c` each include one of these files (one translation unit per file, as
upstream builds them), behind `INX_HAS_PUZZLES`. The front end, the drawing and input glue for
the e-paper UI, is `src/games/puzzles/PuzzleFrontend.cpp`; each puzzle's levels and controls are
in `src/games/puzzles/PuzzleGames.cpp`, and the screen that plays them is
`src/activity/settings/PuzzleActivity.cpp`. `test/run_puzzles_host_test.sh`
builds the same files on the host.

## Updating

Copy the files above from a newer commit and update the commit here, then run
`test/run_puzzles_host_test.sh` and build an env. The drawing API is versioned
(`drawing_api.version`); a new version means the front end needs changes. A new puzzle needs
its `.c` file, any core files it uses (upstream's `CMakeLists.txt` lists the core), a wrapper in
`src/games/puzzles/` and a `GAME()` line in `generated-games.h`.
