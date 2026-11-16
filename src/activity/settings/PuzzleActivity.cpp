/**
 * @file PuzzleActivity.cpp
 * @brief Definitions for PuzzleActivity.
 */

#include "PuzzleActivity.h"

#if INX_HAS_PUZZLES

#include <Arduino.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

constexpr int MARGIN = 12;
constexpr int GAP = 8;
/** A full refresh now and then clears the ghosting that fast refreshes leave. */
constexpr int MOVES_PER_FULL_REFRESH = 40;

int buttonHeight(const GfxRenderer& renderer) { return std::max(52, renderer.getScreenWidth() / 9); }

/** The two status lines above the board: level and tally (or Solved!), then the mode's hint. */
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

void PuzzleActivity::onEnter() {
  Activity::onEnter();
  view = View::Board;
  mode = false;
  hasLastTap = false;
  movesSinceFullRefresh = 0;
  puzzle = std::make_unique<PuzzleFrontend>(renderer, *spec.puzzle);
  loadTally();
  if (!puzzle->load(spec.saveFile)) {
    startNewGame(spec.defaultLevel);
    return;
  }
  render(true);
}

void PuzzleActivity::onExit() {
  if (puzzle) puzzle->save(spec.saveFile);
  puzzle.reset();
  Activity::onExit();
}

int PuzzleActivity::levelIndex() const {
  if (!puzzle) return -1;
  const std::string current = puzzle->params();
  for (int i = 0; i < spec.levelCount; ++i) {
    if (puzzle->canonicalParams(spec.levels[i].params) == current) return i;
  }
  return -1;
}

std::string PuzzleActivity::levelName() const {
  const int level = levelIndex();
  return level >= 0 ? spec.levels[level].name : spec.title;
}

int PuzzleActivity::digitCount() const {
  const int level = levelIndex();
  return spec.levels[level >= 0 ? level : spec.defaultLevel].digits;
}

void PuzzleActivity::loadTally() {
  solvedCount.fill(0);
  solveCounted = false;
  char text[128] = {};
  {
    SdIoMutex::Lock lock;
    FsFile file = SdMan.open(spec.tallyFile, O_RDONLY);
    if (!file) return;
    const int n = file.read(text, sizeof(text) - 1);
    file.close();
    if (n <= 0) return;
  }
  // One count per level, then the counted flag.
  const char* cursor = text;
  for (int i = 0; i <= spec.levelCount; ++i) {
    char* end = nullptr;
    const long value = std::strtol(cursor, &end, 10);
    if (end == cursor) break;
    cursor = end;
    if (i < spec.levelCount) {
      solvedCount[i] = static_cast<int>(std::max(0L, value));
    } else {
      solveCounted = value != 0;
    }
  }
}

void PuzzleActivity::saveTally() const {
  std::string text;
  for (int i = 0; i < spec.levelCount; ++i) text += std::to_string(solvedCount[i]) + " ";
  text += solveCounted ? "1\n" : "0\n";
  SdIoMutex::Lock lock;
  SdMan.mkdir("/.system");
  FsFile file = SdMan.open(spec.tallyFile, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(text.data(), text.size());
  file.close();
}

void PuzzleActivity::countSolve() {
  if (solveCounted || puzzle->status() <= 0) return;
  const int level = levelIndex();
  if (level >= 0) ++solvedCount[level];
  solveCounted = true;
  saveTally();
}

void PuzzleActivity::button(const int x, const int y, const int w, const int h, const char* label, const Action action,
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

void PuzzleActivity::renderStatus() {
  const int font = systemFontId();
  const int lineHeight = renderer.text.getLineHeight(font);
  const int maxWidth = renderer.getScreenWidth() - 2 * MARGIN;
  renderer.rectangle.fill(0, statusY, renderer.getScreenWidth(), statusHeight(renderer), false);
  std::string text;
  const int level = levelIndex();
  if (puzzle->status() > 0) {
    text = level >= 0 ? "Solved! " + std::to_string(solvedCount[level]) + " " + spec.levels[level].name +
                            " solved. Tap New for another."
                      : "Solved! Tap New for another puzzle.";
  } else {
    text = levelName() + (level >= 0 ? "  -  " + std::to_string(solvedCount[level]) + " solved" : "");
  }
  renderer.text.render(font, MARGIN, statusY + 4, renderer.text.truncate(font, text.c_str(), maxWidth).c_str(), true,
                       puzzle->status() > 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  if (mode && puzzle->status() <= 0) {
    renderer.text.render(font, MARGIN, statusY + 4 + lineHeight,
                         renderer.text.truncate(font, spec.modeHint, maxWidth).c_str());
  }
}

void PuzzleActivity::renderPad() {
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
  int y = padY;
  const int digits = digitCount();
  if (digits > 0) {
    // Sized for nine, so a small grid's few buttons are not stretched across the screen; centred.
    const int digitW = (inner - 8 * GAP) / 9;
    const int left = MARGIN + (inner - (digits * digitW + (digits - 1) * GAP)) / 2;
    for (int d = 1; d <= digits; ++d) {
      const char label[2] = {static_cast<char>('0' + d), '\0'};
      button(left + (d - 1) * (digitW + GAP), y, digitW, h, label, Action::Digit, d);
    }
    y += h + GAP;
  }
  struct Tool {
    const char* label;
    Action action;
  };
  Tool tools[5];
  int toolCount = 0;
  tools[toolCount++] = {spec.modeLabel, Action::Mode};
  if (spec.selectsCells) tools[toolCount++] = {"Erase", Action::Erase};
  tools[toolCount++] = {"Undo", Action::Undo};
  tools[toolCount++] = {"Redo", Action::Redo};
  tools[toolCount++] = {"New", Action::New};
  const int toolW = (inner - (toolCount - 1) * GAP) / toolCount;
  for (int i = 0; i < toolCount; ++i) {
    button(MARGIN + i * (toolW + GAP), y, toolW, h, tools[i].label, tools[i].action, 0,
           tools[i].action == Action::Mode && mode);
  }
}

void PuzzleActivity::renderLevels() {
  const int width = renderer.getScreenWidth();
  const int font = systemFontId();
  int y = statusY;
  renderer.text.render(font, MARGIN, y + 4, spec.levelPrompt, true, EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(font) + 8 + GAP;
  // The levels, Clear board and Cancel (with an extra gap before those two): shrink to fit if needed.
  const int rows = spec.levelCount + 2;
  const int room = renderer.getScreenHeight() - MARGIN - y - (rows + 1) * GAP;
  const int h = std::max(36, std::min(buttonHeight(renderer), room / rows));
  for (int i = 0; i < spec.levelCount; ++i) {
    const std::string label = std::string(spec.levels[i].name) + "  -  " + std::to_string(solvedCount[i]) + " solved";
    button(MARGIN, y, width - 2 * MARGIN, h, label.c_str(), Action::Level, i);
    y += h + GAP;
  }
  y += GAP;
  button(MARGIN, y, width - 2 * MARGIN, h, "Clear board (start this puzzle over)", Action::Restart);
  y += h + GAP;
  button(MARGIN, y, width - 2 * MARGIN, h, "Cancel", Action::Cancel);
}

void PuzzleActivity::render(const bool fullRefresh) {
  targets.clear();
  renderer.clearScreen();
  statusY = SubPage::header(renderer, spec.title);
  const int h = buttonHeight(renderer);
  padY = renderer.getScreenHeight() - MARGIN - padRows() * h - (padRows() - 1) * GAP;
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

void PuzzleActivity::startNewGame(const int level) {
  view = View::Board;
  mode = false;
  hasLastTap = false;
  puzzle->setParams(spec.levels[level].params);
  // Generating takes up to a second or two on the S3: say so first.
  targets.clear();
  renderer.clearScreen();
  statusY = SubPage::header(renderer, spec.title);
  const std::string text = std::string("Making a new ") + spec.levels[level].name + " puzzle...";
  renderer.text.render(systemFontId(), MARGIN, statusY + 4, text.c_str());
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  const uint32_t started = millis();
  puzzle->newGame();
  solveCounted = false;
  saveTally();
  INX_SERIAL.printf("[PUZZLE] %s %s in %lu ms\n", spec.title, spec.levels[level].name,
                    static_cast<unsigned long>(millis() - started));
  {
    SdIoMutex::Lock lock;
    SdMan.mkdir("/.system");
  }
  puzzle->save(spec.saveFile);
  render(true);
}

void PuzzleActivity::pushChanges() {
  GfxRenderer::DisplayRegion changed;
  if (!puzzle->takeDirty(changed)) return;
  const bool clearGhosting = ++movesSinceFullRefresh >= MOVES_PER_FULL_REFRESH;
  if (clearGhosting) movesSinceFullRefresh = 0;
  pushFrame(renderer, clearGhosting);
}

void PuzzleActivity::afterMove(const bool changed) {
  if (!changed) return;
  puzzle->service();  // a completion flash ends at once
  puzzle->save(spec.saveFile);
  countSolve();
  renderStatus();
  // The status line sits outside the board: make sure this refresh goes out.
  puzzle->markUpdate(0, 0, 1, 1);
  pushChanges();
}

void PuzzleActivity::run(const Action action, const int value) {
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
    case Action::Mode:
      mode = !mode;
      // A selected cell switches to the new mode (Solo: right click selects for pencil
      // marks). In a tap-to-set puzzle that tap would change the cell, so not there.
      if (spec.selectsCells && hasLastTap) puzzle->tap(lastTapX, lastTapY, mode);
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
      mode = false;
      hasLastTap = false;
      puzzle->restart();
      puzzle->service();
      puzzle->save(spec.saveFile);
      render(false);
      return;
    case Action::Cancel:
      view = View::Board;
      render(false);
      return;
  }
}

void PuzzleActivity::loop() {
  if (view == View::Levels && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    run(Action::Cancel, 0);
    return;
  }
  if (SubPage::closeInput(renderer, mappedInput, [this] {
        puzzle->save(spec.saveFile);
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
          if (spec.selectsCells) {
            puzzle->tap(x, y, mode);  // selects: nothing to save yet
            pushChanges();
          } else {
            afterMove(puzzle->tap(x, y, mode));
          }
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
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) cursor = mode ? CURSOR_SELECT2 : CURSOR_SELECT;
    if (cursor) {
      const bool used = puzzle->key(cursor);
      if (spec.selectsCells || (cursor != CURSOR_SELECT && cursor != CURSOR_SELECT2)) {
        pushChanges();
      } else {
        afterMove(used);
      }
    }
    puzzle->service();
    pushChanges();
  }
}

#endif  // INX_HAS_PUZZLES
