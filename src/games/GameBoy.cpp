/**
 * @file GameBoy.cpp
 * @brief Definitions for the Game Boy game-mode screen.
 */

#include "GameBoy.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <HalGPIO.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "GameDisplay.h"
#include "GameMode.h"
#include "GameOverlay.h"
#include "GameSound.h"
#include "GameUi.h"
#include "GameVideo.h"
#include "gb/GbSession.h"

extern HalGPIO gpio;

namespace GameBoy {

namespace {

using GameUi::AREA_H;
using GameUi::AREA_W;
using GameUi::AREA_X;
using GameUi::AREA_Y;
using GameUi::BAR_H;
using GameUi::BOTTOM_Y;
using GameUi::Contact;

constexpr int SCALE = 3;
constexpr size_t MAX_ROM_BYTES = 4u * 1024 * 1024;
constexpr int64_t FRAME_US = 16743;  // 59.73 Hz
constexpr int64_t MAX_LAG_US = 4 * FRAME_US;
constexpr uint32_t SAVE_IDLE_MS = 3000;  // write the .sav this long after the game stops writing
constexpr uint32_t STATS_MS = 2000;
/** Sound samples per frame at GBS_AUDIO_RATE, Q16: 44100 * 70224 / 4194304 = 738.35. */
constexpr uint32_t AUDIO_PER_FRAME_Q16 =
    static_cast<uint32_t>(static_cast<uint64_t>(GBS_AUDIO_RATE) * 70224 * 65536 / 4194304);
constexpr int AUDIO_MAX_PER_FRAME = 740;

using namespace GameUi::GameBoyControls;

// ---------------------------------------------------------------------------------------------
// Frame rendering: 3x scale, 2x2 Bayer dither (the shades checked in the milestone 1 test).

/** 12 output pixels (MSB first, 1 = white) for one LCD byte (4 Game Boy pixels), per dither row. */
uint16_t pixelLut[2][256];

void buildLut() {
  constexpr int BAYER[2][2] = {{0, 2}, {3, 1}};
  constexpr int INK[4] = {0, 1, 3, 4};  // black pixels out of 4, per shade
  for (int phase = 0; phase < 2; ++phase) {
    for (int b = 0; b < 256; ++b) {
      uint16_t out = 0;
      for (int o = 0; o < 4 * SCALE; ++o) {
        const int shade = (b >> ((o / SCALE) * 2)) & 3;
        const bool black = BAYER[phase][o & 1] < INK[shade];  // chunks start at even x
        out = static_cast<uint16_t>((out << 1) | (black ? 0 : 1));
      }
      pixelLut[phase][b] = out;
    }
  }
}

/** Draws the Game Boy frame into the panel buffer; marks only rows whose pixels changed. */
void renderFrame(FASTEPD& epd, const uint8_t* lcd) {
  constexpr int PITCH = GameDisplay::WIDTH / 8;
  constexpr int OUT_BYTES = AREA_W / 8;
  uint8_t* buffer = epd.currentBuffer();
  uint8_t row[2][OUT_BYTES];
  for (int gy = 0; gy < GBS_LCD_H; ++gy) {
    const uint8_t* src = lcd + gy * GBS_LCD_ROW_BYTES;
    for (int phase = 0; phase < 2; ++phase) {
      uint8_t* out = row[phase];
      for (int i = 0; i < GBS_LCD_ROW_BYTES; i += 2) {
        const uint32_t v = (static_cast<uint32_t>(pixelLut[phase][src[i]]) << 12) | pixelLut[phase][src[i + 1]];
        *out++ = static_cast<uint8_t>(v >> 16);
        *out++ = static_cast<uint8_t>(v >> 8);
        *out++ = static_cast<uint8_t>(v);
      }
    }
    for (int r = 0; r < SCALE; ++r) {
      const int y = AREA_Y + gy * SCALE + r;
      uint8_t* dst = buffer + y * PITCH + AREA_X / 8;
      const uint8_t* line = row[y & 1];
      if (std::memcmp(dst, line, OUT_BYTES) != 0) {
        std::memcpy(dst, line, OUT_BYTES);
        GameVideo::markRows(y, y);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Controls.

void drawControls(FASTEPD& epd) {
  // D-pad: a cross with its arms outlined.
  const int h = DPAD_WIDTH / 2;
  epd.drawRect(DPAD_X - h, DPAD_Y - DPAD_ARM, DPAD_WIDTH, 2 * DPAD_ARM, BBEP_BLACK);
  epd.drawRect(DPAD_X - DPAD_ARM, DPAD_Y - h, 2 * DPAD_ARM, DPAD_WIDTH, BBEP_BLACK);
  epd.fillRect(DPAD_X - h + 1, DPAD_Y - h + 1, DPAD_WIDTH - 2, DPAD_WIDTH - 2, BBEP_WHITE);
  epd.fillCircle(DPAD_X, DPAD_Y, 8, BBEP_BLACK);
  GameUi::text(epd, DPAD_X - 6, DPAD_Y - DPAD_ARM + 12, "^");
  GameUi::text(epd, DPAD_X - 6, DPAD_Y + DPAD_ARM - 28, "v");
  GameUi::text(epd, DPAD_X - DPAD_ARM + 12, DPAD_Y - 8, "<");
  GameUi::text(epd, DPAD_X + DPAD_ARM - 24, DPAD_Y - 8, ">");

  epd.drawCircle(A_X, A_Y, AB_R, BBEP_BLACK);
  epd.drawCircle(A_X, A_Y, AB_R - 1, BBEP_BLACK);
  GameUi::text(epd, A_X - 6, A_Y - 8, "A");
  epd.drawCircle(B_X, B_Y, AB_R, BBEP_BLACK);
  epd.drawCircle(B_X, B_Y, AB_R - 1, BBEP_BLACK);
  GameUi::text(epd, B_X - 6, B_Y - 8, "B");

  const int y = BOTTOM_Y + 6;
  const int hgt = GameDisplay::HEIGHT - y - 4;
  epd.drawRect(SELECT_X, y, SMALL_W, hgt, BBEP_BLACK);
  GameUi::text(epd, SELECT_X + (SMALL_W - GameUi::textWidth("SELECT")) / 2, y + (hgt - 16) / 2, "SELECT");
  epd.drawRect(START_X, y, SMALL_W, hgt, BBEP_BLACK);
  GameUi::text(epd, START_X + (SMALL_W - GameUi::textWidth("START")) / 2, y + (hgt - 16) / 2, "START");
}

uint8_t buttonsFor(const Contact* touches, const int count) {
  uint8_t pressed = 0;
  for (int i = 0; i < count; ++i) {
    const Contact& c = touches[i];
    if (c.y < BAR_H) continue;
    if (c.x < AREA_X) {  // D-pad, 8-way: a touch off an axis by more than ~22 degrees presses both directions
      const int dx = c.x - DPAD_X;
      const int dy = c.y - DPAD_Y;
      if (dx * dx + dy * dy < DPAD_DEAD * DPAD_DEAD) continue;
      const float angle = std::atan2(static_cast<float>(-dy), static_cast<float>(dx)) * 180.0f / 3.14159265f;
      const float a = angle < 0 ? angle + 360.0f : angle;
      if (a < 67.5f || a >= 292.5f) pressed |= GBS_RIGHT;
      if (a >= 22.5f && a < 157.5f) pressed |= GBS_UP;
      if (a >= 112.5f && a < 247.5f) pressed |= GBS_LEFT;
      if (a >= 202.5f && a < 337.5f) pressed |= GBS_DOWN;
    } else if (c.x >= AREA_X + AREA_W) {  // A and B: the nearer one, both when between them
      const int da = (c.x - A_X) * (c.x - A_X) + (c.y - A_Y) * (c.y - A_Y);
      const int db = (c.x - B_X) * (c.x - B_X) + (c.y - B_Y) * (c.y - B_Y);
      const int mx = (A_X + B_X) / 2;
      const int my = (A_Y + B_Y) / 2;
      const int dm = (c.x - mx) * (c.x - mx) + (c.y - my) * (c.y - my);
      if (dm < 30 * 30) {
        pressed |= GBS_A | GBS_B;
      } else if (std::min(da, db) < AB_REACH * AB_REACH) {
        pressed |= da <= db ? GBS_A : GBS_B;
      }
    } else if (c.y >= BOTTOM_Y) {
      if (c.x >= SELECT_X - 20 && c.x < SELECT_X + SMALL_W + 20) pressed |= GBS_SELECT;
      if (c.x >= START_X - 20 && c.x < START_X + SMALL_W + 20) pressed |= GBS_START;
    }
  }
  return pressed;
}

// ---------------------------------------------------------------------------------------------
// ROM and save files.

uint8_t* loadRom(const char* path, size_t* size) {
  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) return nullptr;
  const size_t length = static_cast<size_t>(file.fileSize());
  if (length < 0x150 || length > MAX_ROM_BYTES) {
    INX_SERIAL.printf("[GAME] ROM size %u not supported\n", static_cast<unsigned>(length));
    file.close();
    return nullptr;
  }
  auto* rom = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  size_t done = 0;
  while (rom && done < length) {
    const int got = file.read(rom + done, std::min<size_t>(32 * 1024, length - done));
    if (got <= 0) break;
    done += static_cast<size_t>(got);
  }
  file.close();
  if (!rom || done != length) {
    free(rom);
    return nullptr;
  }
  *size = length;
  return rom;
}

std::string savePath(const char* romPath) {
  std::string path(romPath);
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.erase(dot);
  return path + ".sav";
}

void loadSave(const std::string& path) {
  size_t size = 0;
  uint8_t* ram = gbs_cart_ram(&size);
  if (!ram || !gbs_has_battery()) return;
  FsFile file = SdMan.open(path.c_str(), O_RDONLY);
  if (!file) return;
  if (static_cast<size_t>(file.fileSize()) >= size) {
    file.read(ram, size);
    INX_SERIAL.printf("[GAME] loaded %s\n", path.c_str());
  }
  file.close();
}

/** Writes cartridge RAM to <name>.sav through a temporary file, so a cut-off write keeps the old save. */
void writeSave(const std::string& path) {
  size_t size = 0;
  const uint8_t* ram = gbs_cart_ram(&size);
  if (!ram || !gbs_has_battery()) return;
  const std::string temp = path + ".tmp";
  FsFile file = SdMan.open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) return;
  const bool ok = file.write(ram, size) == size;
  file.close();
  if (!ok) {
    SdMan.remove(temp.c_str());
    return;
  }
  SdMan.remove(path.c_str());
  SdMan.rename(temp.c_str(), path.c_str());
  INX_SERIAL.printf("[GAME] saved %s\n", path.c_str());
}

const char* baseName(const char* path) {
  const char* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/** Second line of the top bar, left of the light controls (up to 38 characters). */
void drawStatus(FASTEPD& epd, const char* line) {
  constexpr int Y = 28;
  epd.fillRect(0, Y, 470, 20, BBEP_WHITE);
  GameUi::text(epd, 8, Y + 2, line);
  GameOverlay::apply(epd, 0, Y, 470, 20);
  GameVideo::markRows(Y, Y + 19);
}

}  // namespace

void run(const char* romPath) {
  // The overlay first: decoding it needs a few MB of PSRAM for a moment, before the ROM takes its share.
  const char* overlayError = GameOverlay::load();
  size_t romSize = 0;
  uint8_t* rom = loadRom(romPath, &romSize);
  if (!rom) {
    INX_SERIAL.printf("[GAME] could not read %s\n", romPath);
    return;
  }
  const bool sound = GameSound::begin();
  if (!gbs_open(rom, romSize, sound)) {
    free(rom);
    return;
  }
  const std::string save = savePath(romPath);
  loadSave(save);
  buildLut();

  FASTEPD& epd = GameDisplay::epd();
  epd.setFont(FONT_12x16);
  epd.fillScreen(BBEP_WHITE);
  char title[40];
  std::snprintf(title, sizeof(title), "%.36s", baseName(romPath));
  char subtitle[40];
  std::snprintf(subtitle, sizeof(subtitle), "%s%.30s", overlayError ? "Overlay: " : "",
                overlayError ? overlayError : (gbs_is_color() ? "Game Boy Color" : "Game Boy"));
  GameUi::drawBar(epd, title, subtitle);
  epd.drawRect(AREA_X - 2, AREA_Y, AREA_W + 4, AREA_H, BBEP_BLACK);
  drawControls(epd);
  GameUi::drawSoundControls(epd);
  GameOverlay::apply(epd, 0, 0, GameDisplay::WIDTH, GameDisplay::HEIGHT);
  epd.fullUpdate(CLEAR_FAST, true);
  if (!GameVideo::start()) {
    gbs_close();
    free(rom);
    return;
  }

  GameUi::BarInput bar;
  bool cartWritten = false;
  uint32_t lastCartWrite = 0;
  uint32_t statsStart = millis();
  const uint32_t statusAfter = statsStart + (overlayError ? 8000 : 0);  // leave an overlay error up a while
  uint32_t statsFrames = 0;
  uint32_t lastScans = GameVideo::scans();
  uint32_t emuUs = 0;
  int64_t nextFrame = esp_timer_get_time();
  uint32_t audioDue = 0;  // Q16 samples owed
  static int16_t audio[AUDIO_MAX_PER_FRAME];

  for (;;) {
    gpio.update();
    Contact touches[GameUi::MAX_CONTACTS];
    const int count = GameUi::contacts(touches, GameUi::MAX_CONTACTS);
    const GameUi::BarButton button = bar.update(epd, touches, count);
    const bool leave = button == GameUi::BarButton::Exit || gpio.wasPressed(HalGPIO::BTN_POWER);
    if (leave) break;
    // The sound readout also changes by itself (the module reports its volume); check it now and then.
    const bool soundChanged =
        GameUi::isSoundButton(button) || (statsFrames % 15 == 0 && GameUi::refreshSoundReadout(epd));
    if (soundChanged) {
      GameOverlay::apply(epd, GameUi::SOUND_X, BOTTOM_Y, GameDisplay::WIDTH - GameUi::SOUND_X,
                         GameDisplay::HEIGHT - BOTTOM_Y);
      GameVideo::markRows(BOTTOM_Y, GameDisplay::HEIGHT - 1);
    } else if (button != GameUi::BarButton::None) {
      GameOverlay::apply(epd, 0, 0, GameDisplay::WIDTH, BAR_H);
      GameVideo::markRows(0, BAR_H - 1);
    }

    gbs_set_buttons(buttonsFor(touches, count));
    const int64_t started = esp_timer_get_time();
    gbs_run_frame();
    if (sound) {
      audioDue += AUDIO_PER_FRAME_Q16;
      const int samples = static_cast<int>(audioDue >> 16);
      audioDue &= 0xFFFF;
      gbs_render_audio(audio, samples);
      GameSound::push(audio, static_cast<size_t>(samples));
    }
    emuUs += static_cast<uint32_t>(esp_timer_get_time() - started);
    GameSound::service();
    renderFrame(epd, gbs_lcd());
    ++statsFrames;

    if (gbs_take_cart_ram_written()) {
      cartWritten = true;
      lastCartWrite = millis();
    }
    if (cartWritten && millis() - lastCartWrite >= SAVE_IDLE_MS) {
      writeSave(save);
      cartWritten = false;
    }

    if (millis() - statsStart >= STATS_MS) {
      const uint32_t elapsed = millis() - statsStart;
      const uint32_t scans = GameVideo::scans();
      char line[48];
      const char* soundText = GameSound::statusText();
      // With sound, its state replaces the "/s" so the line still fits (38 characters).
      std::snprintf(line, sizeof(line), "%s %lu fps, emu %lu ms, scan %lu%s%s", gbs_is_color() ? "GBC" : "GB",
                    static_cast<unsigned long>(statsFrames * 1000 / elapsed),
                    static_cast<unsigned long>(statsFrames ? emuUs / statsFrames / 1000 : 0),
                    static_cast<unsigned long>((scans - lastScans) * 1000 / elapsed), soundText[0] ? " " : "/s",
                    soundText);
      INX_SERIAL.printf("[GAME] %s\n", line);
      if (static_cast<int32_t>(millis() - statusAfter) >= 0) drawStatus(epd, line);
      lastScans = scans;
      statsFrames = 0;
      emuUs = 0;
      statsStart = millis();
    }

    // Pace to the Game Boy's frame rate. A late frame (a slow touch read, a save) is made up by
    // running the next ones back to back; only a long stall is written off.
    nextFrame += FRAME_US;
    const int64_t now = esp_timer_get_time();
    if (nextFrame > now) {
      const int64_t wait = nextFrame - now;
      if (wait > 1500) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
      while (esp_timer_get_time() < nextFrame) {
      }
    } else if (now - nextFrame > MAX_LAG_US) {
      nextFrame = now;
      vTaskDelay(1);
    }
  }

  if (cartWritten) writeSave(save);
  GameSound::end();
  GameVideo::stop();
  gbs_close();
  free(rom);
  GameMode::exitToReader();
}

}  // namespace GameBoy

#endif  // INX_HAS_GAMES
