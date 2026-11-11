#pragma once

#include <cstddef>

class Activity;
class GfxRenderer;

class LibraryIndexRefresh {
 public:
  /**
   * Rebuilds the library index on a task behind an "Updating" popup (the main loop holds every screen
   * meanwhile). withMetadata: then rebuilds the metadata indexes already on the card (Authors, Series,
   * ...: MetadataIndex::existingIndexes()), for books added since (an OPDS download).
   */
  static void start(GfxRenderer& renderer, Activity* screen, bool withMetadata = false);
  static void render(GfxRenderer& renderer);
  static void finish(GfxRenderer& renderer, Activity* screen);
  static bool isRunning();
};
