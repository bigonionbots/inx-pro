/**
 * @file GameDisplay.cpp
 * @brief Definitions for the game-mode panel driver.
 */

#include "GameDisplay.h"

#if INX_HAS_GAMES

#include <Arduino.h>
#include <BoardT5S3Pins.h>
#include <LgfxEpdConfig.h>
#include <driver/gpio.h>

namespace freeink {
// Provided by freeink-sdk's BoardT5S3 (pins and the PCA9535/TPS65185 power sequence).
const LgfxEpdConfig& lilygoT5S3LgfxConfig();
}  // namespace freeink

namespace GameDisplay {

namespace {

FASTEPD panel;

const freeink::LgfxEpdConfig& board() { return freeink::lilygoT5S3LgfxConfig(); }

int ioInit(void*) {
  const auto& cfg = board();
  for (const int8_t pin : {cfg.pinSpv, cfg.pinCkv, cfg.pinLe}) {
    gpio_reset_pin(static_cast<gpio_num_t>(pin));
    gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
  }
  return cfg.power.prepare && !cfg.power.prepare() ? BBEP_IO_ERROR : BBEP_SUCCESS;
}

int einkPower(void* raw, const int on) {
  auto* state = static_cast<FASTEPDSTATE*>(raw);
  if (on == state->pwr_on) return BBEP_SUCCESS;
  const auto& cfg = board();
  if (on) {
    if (!cfg.power.powerOn || !cfg.power.powerOn()) return BBEP_IO_ERROR;
  } else if (cfg.power.powerOff) {
    cfg.power.powerOff();
  }
  state->pwr_on = static_cast<uint8_t>(on);
  return BBEP_SUCCESS;
}

// Standard gate-driver sequencing for the ED047TC1: STV (SPV) starts a frame, CKV clocks one row,
// LE latches the row the source drivers just received. Only plain GPIOs on this board.
void IRAM_ATTR rowControl(void* raw, const int step) {
  const auto* state = static_cast<FASTEPDSTATE*>(raw);
  const auto ckv = static_cast<gpio_num_t>(state->panelDef.ioCKV);
  const auto spv = static_cast<gpio_num_t>(state->panelDef.ioSPV);
  const auto le = static_cast<gpio_num_t>(state->panelDef.ioLE);
  if (step == ROW_START) {
    gpio_set_level(ckv, 1);
    delayMicroseconds(7);
    gpio_set_level(spv, 0);
    delayMicroseconds(10);
    gpio_set_level(ckv, 0);
    gpio_set_level(ckv, 1);
    delayMicroseconds(8);
    gpio_set_level(spv, 1);
    delayMicroseconds(10);
    // Clock past the panel's invisible leading rows.
    for (int i = 0; i < 3; ++i) {
      gpio_set_level(ckv, 0);
      gpio_set_level(ckv, 1);
      delayMicroseconds(10);
    }
  } else if (step == ROW_STEP) {
    gpio_set_level(ckv, 0);
    gpio_set_level(le, 1);
    gpio_set_level(le, 0);
  }
}

}  // namespace

bool begin() {
  const auto& cfg = board();
  BBPANELDEF def = {};
  def.width = WIDTH;
  def.height = HEIGHT;
  def.bus_speed = cfg.busHz;
  def.flags = BB_PANEL_FLAG_NONE;
  for (int i = 0; i < 8; ++i) def.data[i] = static_cast<uint8_t>(cfg.dataPins[i]);
  def.bus_width = 8;
  def.ioPWR = BB_NOT_USED;
  def.ioSPV = static_cast<uint8_t>(cfg.pinSpv);
  def.ioCKV = static_cast<uint8_t>(cfg.pinCkv);
  def.ioSPH = static_cast<uint8_t>(cfg.pinSph);
  def.ioOE = BB_NOT_USED;  // behind the PCA9535; the power hooks drive it
  def.ioLE = static_cast<uint8_t>(cfg.pinLe);
  def.ioCL = static_cast<uint8_t>(cfg.pinCl);
  def.ioPWR_Good = BB_NOT_USED;
  def.ioSDA = BB_NOT_USED;  // the I2C bus belongs to Wire (touch, charger, expander)
  def.ioSCL = BB_NOT_USED;
  def.ioShiftSTR = BB_NOT_USED;
  def.ioShiftMask = 0;
  def.ioDCDummy = static_cast<uint8_t>(T5S3_LORA_CS);  // unpopulated on the Pro Lite, as in the SDK config
  def.pGrayMatrix = nullptr;                          // FastEPD's ED047TC1-class default
  def.iMatrixSize = 0;
  def.iLinePadding = 16;
  def.iVCOM = -1600;

  BBPANELPROCS procs = {};
  procs.pfnEinkPower = einkPower;
  procs.pfnIOInit = ioInit;
  procs.pfnRowControl = rowControl;

  if (panel.initCustomPanel(&def, &procs) != BBEP_SUCCESS) return false;
  panel.setMode(BB_MODE_1BPP);
  panel.fillScreen(BBEP_WHITE);
  return panel.fullUpdate(CLEAR_SLOW, true) == BBEP_SUCCESS;
}

FASTEPD& epd() { return panel; }

void end() { panel.einkPower(0); }

}  // namespace GameDisplay

#endif  // INX_HAS_GAMES
