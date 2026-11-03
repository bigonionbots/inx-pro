/**
 * @file GamesActivity.cpp
 * @brief Definitions for GamesActivity.
 */

#include "GamesActivity.h"

#if INX_HAS_GAMES

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>

#include "activity/page/SubPage.h"
#include "games/GameMode.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/UiLayout.h"
#include "util/SdIoMutex.h"

namespace {

constexpr int MARGIN = 20;
constexpr int GAP = 12;
constexpr int BUTTON_HEIGHT = 76;
constexpr int ROW_HEIGHT = UiLayout::LIST_ITEM_HEIGHT;

bool isRom(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == "gb" || ext == "gbc" || ext == "nes";
}

}  // namespace

void GamesActivity::onEnter() {
  roms.clear();
  {
    SdIoMutex::Lock lock;
    FsFile dir = SdMan.open(GameMode::ROMS_DIRECTORY, O_RDONLY);
    if (dir && dir.isDirectory()) {
      char name[256];
      for (;;) {
        FsFile entry = dir.openNextFile();
        if (!entry) break;
        entry.getName(name, sizeof(name));
        const bool isFile = !entry.isDirectory();
        entry.close();
        if (isFile && name[0] != '.' && isRom(name)) roms.emplace_back(name);
      }
      dir.close();
    }
  }
  std::sort(roms.begin(), roms.end());
  updateRequired = true;
}

void GamesActivity::loop() {
  if (SubPage::closeInput(renderer, mappedInput, onClose)) return;
  if (mappedInput.hasTouch()) {
    float nx = 0.0f;
    float ny = 0.0f;
    if (mappedInput.wasTouchTapInScreen(renderer, nx, ny)) {
      const int y = static_cast<int>(ny * renderer.getScreenHeight());
      if (y >= testButtonY && y < testButtonY + BUTTON_HEIGHT) GameMode::launchDisplayTest();
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) GameMode::launchDisplayTest();
  if (updateRequired) {
    updateRequired = false;
    render();
  }
}

void GamesActivity::render() {
  renderer.clearScreen();
  int y = SubPage::header(renderer, "Games") + 16;
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  testButtonY = y;
  renderer.rectangle.fill(MARGIN, y, inner, BUTTON_HEIGHT, true, true);
  const char* label = "Display and touch test";
  renderer.text.render(font, MARGIN + (inner - renderer.text.getWidth(font, label)) / 2,
                       y + (BUTTON_HEIGHT - renderer.text.getLineHeight(font)) / 2, label, false);
  y += BUTTON_HEIGHT + GAP;
  renderer.text.render(smallFont, MARGIN, y, "Restarts into game mode. EXIT or the power button returns.");
  y += renderer.text.getLineHeight(smallFont) + 3 * GAP;

  renderer.text.render(font, MARGIN, y, "Games on the SD card (/Games):", true, EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(font) + GAP;
  if (roms.empty()) {
    renderer.text.render(smallFont, MARGIN, y, "None yet. Copy .gb, .gbc or .nes files to /Games.");
  }
  const int bottom = renderer.getScreenHeight() - GAP;
  for (const std::string& rom : roms) {
    if (y + ROW_HEIGHT > bottom) break;
    renderer.text.render(font, MARGIN, y + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2,
                         renderer.text.truncate(font, rom.c_str(), inner).c_str());
    y += ROW_HEIGHT;
  }
  y += GAP;
  if (!roms.empty() && y + renderer.text.getLineHeight(smallFont) < bottom) {
    renderer.text.render(smallFont, MARGIN, y, "Playing them comes with the next milestone.");
  }
  renderer.displayBuffer();
}

#endif  // INX_HAS_GAMES
