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
#include <cstdio>

#include "activity/page/SubPage.h"
#include "activity/page/components/global/PopUp.h"
#include "games/GameMode.h"
#include "games/GameOverlay.h"
#include "games/GameSound.h"
#include "images/Star.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/UiLayout.h"
#include "util/SdIoMutex.h"

namespace {

constexpr int MARGIN = 20;
constexpr int GAP = 12;
constexpr int BUTTON_HEIGHT = 76;
constexpr int ROW_HEIGHT = UiLayout::LIST_ITEM_HEIGHT;
constexpr int FOOTER_HEIGHT = 64;
constexpr int STAR_SIZE = 24;
constexpr unsigned long LONG_PRESS_MS = 500;  // as in the Library
constexpr const char* FAVORITES_FILE = "/Games/.favorites";

std::string extensionOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

bool isRom(const std::string& name) {
  const std::string ext = extensionOf(name);
  return ext == "gb" || ext == "gbc";
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
  loadFavorites();
  sortRoms();
  GameOverlay::ensureTemplate();
  overlay = GameOverlay::selected();
  sound = GameSound::enabled();
  pickingOverlay = false;
  page = 0;
  closeMenu();
}

int GamesActivity::itemCount() const { return static_cast<int>(pickingOverlay ? overlays.size() : roms.size()); }

void GamesActivity::openOverlayPicker() {
  overlays = GameOverlay::list();
  overlays.insert(overlays.begin(), "");
  gamesPage = page;
  page = 0;
  pickingOverlay = true;
  updateRequired = true;
}

void GamesActivity::closeOverlayPicker() {
  pickingOverlay = false;
  overlays.clear();
  page = gamesPage;
  updateRequired = true;
}

void GamesActivity::chooseOverlay(const size_t index) {
  overlay = overlays[index];
  GameOverlay::select(overlay);
  closeOverlayPicker();
}

void GamesActivity::sortRoms() {
  std::sort(roms.begin(), roms.end(), [this](const std::string& a, const std::string& b) {
    const bool fa = favorites.count(a) != 0;
    const bool fb = favorites.count(b) != 0;
    return fa != fb ? fa : a < b;
  });
}

void GamesActivity::loadFavorites() {
  favorites.clear();
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(FAVORITES_FILE, O_RDONLY);
  if (!file) return;
  std::string line;
  for (int c = file.read(); c >= 0; c = file.read()) {
    if (c == '\n' || c == '\r') {
      if (!line.empty()) favorites.insert(line);
      line.clear();
    } else {
      line += static_cast<char>(c);
    }
  }
  if (!line.empty()) favorites.insert(line);
  file.close();
}

void GamesActivity::saveFavorites() const {
  SdIoMutex::Lock lock;
  if (favorites.empty()) {
    SdMan.remove(FAVORITES_FILE);
    return;
  }
  FsFile file = SdMan.open(FAVORITES_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  for (const std::string& name : favorites) {
    file.write(name.data(), name.size());
    file.write('\n');
  }
  file.close();
}

std::vector<GamesActivity::Action> GamesActivity::menuActions() const {
  std::vector<Action> actions;
  if (menuIndex >= 0) actions.push_back(Action::Open);
  actions.push_back(Action::Delete);
  actions.push_back(Action::Favorite);
  return actions;
}

std::string GamesActivity::actionLabel(const Action action) const {
  switch (action) {
    case Action::Open:
      return "Open";
    case Action::Delete:
      return "Delete";
    case Action::Favorite:
    default:
      return favorites.count(roms[static_cast<size_t>(menuIndex)]) ? "Remove from favorites" : "Add to favorites";
  }
}

std::string GamesActivity::confirmTitle() const {
  const std::string& name = roms[static_cast<size_t>(menuIndex)];
  switch (pendingAction) {
    case Action::Open:
      return "Open " + name + "?";
    case Action::Delete:
      return "Delete " + name + "?";
    case Action::Favorite:
    default:
      return favorites.count(name) ? "Remove " + name + " from favorites?" : "Add " + name + " to favorites?";
  }
}

void GamesActivity::closeMenu() {
  menuIndex = -1;
  confirming = false;
  updateRequired = true;
}

void GamesActivity::openRom(const size_t index) const {
  const std::string path = std::string(GameMode::ROMS_DIRECTORY) + "/" + roms[index];
  GameMode::launchGame(path.c_str());
}

void GamesActivity::perform() {
  const size_t index = static_cast<size_t>(menuIndex);
  const std::string name = roms[index];
  switch (pendingAction) {
    case Action::Open:
      openRom(index);  // restarts into game mode
      break;
    case Action::Delete: {
      // Only the ROM: its .sav stays, so putting the ROM back restores the progress.
      const std::string path = std::string(GameMode::ROMS_DIRECTORY) + "/" + name;
      bool removed = false;
      {
        SdIoMutex::Lock lock;
        removed = SdMan.remove(path.c_str());
      }
      if (removed) {
        roms.erase(roms.begin() + static_cast<std::ptrdiff_t>(index));
        if (favorites.erase(name)) saveFavorites();
      }
      break;
    }
    case Action::Favorite:
      if (!favorites.erase(name)) favorites.insert(name);
      saveFavorites();
      sortRoms();
      break;
  }
  closeMenu();
}

void GamesActivity::menuInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    closeMenu();
    return;
  }
  if (!mappedInput.hasTouch()) return;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!mappedInput.wasTouchTapInScreen(renderer, nx, ny)) return;
  const int x = static_cast<int>(nx * renderer.getScreenWidth());
  const int y = static_cast<int>(ny * renderer.getScreenHeight());
  const std::vector<Action> actions = menuActions();
  const PopUpBounds box = PopUp::bounds(renderer, confirming ? 1 : static_cast<int>(actions.size()));
  if (x < box.x || x >= box.x + box.width || y < box.y || y >= box.y + box.height) {
    closeMenu();  // a tap outside cancels
    return;
  }
  const int row = (y - box.y - box.header) / box.row;
  if (y < box.y + box.header || row < 0) return;
  if (confirming) {
    if (row != 0) return;
    if (x < box.x + box.width / 2) {
      perform();
    } else {
      closeMenu();
    }
    return;
  }
  if (row >= static_cast<int>(actions.size())) return;
  pendingAction = actions[static_cast<size_t>(row)];
  confirming = true;
  updateRequired = true;
}

int GamesActivity::pageCount() const {
  const int total = itemCount();
  return std::max(1, (total + rowsPerPage - 1) / rowsPerPage);
}

void GamesActivity::turnPage(const int delta) {
  const int next = std::clamp(page + delta, 0, pageCount() - 1);
  if (next == page) return;
  page = next;
  updateRequired = true;
}

void GamesActivity::loop() {
  if (menuIndex >= 0) {
    menuInput();
    if (updateRequired) {
      updateRequired = false;
      render();
    }
    return;
  }
  if (pickingOverlay && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    closeOverlayPicker();
  } else if (SubPage::closeInput(renderer, mappedInput, onClose)) {
    return;
  }
  if (mappedInput.hasTouch()) {
    float nx = 0.0f;
    float ny = 0.0f;
    if (mappedInput.wasTouchTapInScreen(renderer, nx, ny)) {
      const int y = static_cast<int>(ny * renderer.getScreenHeight());
      const int row = (y - listY) / ROW_HEIGHT;
      const size_t index = static_cast<size_t>(page * rowsPerPage + row);
      if (y >= topButtonY && y < topButtonY + BUTTON_HEIGHT) {
        if (pickingOverlay) {
          closeOverlayPicker();
        } else if (nx >= 0.5f) {
          sound = !sound;
          GameSound::setEnabled(sound);
          updateRequired = true;
        } else {
          openOverlayPicker();
        }
      } else if (pageCount() > 1 && y >= footerY) {
        turnPage(nx < 0.5f ? -1 : 1);
      } else if (pickingOverlay) {
        if (y >= listY && row < rowsPerPage && index < overlays.size()) chooseOverlay(index);
      } else if (y >= listY) {
        if (row < rowsPerPage && index < roms.size()) {
          if (mappedInput.lastTouchHeldMs() >= LONG_PRESS_MS) {
            menuIndex = static_cast<int>(index);
            confirming = false;
            updateRequired = true;
          } else {
            openRom(index);
          }
        }
      }
    } else if (mappedInput.wasTouchSwipeUpForRenderer(renderer)) {
      turnPage(1);
    } else if (mappedInput.wasTouchSwipeDownForRenderer(renderer)) {
      turnPage(-1);
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) turnPage(1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) turnPage(-1);
  if (updateRequired) {
    updateRequired = false;
    render();
  }
}

void GamesActivity::render() {
  renderer.clearScreen();
  int y = SubPage::header(renderer, "Games") + 16;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;

  topButtonY = y;
  // Full width "Cancel" while choosing an overlay, else Overlay (left half) and Sound (right half).
  const auto button = [&](const int x, const int w, const std::string& label) {
    renderer.rectangle.fill(x, y, w, BUTTON_HEIGHT, true, true);
    const std::string shown = renderer.text.truncate(font, label.c_str(), w - 2 * GAP);
    renderer.text.render(font, x + (w - renderer.text.getWidth(font, shown.c_str())) / 2,
                         y + (BUTTON_HEIGHT - renderer.text.getLineHeight(font)) / 2, shown.c_str(), false);
  };
  if (pickingOverlay) {
    button(MARGIN, inner, "Cancel");
  } else {
    const int half = (inner - GAP) / 2;
    button(MARGIN, half, "Overlay: " + (overlay.empty() ? std::string("None") : overlay));
    button(MARGIN + half + GAP, inner - half - GAP, sound ? "Sound: On" : "Sound: Off");
  }
  y += BUTTON_HEIGHT + GAP;
  renderer.text.render(smallFont, MARGIN, y,
                       pickingOverlay ? "960 x 540 PNGs in /Games/Overlays. Start from Template.png."
                                      : "Tap a game to play it, hold it for more. EXIT or power returns.");
  y += renderer.text.getLineHeight(smallFont) + 2 * GAP;

  renderer.text.render(font, MARGIN, y, pickingOverlay ? "Choose an overlay:" : "Games on the SD card (/Games):", true,
                       EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(font) + GAP;
  listY = y;
  if (itemCount() == 0) {
    renderer.text.render(smallFont, MARGIN, y, "None yet. Copy .gb or .gbc files to /Games.");
    footerY = height;
    menuIndex = -1;
    renderer.displayBuffer();
    return;
  }

  // Rows that fit with the footer; drop the footer again if everything fits without it.
  rowsPerPage = std::max(1, (height - GAP - y) / ROW_HEIGHT);
  if (itemCount() > rowsPerPage) {
    rowsPerPage = std::max(1, (height - FOOTER_HEIGHT - GAP - y) / ROW_HEIGHT);
  }
  page = std::clamp(page, 0, pageCount() - 1);
  footerY = pageCount() > 1 ? height - FOOTER_HEIGHT : height;

  const size_t first = static_cast<size_t>(page * rowsPerPage);
  const size_t last = std::min(static_cast<size_t>(itemCount()), first + static_cast<size_t>(rowsPerPage));
  for (size_t i = first; i < last; ++i) {
    int textX = MARGIN;
    std::string rowLabel;
    if (pickingOverlay) {
      const std::string& name = overlays[i];
      rowLabel = name.empty() ? "None (the plain screen)" : name;
      if (name == overlay) rowLabel += "  (in use)";
    } else {
      const std::string& rom = roms[i];
      rowLabel = rom;
      if (favorites.count(rom)) {
        renderer.bitmap.icon(Star, MARGIN, y + (ROW_HEIGHT - STAR_SIZE) / 2, STAR_SIZE, STAR_SIZE);
        textX += STAR_SIZE + 10;
      }
    }
    renderer.text.render(font, textX, y + (ROW_HEIGHT - renderer.text.getLineHeight(font)) / 2,
                         renderer.text.truncate(font, rowLabel.c_str(), width - MARGIN - textX).c_str());
    if (i + 1 < last)
      renderer.line.render(MARGIN, y + ROW_HEIGHT - 1, width - MARGIN, y + ROW_HEIGHT - 1, true,
                           LineRender::Style::Dotted);
    y += ROW_HEIGHT;
  }

  if (pageCount() > 1) {
    renderer.line.render(0, footerY, width, footerY, true);
    const int textY = footerY + (FOOTER_HEIGHT - renderer.text.getLineHeight(font)) / 2;
    const bool hasPrev = page > 0;
    const bool hasNext = page + 1 < pageCount();
    if (hasPrev) renderer.text.render(font, MARGIN, textY, "\xC2\xAB Prev");
    if (hasNext) {
      const char* next = "Next \xC2\xBB";
      renderer.text.render(font, width - MARGIN - renderer.text.getWidth(font, next), textY, next);
    }
    char where[32];
    std::snprintf(where, sizeof(where), "%d of %d", page + 1, pageCount());
    renderer.text.centered(font, textY, where);
  }
  if (menuIndex >= 0) renderMenu();
  renderer.displayBuffer();
}

void GamesActivity::renderMenu() {
  if (confirming) {
    const PopUpBounds box = PopUp::bounds(renderer, 1);
    PopUp::background(renderer, box);
    PopUp::title(renderer, box, confirmTitle());
    const int actionY = box.y + box.header;
    const int half = box.width / 2;
    const int font = systemFontId();
    const int textY = actionY + (box.row - renderer.text.getLineHeight(font)) / 2;
    renderer.text.render(font, box.x + (half - renderer.text.getWidth(font, "Yes")) / 2, textY, "Yes", true);
    renderer.text.render(font, box.x + half + (box.width - half - renderer.text.getWidth(font, "No")) / 2, textY, "No",
                         true);
    renderer.line.render(box.x + half, actionY, box.x + half, actionY + box.row, true, LineRender::Style::Dotted);
    PopUp::border(renderer, box);
    return;
  }
  const std::vector<Action> actions = menuActions();
  std::vector<std::string> labels(actions.size());
  std::transform(actions.begin(), actions.end(), labels.begin(), [this](const Action a) { return actionLabel(a); });
  const PopUpBounds box = PopUp::bounds(renderer, static_cast<int>(labels.size()));
  PopUp::background(renderer, box);
  PopUp::title(renderer, box, roms[static_cast<size_t>(menuIndex)]);
  PopUp::list(renderer, box, labels, -1, 0);
  PopUp::border(renderer, box);
}

#endif  // INX_HAS_GAMES
