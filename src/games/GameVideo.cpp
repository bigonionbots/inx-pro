/**
 * @file GameVideo.cpp
 * @brief Definitions for the game-mode continuous panel scan.
 */

#include "GameVideo.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "GameDisplay.h"
#include "GameVideoRow.h"

namespace GameVideo {

namespace {

constexpr int ROW_BYTES = GameDisplay::WIDTH / 8;   // 1-bpp buffers
constexpr int CODE_BYTES = GameDisplay::WIDTH / 4;  // 2-bit drive codes
constexpr int PLANE_BYTES = ROW_BYTES * GameDisplay::HEIGHT;
constexpr int DIRTY_WORDS = (GameDisplay::HEIGHT + 31) / 32;

const GameVideoRow::CodeTables* tables = nullptr;
uint8_t* shown = nullptr;
uint8_t* count0 = nullptr;
uint8_t* count1 = nullptr;
uint8_t* count2 = nullptr;
bool settling[GameDisplay::HEIGHT];
std::atomic<uint32_t> dirty[DIRTY_WORDS];

int drivePasses = DEFAULT_PASSES;
TaskHandle_t task = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<bool> running{false};
std::atomic<uint32_t> scanCount{0};

uint8_t* allocPlane() {
  auto* p = static_cast<uint8_t*>(heap_caps_malloc(PLANE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!p) p = static_cast<uint8_t*>(heap_caps_malloc(PLANE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return p;
}

bool takeDirty(const int row) {
  const uint32_t bit = 1u << (row & 31);
  return (dirty[row >> 5].fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
}

/** One pass over every panel row. Rows with nothing to do get an all-zero (undriven) row. */
void IRAM_ATTR scanOnce(FASTEPD& epd, uint8_t* dma, const uint8_t* target) {
  int offset = 0;
  int neutralRun = 0;  // the DMA buffer alternates halves: both must be zeroed before reuse
  epd.rowControl(ROW_START);
  for (int row = 0; row < GameDisplay::HEIGHT; ++row) {
    uint8_t* codes = dma + offset;
    if (takeDirty(row) || settling[row]) {
      const size_t at = static_cast<size_t>(row) * ROW_BYTES;
      settling[row] = GameVideoRow::prepare(*tables, ROW_BYTES, target + at, shown + at, count0 + at, count1 + at,
                                            count2 + at, codes, drivePasses);
      neutralRun = 0;
    } else {
      if (neutralRun < 2) std::memset(codes, 0, CODE_BYTES);
      ++neutralRun;
    }
    epd.writeRow(codes, row != 0);
    offset ^= CODE_BYTES;
  }
  // The last row is latched by the next row step: send one undriven row after it.
  uint8_t* codes = dma + offset;
  std::memset(codes, 0, CODE_BYTES);
  epd.writeRow(codes, true);
}

void scanTask(void*) {
  FASTEPD& epd = GameDisplay::epd();
  uint8_t* dma = epd.dmaBuffer();
  const uint8_t* target = epd.currentBuffer();
  while (!stopRequested.load(std::memory_order_acquire)) {
    scanOnce(epd, dma, target);
    scanCount.fetch_add(1, std::memory_order_relaxed);
    vTaskDelay(1);  // lets core 0's idle task feed the task watchdog
  }
  running.store(false, std::memory_order_release);
  task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool start(const int passes) {
  if (running.load()) return true;
  if (!tables) tables = new GameVideoRow::CodeTables();
  if (!shown) shown = allocPlane();
  if (!count0) count0 = allocPlane();
  if (!count1) count1 = allocPlane();
  if (!count2) count2 = allocPlane();
  if (!shown || !count0 || !count1 || !count2) {
    INX_SERIAL.println("[GAME] video: out of memory");
    return false;
  }
  FASTEPD& epd = GameDisplay::epd();
  std::memcpy(shown, epd.currentBuffer(), PLANE_BYTES);
  std::memset(count0, 0, PLANE_BYTES);
  std::memset(count1, 0, PLANE_BYTES);
  std::memset(count2, 0, PLANE_BYTES);
  std::fill(std::begin(settling), std::end(settling), false);
  for (auto& word : dirty) word.store(0);
  drivePasses = std::clamp(passes, 1, GameVideoRow::MAX_PASSES);
  epd.einkPower(1);

  stopRequested.store(false);
  running.store(true);
  if (xTaskCreatePinnedToCore(scanTask, "game_video", 4096, nullptr, configMAX_PRIORITIES - 2, &task, 0) != pdPASS) {
    running.store(false);
    INX_SERIAL.println("[GAME] video: task did not start");
    return false;
  }
  return true;
}

void stop() {
  if (!running.load()) return;
  stopRequested.store(true, std::memory_order_release);
  while (running.load(std::memory_order_acquire)) vTaskDelay(1);
  // FastEPD's own updates diff against its previous buffer: make it what the panel shows.
  std::memcpy(GameDisplay::epd().previousBuffer(), shown, PLANE_BYTES);
}

void markRows(int top, int bottom) {
  top = std::max(top, 0);
  bottom = std::min(bottom, GameDisplay::HEIGHT - 1);
  for (int row = top; row <= bottom; ++row) {
    dirty[row >> 5].fetch_or(1u << (row & 31), std::memory_order_release);
  }
}

void clean() {
  const bool wasRunning = running.load();
  stop();
  GameDisplay::epd().fullUpdate(CLEAR_FAST, true);
  if (wasRunning) start(drivePasses);
}

uint32_t scans() { return scanCount.load(std::memory_order_relaxed); }

}  // namespace GameVideo

#endif  // INX_HAS_GAMES
