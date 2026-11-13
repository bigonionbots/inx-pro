#include "LibraryIndexRefresh.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "../activity/Activity.h"
#include "../state/SystemSetting.h"
#include "../system/ScreenComponents.h"
#include "LibraryIndex.h"
#include "MetadataIndex.h"

namespace {

volatile bool running = false;
bool popup = false;
Activity* screen = nullptr;
uint8_t* frame = nullptr;
size_t frameSize = 0;
ScreenComponents::LoadingProgressLayout layout{};
volatile int current = 0;
volatile int total = 0;
bool rebuildMetadata = false;  // for the task: this run also rebuilds the metadata indexes
int displayedProgress = -1;
constexpr uint32_t stackSize = 16384;

void clearFrame() {
  free(frame);
  frame = nullptr;
  frameSize = 0;
  screen = nullptr;
  popup = false;
  layout = {};
  current = 0;
  total = 0;
  displayedProgress = -1;
}

}  // namespace

void LibraryIndexRefresh::start(GfxRenderer& renderer, Activity* currentScreen, const char* message,
                                const bool withMetadata) {
  if (running || popup) return;
  rebuildMetadata = withMetadata;

  renderer.syncWriteBufferFromActive();
  frameSize = renderer.getBufferSize();
  frame = static_cast<uint8_t*>(malloc(frameSize));
  if (frame && renderer.getFrameBuffer()) {
    memcpy(frame, renderer.getFrameBuffer(), frameSize);
  }

  layout = ScreenComponents::LoadingProgress::show(renderer, message ? message : "Updating", 0);
  displayedProgress = 0;
  running = true;
  popup = true;
  screen = currentScreen;

  const BaseType_t created = xTaskCreate(
      [](void*) {
        LibraryIndex::indexAll([](int indexed, int count, const char*) {
          current = indexed;
          total = count;
          if (indexed % 10 == 0) vTaskDelay(pdMS_TO_TICKS(1));
        });
        SETTINGS.useLibraryIndex = 1;
        SETTINGS.saveToFile();
        if (rebuildMetadata) {
          const MetadataIndex::Options options = MetadataIndex::existingIndexes();
          if (MetadataIndex::any(options)) {
            current = 0;
            total = 0;
            MetadataIndex::generate(options, [](const int indexed, const int count, const char*, const char*) {
              current = indexed;
              total = count;
            });
          }
        }
        running = false;
        vTaskDelete(nullptr);
      },
      "GlobalLibraryIndex", stackSize, nullptr, 1, nullptr);

  if (created != pdPASS) running = false;
}

void LibraryIndexRefresh::render(GfxRenderer& renderer) {
  if (!popup || !running || total <= 0) return;

  const int progress = std::max(0, std::min(100, current * 100 / total));
  if (progress == displayedProgress) return;

  ScreenComponents::LoadingProgress::setProgress(renderer, layout, progress);
  displayedProgress = progress;
}

void LibraryIndexRefresh::finish(GfxRenderer& renderer, Activity* currentScreen) {
  if (!popup || running) return;

  if (currentScreen == screen) {
    if (frame && renderer.getFrameBuffer() && frameSize == renderer.getBufferSize()) {
      memcpy(renderer.getFrameBuffer(), frame, frameSize);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  clearFrame();
}

bool LibraryIndexRefresh::isRunning() { return running; }
