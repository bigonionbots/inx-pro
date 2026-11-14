#pragma once

/**
 * @file AbsWifi.h
 * @brief Wi-Fi for the Audiobookshelf features: connect on demand, off again when unused.
 *
 * acquire() connects with the saved networks (WIFI_STORE) if Wi-Fi is down and counts a user;
 * release() drops the count and turns Wi-Fi off once nobody needs it, but only if acquire()
 * turned it on (Wi-Fi someone else started stays up). The saved networks come from the SD card,
 * so prepare() loads them on the main loop; acquire() itself may run on any task.
 *
 * Screens do not rely on acquire() to get online: they open the Wi-Fi picker when Wi-Fi is down
 * (as every network screen does) and then adopt() what it connected. acquire()'s silent tries of
 * the saved networks are for background work (streaming, progress sync) with no screen to ask.
 */

#if INX_HAS_AUDIO_MODULE

#include <string>

namespace AbsWifi {

/** Loads the saved networks. Main loop (SD); call at boot (it also starts the Wi-Fi step task). */
void prepare();
bool hasSavedNetworks();
/** Blocks up to ~15 s per saved network. Any task. */
bool acquire(std::string& error);
void release();
bool connected();
/** The Wi-Fi picker connected for us: turn it off again with the last release(). Main loop. */
void adopt();
/** Turns Wi-Fi off unless something holds it (after a cancelled picker). Main loop. */
void offIfUnused();

/** RAII user of the connection. */
class Use {
 public:
  explicit Use(std::string& error) : ok_(acquire(error)) {}
  ~Use() {
    if (ok_) release();
  }
  Use(const Use&) = delete;
  Use& operator=(const Use&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_;
};

}  // namespace AbsWifi

#endif  // INX_HAS_AUDIO_MODULE
