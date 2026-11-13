#pragma once

#include <cstddef>
#include <cstdint>

class Activity;
class GfxRenderer;

class LibraryIndexRefresh {
 public:
  /** Work after the library index, for books added since (an OPDS download). */
  enum Extra : uint8_t {
    EXTRA_NONE = 0,
    EXTRA_METADATA = 1 << 0,    ///< rebuild the metadata indexes already on the card (Authors, Series, ...)
    EXTRA_THUMBNAILS = 1 << 1,  ///< generate the thumbnails books are missing (the Library's cover views)
  };
  /**
   * Rebuilds the library index on a task behind a progress popup (the main loop holds every screen
   * meanwhile), then does the extras (Extra flags).
   */
  static void start(GfxRenderer& renderer, Activity* screen, const char* message = "Updating",
                    uint8_t extras = EXTRA_NONE);
  static void render(GfxRenderer& renderer);
  static void finish(GfxRenderer& renderer, Activity* screen);
  static bool isRunning();
};
