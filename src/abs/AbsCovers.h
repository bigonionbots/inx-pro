#pragma once

/**
 * @file AbsCovers.h
 * @brief Audiobookshelf cover art, cached on the SD card.
 *
 * A cover is fetched once, as a JPEG the server scales down (GET /api/items/:id/cover?width=240
 * &format=jpeg), and kept in /.system/cache/abs-covers/<item id>.jpg, so screens draw it from the
 * card at once and with Wi-Fi off. Screens draw what is cached and request() the rest.
 *
 * A long-lived task (stack in PSRAM) fetches requests over one kept-alive connection, so a
 * screenful costs one TLS handshake rather than one per cover, and only while Wi-Fi is already up
 * (it never connects by itself). It never touches the SD card: finished covers wait in PSRAM
 * until service(), on the main loop, writes them (SdFat is not thread-safe). main.cpp calls
 * service() every loop; screens watch changeCounter() to redraw when covers arrive.
 */

#if INX_HAS_AUDIO_MODULE

#include <cstdint>
#include <string>
#include <vector>

namespace AbsCovers {

/** Where an item's cover is (or will be) cached. */
std::string path(const std::string& itemId);

/** The cover is on the card. Main loop (SD). */
bool cached(const std::string& itemId);

/** Fetches the covers of these items that are not cached or already asked for. Main loop. */
void request(const std::vector<std::string>& itemIds);

/**
 * Fetches these items' covers in the background, a few at a time, while Wi-Fi is up (replaces an
 * earlier list). Cached ones are skipped as they come up. Main loop.
 */
void prefetch(const std::vector<std::string>& itemIds);

/** Drops requests not started yet and the prefetch (leaving the screen). */
void cancel();

/** Writes fetched covers to the card. Main loop (main.cpp, every loop). */
void service();

/** Increments whenever a cover was written, so screens know to redraw. */
uint32_t changeCounter();

}  // namespace AbsCovers

#endif  // INX_HAS_AUDIO_MODULE
