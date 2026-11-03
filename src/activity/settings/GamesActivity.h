#pragma once

/**
 * @file GamesActivity.h
 * @brief Public interface and types for GamesActivity.
 */

#if INX_HAS_GAMES

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "activity/Activity.h"

/**
 * Games screen (Home -> Shortcuts -> Games). Lists the ROMs under /Games and launches game mode
 * (games/GameMode.h), which restarts the device into a boot that drives the panel directly.
 * Game Boy / Color ROMs launch from the list; NES follows. The button at the top picks the
 * screen overlay (games/GameOverlay.h) from /Games/Overlays.
 *
 * A tap opens a game. A long press opens a menu (Open, Delete, Add to / Remove from favorites),
 * and each choice asks for confirmation. Favorites (kept in /Games/.favorites, one file name per
 * line) are starred and listed first.
 */
class GamesActivity final : public Activity {
 public:
  GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onClose)
      : Activity("Games", renderer, mappedInput), onClose(std::move(onClose)) {}

  void onEnter() override;
  void loop() override;

 private:
  const std::function<void()> onClose;
  std::vector<std::string> roms;
  bool updateRequired = false;
  int topButtonY = 0;   // Overlay button (Cancel while choosing one)
  int listY = 0;        // top of the first ROM row
  int rowsPerPage = 1;  // ROM rows that fit on one page
  int page = 0;
  int footerY = 0;  // Prev / page / Next row, shown when there is more than one page

  enum class Action : uint8_t { Open, Delete, Favorite };
  std::set<std::string> favorites;
  int menuIndex = -1;       // ROM whose long-press menu (or confirmation) is showing
  bool confirming = false;  // the Yes / No step for `pendingAction`
  Action pendingAction = Action::Open;

  bool pickingOverlay = false;
  std::vector<std::string> overlays;  // while picking: "" (none) then the PNGs
  std::string overlay;                // the chosen overlay, "" for none
  int gamesPage = 0;                  // the games list's page while picking

  int itemCount() const;
  void openOverlayPicker();
  void closeOverlayPicker();
  void chooseOverlay(size_t index);
  int pageCount() const;
  void turnPage(int delta);
  void sortRoms();
  void loadFavorites();
  void saveFavorites() const;
  std::vector<Action> menuActions() const;
  std::string actionLabel(Action action) const;
  std::string confirmTitle() const;
  void closeMenu();
  void openRom(size_t index) const;
  void perform();
  void menuInput();

  static bool isGameBoyRom(const std::string& name);

  void render();
  void renderMenu();
};

#endif  // INX_HAS_GAMES
