#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

#include "system/PluginManager.h"

class GfxRenderer;

/**
 * Home screen widget whose text comes from an installed plugin's `home_widget` hook.
 *
 * The plugin function receives the date, time, and slot size, and returns a table of strings:
 * `before`, `highlight` (drawn bold), `after`, and `footer` (a smaller grey line at the bottom).
 * The firmware lays the text out, so plugins never draw directly.
 */
class PluginWidget final {
 public:
  explicit PluginWidget(GfxRenderer& renderer) : renderer_(renderer) {}

  void render(int x, int y, int width, int height) const;
  bool needsRefresh() const;
  /** True when an installed plugin contributes a home widget. */
  bool available() const { return resolve(); }
  /** Label of the installed plugin's widget, for the widget picker. */
  const char* label() const;

  struct Run {
    std::string text;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };

  struct Line {
    std::vector<Run> runs;
  };

 private:
  bool resolve() const;
  bool refreshKey(uint64_t& key) const;
  void load(int width, int height) const;
  void renderMessage(int x, int y, int width, int height, const char* message) const;

  GfxRenderer& renderer_;
  mutable bool resolved_ = false;
  mutable bool available_ = false;
  mutable PluginManager::HomeWidgetLink link_;
  mutable bool loaded_ = false;
  mutable uint64_t loadedKey_ = 0;
  mutable int loadedWidth_ = -1;
  mutable int loadedHeight_ = -1;
  mutable std::string before_;
  mutable std::string highlight_;
  mutable std::string after_;
  mutable std::string footer_;
  mutable std::string error_;
};
