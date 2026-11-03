#pragma once

/**
 * @file AbsWifi.h
 * @brief Wi-Fi for the Audiobookshelf features: connect on demand, off again when unused.
 *
 * acquire() connects with the saved networks (WIFI_STORE) if Wi-Fi is down and counts a user;
 * release() drops the count and turns Wi-Fi off once nobody needs it, but only if acquire()
 * turned it on (Wi-Fi someone else started stays up). The saved networks come from the SD card,
 * so prepare() loads them on the main loop; acquire() itself may run on any task.
 */

#if INX_HAS_AUDIO_MODULE

#include <string>

namespace AbsWifi {

/** Loads the saved networks. Main loop (SD); call before acquire() can be needed. */
void prepare();
bool hasSavedNetworks();
/** Blocks up to ~15 s per saved network. Any task. */
bool acquire(std::string& error);
void release();
bool connected();

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
