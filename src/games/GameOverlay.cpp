/**
 * @file GameOverlay.cpp
 * @brief Definitions for the game-mode screen overlay.
 */

#include "GameOverlay.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "GameDisplay.h"
#include "GameUi.h"
#include "OverlayPng.h"
#include "generated/OverlayTemplatePng.h"
#include "util/SdIoMutex.h"

namespace GameOverlay {

namespace {

constexpr const char* SELECTION_FILE = "/Games/.overlay";
constexpr size_t MAX_PNG_BYTES = 4u * 1024 * 1024;
constexpr int PLANE_BYTES = GameDisplay::WIDTH * GameDisplay::HEIGHT / 8;

uint8_t* ink = nullptr;     // 1 = white
uint8_t* opaque = nullptr;  // 1 = drawn

bool isPng(const std::string& name) {
  if (name.size() < 5 || name[0] == '.') return false;
  std::string ext = name.substr(name.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".png";
}

}  // namespace

std::string selected() {
  SdIoMutex::Lock lock;
  FsFile file = SdMan.open(SELECTION_FILE, O_RDONLY);
  if (!file) return "";
  char name[128] = {};
  const int got = file.read(name, sizeof(name) - 1);
  file.close();
  std::string value(name, got > 0 ? static_cast<size_t>(got) : 0);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}

void select(const std::string& name) {
  SdIoMutex::Lock lock;
  if (name.empty()) {
    SdMan.remove(SELECTION_FILE);
    return;
  }
  FsFile file = SdMan.open(SELECTION_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(name.data(), name.size());
  file.close();
}

std::vector<std::string> list() {
  std::vector<std::string> names;
  SdIoMutex::Lock lock;
  FsFile dir = SdMan.open(DIRECTORY, O_RDONLY);
  if (dir && dir.isDirectory()) {
    char name[128];
    for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      const bool file = !entry.isDirectory();
      entry.close();
      if (file && isPng(name)) names.emplace_back(name);
    }
  }
  if (dir) dir.close();
  std::sort(names.begin(), names.end());
  return names;
}

void ensureTemplate() {
  SdIoMutex::Lock lock;
  SdMan.mkdir(DIRECTORY);
  const struct {
    const char* name;
    const uint8_t* data;
    size_t size;
  } files[] = {{TEMPLATE_NAME, OVERLAY_TEMPLATE_PNG, OVERLAY_TEMPLATE_PNG_SIZE},
               {EXAMPLE_NAME, OVERLAY_EXAMPLE_PNG, OVERLAY_EXAMPLE_PNG_SIZE}};
  for (const auto& f : files) {
    const std::string path = std::string(DIRECTORY) + "/" + f.name;
    if (SdMan.exists(path.c_str())) continue;
    FsFile file = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) continue;
    file.write(f.data, f.size);
    file.close();
  }
}

const char* load() {
  const std::string name = selected();
  if (name.empty()) return nullptr;
  const std::string path = std::string(DIRECTORY) + "/" + name;
  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) return "overlay file missing";
  const size_t length = static_cast<size_t>(file.fileSize());
  if (length > MAX_PNG_BYTES) {
    file.close();
    return "overlay file too large";
  }
  auto* png = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const bool read = png && file.read(png, length) == static_cast<int>(length);
  file.close();
  if (!read) {
    free(png);
    return "overlay could not be read";
  }
  if (!ink) ink = static_cast<uint8_t*>(heap_caps_malloc(PLANE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!opaque) opaque = static_cast<uint8_t*>(heap_caps_malloc(PLANE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  OverlayPng::Error error = OverlayPng::Error::OutOfMemory;
  if (ink && opaque) error = OverlayPng::decode(png, length, GameDisplay::WIDTH, GameDisplay::HEIGHT, ink, opaque);
  free(png);
  if (error != OverlayPng::Error::None) {
    free(ink);
    free(opaque);
    ink = nullptr;
    opaque = nullptr;
    INX_SERIAL.printf("[GAME] overlay %s: %s\n", name.c_str(), OverlayPng::describe(error));
    return OverlayPng::describe(error);
  }
  INX_SERIAL.printf("[GAME] overlay %s loaded\n", name.c_str());
  return nullptr;
}

void apply(FASTEPD& epd, const int x, const int y, const int w, const int h) {
  if (!ink || !opaque) return;
  constexpr int PITCH = GameDisplay::WIDTH / 8;
  constexpr int AREA_FIRST = GameUi::AREA_X / 8;
  constexpr int AREA_END = (GameUi::AREA_X + GameUi::AREA_W) / 8;
  uint8_t* buffer = epd.currentBuffer();
  const int top = std::max(0, y);
  const int bottom = std::min(GameDisplay::HEIGHT, y + h);
  const int first = std::max(0, x / 8);
  const int end = std::min(PITCH, (x + w + 7) / 8);
  for (int row = top; row < bottom; ++row) {
    const bool inArea = row >= GameUi::AREA_Y && row < GameUi::AREA_Y + GameUi::AREA_H;
    const size_t at = static_cast<size_t>(row) * PITCH;
    for (int i = first; i < end; ++i) {
      if (inArea && i >= AREA_FIRST && i < AREA_END) continue;
      const uint8_t m = opaque[at + i];
      buffer[at + i] = static_cast<uint8_t>((buffer[at + i] & ~m) | (ink[at + i] & m));
    }
  }
}

}  // namespace GameOverlay

#endif  // INX_HAS_GAMES
