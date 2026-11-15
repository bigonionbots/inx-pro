/**
 * @file SudokuActivity.cpp
 * @brief Definitions for SudokuActivity.
 */

#include "SudokuActivity.h"

#if INX_HAS_PUZZLES

#include <Arduino.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "activity/page/SubPage.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "util/SdIoMutex.h"

extern "C" {
#define COMBINED
#include "../../../third_party/puzzles/puzzles.h"
#undef COMBINED
}
// puzzles.h defines max() and min() as macros, which would break std::max and std::min.
#undef max
#undef min

namespace {

constexpr const char* SAVE_FILE = "/.system/sudoku.sav";
/** Solved counts per level (LEVELS order), then 1 if the game on the board is already counted. */
constexpr const char* TALLY_FILE = "/.system/sudoku-solved.txt";
constexpr int MARGIN = 12;
constexpr int GAP = 8;
/** A full refresh now and then clears the ghosting that fast refreshes leave. */
constexpr int MOVES_PER_FULL_REFRESH = 40;

struct Level {
  const char* name;
  const char* params;  ///< Solo's parameter string: 3x3 blocks, `d` + difficulty letter
  char letter;
};
constexpr Level LEVELS[] = {
    {"Beginner", "3x3dt", 't'}, {"Easy", "3x3db", 'b'},   {"Medium", "3x3di", 'i'},
    {"Hard", "3x3da", 'a'},     {"Expert", "3x3de", 'e'},
};
constexpr int LEVEL_COUNT = static_cast<int>(sizeof(LEVELS) / sizeof(LEVELS[0]));
constexpr int DEFAULT_LEVEL = 1;  // Easy
static_assert(LEVEL_COUNT == 5, "solvedCount in SudokuActivity.h has one slot per level");

int buttonHeight(const GfxRenderer& renderer) { return std::max(52, renderer.getScreenWidth() / 9); }

/** The two status lines above the board: level and tally (or Solved!), then the Notes hint. */
int statusHeight(const GfxRenderer& renderer) { return 2 * renderer.text.getLineHeight(systemFontId()) + 8; }

/**
 * Sends the frame out, then copies it back into the buffer drawn next. The display swaps between two
 * framebuffers on every refresh, and this screen draws only what changed (a cell, the status line)
 * over the last frame, so without the copy it drew over the frame before last: the first tap
 * showed the Games menu around the selected cell.
 */
void pushFrame(GfxRenderer& renderer, const bool clearGhosting) {
  renderer.displayBuffer(clearGhosting ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  renderer.syncWriteBufferFromActive();
}

}  // namespace

void SudokuActivity::onEnter() {
  Activity::onEnter();
  view = View::Board;
  notes = false;
  hasLastTap = false;
  movesSinceFullRefresh = 0;
  puzzle = std::make_unique<PuzzleFrontend>(renderer, solo);
  loadTally();
  if (!puzzle->load(SAVE_FILE)) {
    startNewGame(DEFAULT_LEVEL);
    return;
  }
  render(true);
}

void SudokuActivity::onExit() {
  if (puzzle) puzzle->save(SAVE_FILE);
  puzzle.reset();
  Activity::onExit();
}

int SudokuActivity::levelIndex() const {
  const std::string p = puzzle ? puzzle->params() : "";
  const size_t d = p.rfind('d');
  if (d != std::string::npos && d + 1 < p.size()) {
    for (int i = 0; i < LEVEL_COUNT; ++i) {
      if (LEVELS[i].letter == p[d + 1]) return i;
    }
  }
  return -1;
}

std::string SudokuActivity::levelName() const {
  const int level = levelIndex();
  return level >= 0 ? LEVELS[level].name : "Sudoku";
}

void SudokuActivity::loadTally() {
  solvedCount.fill(0);
  solveCounted = false;
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(TALLY_FILE, O_RDONLY);
  if (!file) return;
  char text[96] = {};
  const int n = file.read(text, sizeof(text) - 1);
  file.close();
  if (n <= 0) return;
  int counted = 0;
  std::sscanf(text, "%d %d %d %d %d %d", &solvedCount[0], &solvedCount[1], &solvedCount[2], &solvedCount[3],
              &solvedCount[4], &counted);
  for (int& count : solvedCount) count = std::max(0, count);
  solveCounted = counted != 0;
}

void SudokuActivity::saveTally() const {
  char text[96];
  const int n = std::snprintf(text, sizeof(text), "%d %d %d %d %d %d\n", solvedCount[0], solvedCount[1], solvedCount[2],
                              solvedCount[3], solvedCount[4], solveCounted ? 1 : 0);
  SdIoMutex::Lock lock;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(TALLY_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text, static_cast<size_t>(n));
  file.close();
}

void SudokuActivity::countSolve() {
  if (solveCounted || puzzle->status() <= 0) return;
  const int level = levelIndex();
  if (level >= 0) ++solvedCount[level];
  solveCounted = true;
  saveTally();
}

void SudokuActivity::button(const int x, const int y, const int w, const int h, const char* label, const Action action,
                            const int value, const bool filled) {
  if (filled) {
    renderer.rectangle.fill(x, y, w, h, true, true);
  } else {
    renderer.rectangle.render(x, y, w, h, true, true);
  }
  const int font = systemFontId();
  const std::string text = renderer.text.truncate(font, label, w - 8);
  const int textW = renderer.text.getWidth(font, text.c_str());
  renderer.text.render(font, x + (w - textW) / 2, y + (h - renderer.text.getLineHeight(font)) / 2, text.c_str(),
                       !filled);
  targets.push_back({x, y, w, h, action, value});
}

void SudokuActivity::renderStatus() {
  const int font = systemFontId();
  const int lineHeight = renderer.text.getLineHeight(font);
  const int maxWidth = renderer.getScreenWidth() - 2 * MARGIN;
  renderer.rectangle.fill(0, statusY, renderer.getScreenWidth(), statusHeight(renderer), false);
  std::string text;
  const int level = levelIndex();
  if (puzzle->status() > 0) {
    text = level >= 0 ? "Solved! " + std::to_string(solvedCount[level]) + " " + LEVELS[level].name +
                            " solved. Tap New for another."
                      : "Solved! Tap New for another puzzle.";
  } else {
    text = levelName() + (level >= 0 ? "  -  " + std::to_string(solvedCount[level]) + " solved" : "");
  }
  renderer.text.render(font, MARGIN, statusY + 4, renderer.text.truncate(font, text.c_str(), maxWidth).c_str(), true,
                       puzzle->status() > 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  if (notes && puzzle->status() <= 0) {
    renderer.text.render(font, MARGIN, statusY + 4 + lineHeight,
                         renderer.text.truncate(font, "Notes on: digits go in as pencil marks", maxWidth).c_str());
  }
}

void SudokuActivity::renderPad() {
  const int width = renderer.getScreenWidth();
  const int h = buttonHeight(renderer);
  renderer.rectangle.fill(0, padY, width, renderer.getScreenHeight() - padY, false);
  targets.erase(std::remove_if(targets.begin(), targets.end(),
                               [](const Target& t) {
                                 return t.action != Action::Level && t.action != Action::Restart &&
                                        t.action != Action::Cancel;
                               }),
                targets.end());
  const int inner = width - 2 * MARGIN;
  const int digitW = (inner - 8 * GAP) / 9;
  for (int d = 1; d <= 9; ++d) {
    const char label[2] = {static_cast<char>('0' + d), '\0'};
    button(MARGIN + (d - 1) * (digitW + GAP), padY, digitW, h, label, Action::Digit, d);
  }
  const int y = padY + h + GAP;
  const int toolW = (inner - 4 * GAP) / 5;
  const struct {
    const char* label;
    Action action;
  } tools[] = {{"Notes", Action::Notes},
               {"Erase", Action::Erase},
               {"Undo", Action::Undo},
               {"Redo", Action::Redo},
               {"New", Action::New}};
  for (int i = 0; i < 5; ++i) {
    button(MARGIN + i * (toolW + GAP), y, toolW, h, tools[i].label, tools[i].action, 0,
           tools[i].action == Action::Notes && notes);
  }
}

void SudokuActivity::renderLevels() {
  const int width = renderer.getScreenWidth();
  const int font = systemFontId();
  int y = statusY;
  renderer.text.render(font, MARGIN, y + 4, "New puzzle: choose a difficulty", true, EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(font) + 8 + GAP;
  // Five levels, Clear board and Cancel (with an extra gap before those two): shrink to fit if needed.
  const int rows = LEVEL_COUNT + 2;
  const int room = renderer.getScreenHeight() - MARGIN - y - (rows + 1) * GAP;
  const int h = std::max(36, std::min(buttonHeight(renderer), room / rows));
  for (int i = 0; i < LEVEL_COUNT; ++i) {
    const std::string label = std::string(LEVELS[i].name) + "  -  " + std::to_string(solvedCount[i]) + " solved";
    button(MARGIN, y, width - 2 * MARGIN, h, label.c_str(), Action::Level, i);
    y += h + GAP;
  }
  y += GAP;
  button(MARGIN, y, width - 2 * MARGIN, h, "Clear board (start this puzzle over)", Action::Restart);
  y += h + GAP;
  button(MARGIN, y, width - 2 * MARGIN, h, "Cancel", Action::Cancel);
}

void SudokuActivity::render(const bool fullRefresh) {
  targets.clear();
  renderer.clearScreen();
  statusY = SubPage::header(renderer, "Sudoku");
  const int h = buttonHeight(renderer);
  padY = renderer.getScreenHeight() - MARGIN - 2 * h - GAP;
  if (view == View::Levels) {
    renderLevels();
  } else {
    const int boardTop = statusY + statusHeight(renderer) + GAP;
    puzzle->layout(MARGIN, boardTop, renderer.getScreenWidth() - 2 * MARGIN, padY - GAP - boardTop);
    puzzle->drawAll();
    renderStatus();
    renderPad();
  }
  GfxRenderer::DisplayRegion ignored;
  puzzle->takeDirty(ignored);  // all of it goes out with this refresh
  pushFrame(renderer, fullRefresh);
  if (fullRefresh) movesSinceFullRefresh = 0;
}

void SudokuActivity::startNewGame(const int level) {
  view = View::Board;
  notes = false;
  hasLastTap = false;
  puzzle->setParams(LEVELS[level].params);
  // Generating takes up to a second or two on the S3: say so first.
  targets.clear();
  renderer.clearScreen();
  statusY = SubPage::header(renderer, "Sudoku");
  const std::string text = std::string("Making a new ") + LEVELS[level].name + " puzzle...";
  renderer.text.render(systemFontId(), MARGIN, statusY + 4, text.c_str());
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  const uint32_t started = millis();
  puzzle->newGame();
  solveCounted = false;
  saveTally();
  INX_SERIAL.printf("[SUDOKU] %s puzzle in %lu ms\n", LEVELS[level].name,
                    static_cast<unsigned long>(millis() - started));
  {
    SdIoMutex::Lock lock;
    SdMan.mkdir("/.system");
  }
  puzzle->save(SAVE_FILE);
  render(true);
}

void SudokuActivity::pushChanges() {
  GfxRenderer::DisplayRegion changed;
  if (!puzzle->takeDirty(changed)) return;
  const bool clearGhosting = ++movesSinceFullRefresh >= MOVES_PER_FULL_REFRESH;
  if (clearGhosting) movesSinceFullRefresh = 0;
  pushFrame(renderer, clearGhosting);
}

void SudokuActivity::afterMove(const bool changed) {
  if (!changed) return;
  puzzle->service();  // a completion flash ends at once
  puzzle->save(SAVE_FILE);
  countSolve();
  renderStatus();
  // The status line sits outside the board: make sure this refresh goes out.
  puzzle->markUpdate(0, 0, 1, 1);
  pushChanges();
}

void SudokuActivity::run(const Action action, const int value) {
  switch (action) {
    case Action::Digit:
      afterMove(puzzle->key('0' + value));
      return;
    case Action::Erase:
      afterMove(puzzle->key('\b'));
      return;
    case Action::Undo:
      afterMove(puzzle->key(UI_UNDO));
      return;
    case Action::Redo:
      afterMove(puzzle->key(UI_REDO));
      return;
    case Action::Notes:
      notes = !notes;
      // The selected cell switches to the new mode (Solo: right click selects for pencil marks).
      if (hasLastTap) puzzle->tap(lastTapX, lastTapY, notes);
      renderStatus();
      renderPad();
      puzzle->markUpdate(0, 0, 1, 1);
      pushChanges();
      return;
    case Action::New:
      view = View::Levels;
      render(false);
      return;
    case Action::Level:
      startNewGame(value);
      return;
    case Action::Restart:
      // An undoable move: Undo brings the cleared entries back. A solved puzzle stays counted.
      view = View::Board;
      notes = false;
      hasLastTap = false;
      puzzle->restart();
      puzzle->service();
      puzzle->save(SAVE_FILE);
      render(false);
      return;
    case Action::Cancel:
      view = View::Board;
      render(false);
      return;
  }
}

void SudokuActivity::loop() {
  if (view == View::Levels && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    run(Action::Cancel, 0);
    return;
  }
  if (SubPage::closeInput(renderer, mappedInput, [this] {
        puzzle->save(SAVE_FILE);
        onClose();
      })) {
    return;
  }

  if (mappedInput.hasTouch()) {
    float nx = 0;
    float ny = 0;
    if (mappedInput.wasTouchTapInScreen(renderer, nx, ny)) {
      const int x = static_cast<int>(nx * renderer.getScreenWidth());
      const int y = static_cast<int>(ny * renderer.getScreenHeight());
      for (const Target& t : targets) {
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
          run(t.action, t.value);
          return;
        }
      }
      if (view == View::Board) {
        const GfxRenderer::DisplayRegion b = puzzle->board();
        if (x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height) {
          lastTapX = x;
          lastTapY = y;
          hasLastTap = true;
          puzzle->tap(x, y, notes);
          pushChanges();
        }
      }
      return;
    }
  }

  if (view == View::Board) {
    int cursor = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) cursor = CURSOR_UP;
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) cursor = CURSOR_DOWN;
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) cursor = CURSOR_LEFT;
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) cursor = CURSOR_RIGHT;
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) cursor = CURSOR_SELECT;
    if (cursor) {
      puzzle->key(cursor);
      pushChanges();
    }
    puzzle->service();
    pushChanges();
  }
}

#endif  // INX_HAS_PUZZLES
