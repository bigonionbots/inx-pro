/**
 * @file GamesMenuActivity.cpp
 * @brief Definitions for GamesMenuActivity.
 */

#include "GamesMenuActivity.h"

#if INX_HAS_GAMES || INX_HAS_PUZZLES

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "activity/page/SubPage.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#if INX_HAS_GAMES
#include "images/Gamepad.h"
#endif
#if INX_HAS_PUZZLES
#include "games/puzzles/PuzzleGames.h"
#include "images/Pattern.h"
#include "images/Sudoku.h"
#endif

#if INX_HAS_GAMES
extern void onGoToGameBoy();
#endif
#if INX_HAS_PUZZLES
extern void onGoToPuzzle(const PuzzleGame& game);
#endif

namespace {

struct GameItem {
  const char* label;
  const char* subtitle;
  const uint8_t* icon;
  void (*open)();
};

constexpr GameItem kGames[] = {
#if INX_HAS_GAMES
    {"Game Boy", "Play Game Boy and Game Boy Color games", Gamepad, onGoToGameBoy},
#endif
#if INX_HAS_PUZZLES
    {"Sudoku", "Number puzzles, from beginner to expert", SudokuIcon, [] { onGoToPuzzle(PuzzleGames::SUDOKU); }},
    {"Pattern", "Nonograms: shade the picture from the clues", PatternIcon, [] { onGoToPuzzle(PuzzleGames::PATTERN); }},
#endif
};
constexpr int kGameCount = static_cast<int>(sizeof(kGames) / sizeof(kGames[0]));

constexpr int kIconSize = 40;
constexpr int kSideMargin = 20;
constexpr int kIconX = 24;
constexpr int kTextX = 88;
constexpr int kSubtitleGap = 5;
constexpr int kRowHeight = 96;

}  // namespace

void GamesMenuActivity::onEnter() {
  Activity::onEnter();
  selected = std::min(selected, kGameCount - 1);
  render();
}

void GamesMenuActivity::render() {
  renderer.clearScreen();
  listTop = SubPage::header(renderer, "Games");
  rowHeight = kRowHeight;
  const int titleFont = systemFontId();
  const int subtitleFont = systemFontIdForPointSize(8);
  const int screenW = renderer.getScreenWidth();
  const int titleHeight = renderer.text.getLineHeight(titleFont);
  const int subtitleHeight = renderer.text.getLineHeight(subtitleFont);
  const int contentHeight = titleHeight + kSubtitleGap + subtitleHeight;
  for (int i = 0; i < kGameCount; ++i) {
    const GameItem& item = kGames[i];
    const int y = listTop + i * rowHeight;
    const int titleY = y + (rowHeight - contentHeight) / 2;
    renderer.bitmap.icon(item.icon, kIconX, y + (rowHeight - kIconSize) / 2, kIconSize, kIconSize);
    renderer.text.render(titleFont, kTextX, titleY, item.label);  // no selection mark, as in Apps
    const std::string subtitle =
        renderer.text.truncate(subtitleFont, item.subtitle, std::max(1, screenW - kTextX - kSideMargin));
    renderer.text.render(subtitleFont, kTextX, titleY + titleHeight + kSubtitleGap, subtitle.c_str());
    if (i + 1 < kGameCount) {
      renderer.line.render(kSideMargin, y + rowHeight - 1, screenW - kSideMargin, y + rowHeight - 1, true,
                           LineRender::Style::Dotted);
    }
  }
  mappedInput.mapLabels("\xC2\xAB Back", "Open", "Up", "Down");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void GamesMenuActivity::open(const int index) {
  if (index < 0 || index >= kGameCount) return;
  selected = index;
  kGames[index].open();  // switchTo(): deferred to the end of the main loop
}

void GamesMenuActivity::loop() {
  if (SubPage::closeInput(renderer, mappedInput, onClose)) return;

  if (mappedInput.hasTouch()) {
    float nx = 0.0f;
    float ny = 0.0f;
    if (mappedInput.wasTouchTapInScreen(renderer, nx, ny)) {
      const int y = static_cast<int>(ny * renderer.getScreenHeight());
      if (y >= listTop && y < listTop + kGameCount * rowHeight) open((y - listTop) / rowHeight);
      return;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selected = (selected + kGameCount - 1) % kGameCount;
    render();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selected = (selected + 1) % kGameCount;
    render();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    open(selected);
  }
}

#endif  // INX_HAS_GAMES || INX_HAS_PUZZLES
