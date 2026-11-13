#pragma once

/**
 * @file ListeningStatsActivity.h
 * @brief Audiobook listening stats: this device's (ListeningLog) and the Audiobookshelf server's.
 */

#if INX_HAS_AUDIO_MODULE

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activity/Activity.h"

/**
 * Two views, picked with the buttons under the header:
 * - This device: time counted on the T5 (every audiobook, local ones too).
 * - Audiobookshelf: the server's stats for the user (every app and device, the T5's uploaded
 *   sessions among them), fetched on the shared AbsWorker when first shown (Wi-Fi on demand).
 * Each shows today, the last 7 days and the total, a bar per day for the last 14 days, and the
 * most listened books; the server view adds its recent sessions.
 */
class ListeningStatsActivity final : public Activity {
 public:
  explicit ListeningStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onClose)
      : Activity("ListeningStats", renderer, mappedInput), onClose(onClose) {}

  void onEnter() override;
  void loop() override;

  /** The figures one view shows, from either source. Public for the source-specific helpers. */
  struct Figures {
    uint64_t totalSeconds = 0;
    uint32_t todaySeconds = 0;
    std::vector<std::pair<uint32_t, uint32_t>> days;  ///< (YYYYMMDD, seconds), oldest first
    struct Row {
      std::string title;
      std::string detail;  ///< author, or date and device for a session
      uint32_t seconds = 0;
    };
    std::vector<Row> books;     ///< most listened first
    std::vector<Row> sessions;  ///< newest first (server only)
  };

 private:
  /** The server fetch, shared with the worker job so it outlives a closed screen. */
  struct ServerState;

  const std::function<void()> onClose;
  bool serverView = false;
  bool updateRequired = false;
  Figures local;
  std::shared_ptr<ServerState> server;
  struct Target {
    int x, y, w, h;
    bool server;
  };
  std::vector<Target> targets;

  void showServer();
  void render();
  void renderFigures(int top, const Figures& figures, bool withSessions);
};

#endif  // INX_HAS_AUDIO_MODULE
