/**
 * @file ListeningStatsActivity.cpp
 * @brief Audiobook listening stats: this device's (ListeningLog) and the Audiobookshelf server's.
 */

#include "ListeningStatsActivity.h"

#if INX_HAS_AUDIO_MODULE

#include <GfxRenderer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "abs/AbsClient.h"
#include "abs/AbsWifi.h"
#include "abs/AbsWorker.h"
#include "activity/page/SubPage.h"
#include "system/Fonts.h"
#include "system/ListeningLog.h"
#include "system/MappedInputManager.h"
#include "system/ScreenComponents.h"

namespace {

constexpr int MARGIN = 20;
constexpr int GAP = 12;
constexpr int TAB_HEIGHT = 64;
constexpr int CHART_DAYS = 14;
constexpr int CHART_HEIGHT = 140;
constexpr int ROW_HEIGHT = 64;
constexpr size_t BOOKS_SHOWN = 5;
constexpr size_t SESSIONS_SHOWN = 5;

std::string duration(const uint64_t seconds) {
  const uint64_t minutes = seconds / 60;
  char text[24];
  if (minutes < 60) {
    snprintf(text, sizeof(text), "%lum", static_cast<unsigned long>(minutes));
  } else if (minutes < 100 * 60) {
    snprintf(text, sizeof(text), "%luh %lum", static_cast<unsigned long>(minutes / 60),
             static_cast<unsigned long>(minutes % 60));
  } else {
    snprintf(text, sizeof(text), "%luh", static_cast<unsigned long>(minutes / 60));
  }
  return text;
}

int64_t dayNumber(const uint32_t dateKey) {
  int year = static_cast<int>(dateKey / 10000);
  const unsigned month = dateKey / 100 % 100;
  const unsigned day = dateKey % 100;
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

uint32_t dateKeyOf(const std::string& isoDate) {  // "YYYY-MM-DD"
  if (isoDate.size() < 10) return 0;
  return static_cast<uint32_t>(std::atoi(isoDate.substr(0, 4).c_str()) * 10000 +
                               std::atoi(isoDate.substr(5, 2).c_str()) * 100 + std::atoi(isoDate.substr(8, 2).c_str()));
}

/** Today for the chart: the clock's date, else the last day with listening. */
uint32_t chartToday(const ListeningStatsActivity::Figures& figures) {
  const uint32_t today = ListeningLog::todayKey();
  if (today != 0) return today;
  return figures.days.empty() ? 0 : figures.days.back().first;
}

ListeningStatsActivity::Figures fromLocal(const ListeningLog::Stats& stats) {
  ListeningStatsActivity::Figures f;
  f.totalSeconds = stats.totalSeconds;
  const uint32_t today = ListeningLog::todayKey();
  for (const ListeningLog::DayTotal& d : stats.days) {
    f.days.emplace_back(d.dateKey, d.seconds);
    if (d.dateKey == today) f.todaySeconds = d.seconds;
  }
  std::vector<ListeningLog::BookTotal> books = stats.books;
  std::sort(books.begin(), books.end(), [](const auto& a, const auto& b) { return a.seconds > b.seconds; });
  for (const ListeningLog::BookTotal& b : books) f.books.push_back({b.title, b.author, b.seconds});
  return f;
}

ListeningStatsActivity::Figures fromServer(const Abs::ListeningStats& stats) {
  ListeningStatsActivity::Figures f;
  f.totalSeconds = stats.totalSeconds;
  f.todaySeconds = stats.todaySeconds;
  for (const Abs::ListeningStats::Day& d : stats.days) {
    const uint32_t key = dateKeyOf(d.date);
    if (key != 0) f.days.emplace_back(key, d.seconds);
  }
  for (const Abs::ListeningStats::Book& b : stats.books) f.books.push_back({b.title, b.author, b.seconds});
  for (const Abs::ListeningStats::Session& s : stats.recent) {
    std::string detail = s.date;
    if (!s.device.empty()) detail += (detail.empty() ? "" : "  -  ") + s.device;
    f.sessions.push_back({s.title, detail, s.seconds});
  }
  return f;
}

}  // namespace

struct ListeningStatsActivity::ServerState {
  std::atomic<bool> busy{false};
  std::atomic<bool> ready{false};  // a result (figures or error) is waiting for the screen
  bool loaded = false;             // screen side: figures hold the server's stats
  SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  Figures figures;
  std::string error;
  ~ServerState() {
    if (lock) vSemaphoreDelete(lock);
  }
};

void ListeningStatsActivity::onEnter() {
  local = fromLocal(ListeningLog::stats());
  server = std::make_shared<ServerState>();
  serverView = false;
  updateRequired = true;
}

void ListeningStatsActivity::showServer() {
  serverView = true;
  updateRequired = true;
  if (server->busy || server->loaded) return;
  if (!Abs::signedIn()) {
    server->error = "Sign in to Audiobookshelf first (Audiobooks -> Audiobookshelf).";
    return;
  }
  server->busy = true;
  server->error.clear();
  std::shared_ptr<ServerState> state = server;  // the job keeps it alive if the screen closes
  const bool posted = AbsWorker::post([state] {
    Abs::ListeningStats stats;
    std::string error;
    {
      AbsWifi::Use wifi(error);
      if (wifi.ok()) Abs::listeningStats(stats, error);
    }
    xSemaphoreTake(state->lock, portMAX_DELAY);
    if (error.empty()) state->figures = fromServer(stats);
    state->error = error;
    xSemaphoreGive(state->lock);
    state->ready = true;
    state->busy = false;
  });
  if (!posted) {
    server->busy = false;
    server->error = "Out of memory";
  }
}

void ListeningStatsActivity::loop() {
  if (SubPage::closeInput(renderer, mappedInput, onClose)) return;
  if (mappedInput.hasTouch()) {
    float nx = 0.0f;
    float ny = 0.0f;
    if (mappedInput.wasTouchTapInScreen(renderer, nx, ny)) {
      const int x = static_cast<int>(nx * renderer.getScreenWidth());
      const int y = static_cast<int>(ny * renderer.getScreenHeight());
      for (const Target& t : targets) {
        if (x < t.x || x >= t.x + t.w || y < t.y || y >= t.y + t.h) continue;
        if (t.server) {
          showServer();
        } else if (serverView) {
          serverView = false;
          updateRequired = true;
        }
        break;
      }
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    showServer();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
             mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (serverView) updateRequired = true;
    serverView = false;
  }
  if (server->ready.exchange(false)) {
    xSemaphoreTake(server->lock, portMAX_DELAY);
    server->loaded = server->error.empty();
    xSemaphoreGive(server->lock);
    if (serverView) updateRequired = true;
  }
  if (updateRequired) {
    updateRequired = false;
    render();
  }
}

void ListeningStatsActivity::render() {
  ScreenComponents::forgetWifiShown();
  targets.clear();
  renderer.clearScreen();
  int y = SubPage::header(renderer, "Listening Stats") + 16;
  const int width = renderer.getScreenWidth();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();

  // The two sources, the shown one filled.
  const int half = (inner - GAP) / 2;
  const auto tab = [&](const int x, const int w, const char* label, const bool selected, const bool isServer) {
    if (selected) {
      renderer.rectangle.fill(x, y, w, TAB_HEIGHT, true, true);
    } else {
      renderer.rectangle.render(x, y, w, TAB_HEIGHT, true, true);
    }
    const int textW = renderer.text.getWidth(font, label);
    renderer.text.render(font, x + (w - textW) / 2, y + (TAB_HEIGHT - renderer.text.getLineHeight(font)) / 2, label,
                         !selected);
    targets.push_back({x, y, w, TAB_HEIGHT, isServer});
  };
  tab(MARGIN, half, "This device", !serverView, false);
  tab(MARGIN + half + GAP, inner - half - GAP, "Audiobookshelf", serverView, true);
  y += TAB_HEIGHT + 2 * GAP;

  if (!serverView) {
    renderFigures(y, local, false);
  } else if (server->loaded) {
    xSemaphoreTake(server->lock, portMAX_DELAY);
    const Figures figures = server->figures;
    xSemaphoreGive(server->lock);
    renderFigures(y, figures, true);
  } else {
    const std::string text = server->busy ? "Loading from Audiobookshelf..." : server->error;
    renderer.text.render(font, MARGIN, y, renderer.text.truncate(font, text.c_str(), inner).c_str());
  }
  renderer.displayBuffer();
}

void ListeningStatsActivity::renderFigures(int y, const Figures& figures, const bool withSessions) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int inner = width - 2 * MARGIN;
  const int font = systemFontId();
  constexpr int smallFont = MONTSERRAT_10_FONT_ID;
  const int lineHeight = renderer.text.getLineHeight(font);
  const int smallLine = renderer.text.getLineHeight(smallFont);

  // Today, the last 7 days and the total, side by side.
  const uint32_t today = chartToday(figures);
  const int64_t todayNumber = today ? dayNumber(today) : 0;
  uint64_t week = 0;
  for (const auto& d : figures.days) {
    if (today && todayNumber - dayNumber(d.first) < 7 && dayNumber(d.first) <= todayNumber) week += d.second;
  }
  const struct {
    const char* label;
    uint64_t seconds;
  } totals[] = {{"Today", figures.todaySeconds}, {"Last 7 days", week}, {"Total", figures.totalSeconds}};
  const int column = inner / 3;
  for (int i = 0; i < 3; ++i) {
    const int x = MARGIN + i * column;
    renderer.text.render(font, x, y, duration(totals[i].seconds).c_str(), true, EpdFontFamily::BOLD);
    renderer.text.render(smallFont, x, y + lineHeight + 2, totals[i].label);
  }
  y += lineHeight + smallLine + 3 * GAP;

  // A bar per day, the last two weeks; today at the right.
  if (today) {
    uint32_t perDay[CHART_DAYS] = {};
    uint32_t most = 1;
    for (const auto& d : figures.days) {
      const int64_t back = todayNumber - dayNumber(d.first);
      if (back < 0 || back >= CHART_DAYS) continue;
      perDay[CHART_DAYS - 1 - back] = d.second;
    }
    for (const uint32_t s : perDay) most = std::max(most, s);
    const int slot = inner / CHART_DAYS;
    const int barWidth = std::max(4, slot - 8);
    const int baseY = y + CHART_HEIGHT;
    for (int i = 0; i < CHART_DAYS; ++i) {
      const int x = MARGIN + i * slot + (slot - barWidth) / 2;
      const int h = static_cast<int>(static_cast<uint64_t>(CHART_HEIGHT) * perDay[i] / most);
      if (h > 0) renderer.rectangle.fill(x, baseY - h, barWidth, h, true);
      // Day of the month under every other bar (and today's).
      if (i % 2 == (CHART_DAYS - 1) % 2) {
        const int64_t number = todayNumber - (CHART_DAYS - 1 - i);
        // Back from a day number to its day of the month (the inverse of dayNumber()).
        const int64_t civil = number + 719468;
        const int64_t era = (civil >= 0 ? civil : civil - 146096) / 146097;
        const unsigned dayOfEra = static_cast<unsigned>(civil - era * 146097);
        const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
        const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
        const unsigned monthPart = (5 * dayOfYear + 2) / 153;
        const unsigned dayOfMonth = dayOfYear - (153 * monthPart + 2) / 5 + 1;
        const std::string label = std::to_string(dayOfMonth);
        const int labelW = renderer.text.getWidth(smallFont, label.c_str());
        renderer.text.render(smallFont, MARGIN + i * slot + (slot - labelW) / 2, baseY + 6, label.c_str());
      }
    }
    renderer.line.render(MARGIN, baseY, MARGIN + inner, baseY, true);
    y = baseY + smallLine + 3 * GAP;
  } else {
    renderer.text.render(smallFont, MARGIN, y, "Set the clock (Wi-Fi time sync) to see listening by day.");
    y += smallLine + 2 * GAP;
  }

  const auto rows = [&](const char* label, const std::vector<Figures::Row>& list, const size_t shown) {
    if (list.empty() || y + 40 + ROW_HEIGHT > height) return;
    renderer.text.render(smallFont, MARGIN, y + 8, label, true, EpdFontFamily::BOLD);
    y += 40;
    for (size_t i = 0; i < std::min(shown, list.size()) && y + ROW_HEIGHT <= height; ++i) {
      const Figures::Row& row = list[i];
      const std::string time = duration(row.seconds);
      const int timeW = renderer.text.getWidth(font, time.c_str());
      const int textW = inner - timeW - GAP;
      renderer.text.render(font, MARGIN, y + 6, renderer.text.truncate(font, row.title.c_str(), textW).c_str(), true,
                           EpdFontFamily::BOLD);
      renderer.text.render(font, MARGIN + inner - timeW, y + 6, time.c_str());
      if (!row.detail.empty()) {
        renderer.text.render(smallFont, MARGIN, y + 10 + lineHeight,
                             renderer.text.truncate(smallFont, row.detail.c_str(), textW).c_str());
      }
      y += ROW_HEIGHT;
    }
    y += GAP;
  };
  if (figures.books.empty() && figures.sessions.empty()) {
    renderer.text.render(font, MARGIN, y, "No listening yet.");
    return;
  }
  rows("MOST LISTENED", figures.books, BOOKS_SHOWN);
  if (withSessions) rows("RECENT SESSIONS", figures.sessions, SESSIONS_SHOWN);
}

#endif  // INX_HAS_AUDIO_MODULE
