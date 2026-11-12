#include "LibraryIndexRefresh.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "../activity/Activity.h"
#include "../state/SystemSetting.h"
#include "../system/ScreenComponents.h"
#include "LibraryIndex.h"
#include "MetadataIndex.h"
#include "ThumbnailGeneration.h"

namespace {

volatile bool running = false;
bool popup = false;
Activity* screen = nullptr;
uint8_t* frame = nullptr;
size_t frameSize = 0;
ScreenComponents::LoadingProgressLayout layout{};
volatile int current = 0;
volatile int total = 0;
uint8_t extras = 0;  // for the task: LibraryIndexRefresh::Extra flags of this run
GfxRenderer* taskRenderer = nullptr;
// Thumbnail generation draws each new thumbnail into the frame buffer (to fill the display cache)
// while render() draws the progress bar from the main loop: both hold this.
SemaphoreHandle_t drawLock = nullptr;
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
                                const uint8_t extraWork) {
  if (running || popup) return;
  extras = extraWork;
  taskRenderer = &renderer;
  if (!drawLock) drawLock = xSemaphoreCreateMutex();

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
        const int books = total;
        if (extras & EXTRA_METADATA) {
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
        if ((extras & EXTRA_THUMBNAILS) && taskRenderer && drawLock) {
          // Books that already have one are skipped at once: this costs the new books.
          current = 0;
          total = books;
          ThumbnailGeneration::Result result;
          ThumbnailGeneration::generate(*taskRenderer, drawLock, result,
                                        [](const int processed, const char*) { current = processed; });
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

  // Thumbnail generation may be drawing into the frame buffer: wait for the next frame then.
  if (drawLock && xSemaphoreTake(drawLock, 0) != pdTRUE) return;
  ScreenComponents::LoadingProgress::setProgress(renderer, layout, progress);
  if (drawLock) xSemaphoreGive(drawLock);
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
