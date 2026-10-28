/**
 * @file UsbSerialHost.cpp
 * @brief Definitions for the USB host link to the audio module.
 */

#include "UsbSerialHost.h"

#if INX_HAS_AUDIO_MODULE

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <usb/cdc_acm_host.h>
#include <usb/usb_host.h>
#include <usb/vcp_ch34x.h>
#include <usb/vcp_cp210x.h>
#include <usb/vcp_ftdi.h>

#include <atomic>
#include <cstring>

#include "AudioModule.h"

namespace UsbSerialHost {

namespace {

constexpr size_t RX_BUFFER_BYTES = 16 * 1024;
constexpr size_t TX_TRANSFER_BYTES = 2048;
constexpr size_t RX_TRANSFER_BYTES = 512;
constexpr uint32_t OPEN_TIMEOUT_MS = 1000;
constexpr int OPEN_ATTEMPTS = 5;
constexpr uint32_t STOP_TIMEOUT_MS = 5000;

// Driver tasks must outrank the tasks that use the driver (loop task: 1, connect task: 4).
constexpr UBaseType_t HOST_TASK_PRIORITY = 5;
constexpr UBaseType_t CDC_DRIVER_PRIORITY = 6;
constexpr UBaseType_t CONNECT_TASK_PRIORITY = 4;
constexpr uint32_t TASK_STACK_BYTES = 4096;

constexpr uint32_t NOTIFY_NEW_DEVICE = 1u << 0;
constexpr uint32_t NOTIFY_DISCONNECTED = 1u << 1;
constexpr uint32_t NOTIFY_STOP = 1u << 2;

SemaphoreHandle_t lock = nullptr;  // guards handle, device and pending*
cdc_acm_dev_hdl_t handle = nullptr;
DeviceInfo connectedDevice;
DeviceInfo pendingDevice;
bool pendingValid = false;

StreamBufferHandle_t rxStream = nullptr;
StaticStreamBuffer_t rxStreamStruct;
uint8_t* rxStorage = nullptr;

TaskHandle_t hostTask = nullptr;
TaskHandle_t connectTask = nullptr;
SemaphoreHandle_t hostDone = nullptr;
SemaphoreHandle_t connectDone = nullptr;

std::atomic<State> currentState{State::Stopped};
std::atomic<bool> stopping{false};
uint32_t lineBaud = 921600;

std::atomic<uint32_t> rxBytes{0};
std::atomic<uint32_t> txBytes{0};
std::atomic<uint32_t> rxDropped{0};
std::atomic<uint32_t> connects{0};
std::atomic<uint32_t> disconnects{0};
std::atomic<uint32_t> lineErrors{0};

class LockGuard {
 public:
  LockGuard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(lock); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};

void copyStringDescriptor(const usb_str_desc_t* desc, char* out, const size_t outSize) {
  out[0] = '\0';
  if (!desc || outSize == 0) return;
  const size_t chars = desc->bLength > 2 ? (desc->bLength - 2) / 2 : 0;
  size_t written = 0;
  for (size_t i = 0; i < chars && written + 1 < outSize; ++i) {
    const uint16_t c = desc->wData[i];
    out[written++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  out[written] = '\0';
}

// USB Host context: read the descriptors, then let the connect task open the device.
void onNewDevice(usb_device_handle_t usbDev) {
  const usb_device_desc_t* desc = nullptr;
  if (usb_host_get_device_descriptor(usbDev, &desc) != ESP_OK || !desc) return;
  usb_device_info_t info{};
  DeviceInfo found;
  found.vid = desc->idVendor;
  found.pid = desc->idProduct;
  if (usb_host_device_info(usbDev, &info) == ESP_OK) {
    copyStringDescriptor(info.str_desc_product, found.product, sizeof(found.product));
  }
  {
    LockGuard guard;
    pendingDevice = found;
    pendingValid = true;
  }
  if (connectTask) xTaskNotify(connectTask, NOTIFY_NEW_DEVICE, eSetBits);
}

bool onData(const uint8_t* data, const size_t length, void*) {
  const size_t sent = xStreamBufferSend(rxStream, data, length, 0);
  rxBytes += sent;
  if (sent < length) rxDropped += static_cast<uint32_t>(length - sent);
  return true;
}

void onEvent(const cdc_acm_host_dev_event_data_t* event, void*) {
  switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
      if (connectTask) xTaskNotify(connectTask, NOTIFY_DISCONNECTED, eSetBits);
      break;
    case CDC_ACM_HOST_SERIAL_STATE: {
      // FTDI reports this per packet (sent here only when it changes). Overrun on the bridge means
      // its UART-to-USB buffer filled because the host did not poll fast enough.
      const cdc_acm_uart_state_t serial = event->data.serial_state;
      if (serial.bOverRun || serial.bFraming || serial.bParity) {
        ++lineErrors;
        AudioModule::logf("USB: bridge line error overrun=%d framing=%d parity=%d", serial.bOverRun ? 1 : 0,
                          serial.bFraming ? 1 : 0, serial.bParity ? 1 : 0);
      }
      break;
    }
    case CDC_ACM_HOST_ERROR:
      AudioModule::logf("USB: CDC error %d", event->data.error);
      break;
    default:
      break;
  }
}

bool isCh34xPid(const uint16_t pid) { return pid == CH340_PID || pid == CH340_PID_1 || pid == CH341_PID; }

esp_err_t openBridge(const DeviceInfo& dev, const char*& driver, cdc_acm_dev_hdl_t& out) {
  cdc_acm_host_device_config_t config{};
  config.connection_timeout_ms = OPEN_TIMEOUT_MS;
  config.out_buffer_size = TX_TRANSFER_BYTES;
  config.in_buffer_size = RX_TRANSFER_BYTES;
  config.event_cb = onEvent;
  config.data_cb = onData;
  config.user_arg = nullptr;

  if (dev.vid == FTDI_VID) {
    driver = "FTDI";
    return ftdi_vcp_open(dev.pid, 0, &config, &out);
  }
  if (dev.vid == SILICON_LABS_VID) {
    driver = "CP210x";
    return cp210x_vcp_open(dev.pid, 0, &config, &out);
  }
  if (dev.vid == NANJING_QINHENG_MICROE_VID && isCh34xPid(dev.pid)) {
    driver = "CH34x";
    return ch34x_vcp_open(dev.pid, 0, &config, &out);
  }
  // CH9102F (1A86:55D4) and other CDC-compliant bridges. The driver also falls back to any
  // interface with a pair of bulk endpoints when the descriptors are not CDC-compliant.
  driver = "CDC-ACM";
  return cdc_acm_host_open(dev.vid, dev.pid, 0, &config, &out);
}

void configureLine(cdc_acm_dev_hdl_t hdl) {
  // Deassert both first: on the ESP32 auto-reset circuit DTR=RTS keeps EN and IO0 high.
  esp_err_t err = cdc_acm_host_set_control_line_state(hdl, false, false);
  if (err != ESP_OK) AudioModule::logf("USB: DTR/RTS idle failed: %s", esp_err_to_name(err));

  cdc_acm_line_coding_t coding{};
  coding.dwDTERate = lineBaud;
  coding.bCharFormat = 0;
  coding.bParityType = 0;
  coding.bDataBits = 8;
  err = cdc_acm_host_line_coding_set(hdl, &coding);
  if (err != ESP_OK) AudioModule::logf("USB: set %lu baud failed: %s", static_cast<unsigned long>(lineBaud), esp_err_to_name(err));

  // Some bridges re-apply their power-on modem state with the line coding.
  cdc_acm_host_set_control_line_state(hdl, false, false);
}

void closeBridge(const char* reason) {
  cdc_acm_dev_hdl_t closing = nullptr;
  {
    LockGuard guard;
    closing = handle;
    handle = nullptr;
    connectedDevice = DeviceInfo{};
  }
  if (!closing) return;
  cdc_acm_host_close(closing);
  ++disconnects;
  if (!stopping) currentState = State::Waiting;
  AudioModule::logf("USB: bridge closed (%s)", reason);
}

void tryOpenPending(int& attemptsLeft) {
  DeviceInfo dev;
  {
    LockGuard guard;
    if (!pendingValid || handle) return;
    dev = pendingDevice;
  }

  const char* driver = "";
  cdc_acm_dev_hdl_t opened = nullptr;
  const esp_err_t err = openBridge(dev, driver, opened);
  if (err != ESP_OK) {
    AudioModule::logf("USB: open %04X:%04X via %s failed: %s", dev.vid, dev.pid, driver, esp_err_to_name(err));
    if (--attemptsLeft <= 0) {
      LockGuard guard;
      pendingValid = false;
    }
    return;
  }

  configureLine(opened);
  xStreamBufferReset(rxStream);
  dev.driver = driver;
  {
    LockGuard guard;
    handle = opened;
    connectedDevice = dev;
    pendingValid = false;
  }
  ++connects;
  currentState = State::Connected;
  AudioModule::logf("USB: %04X:%04X %s \"%s\" at %lu baud", dev.vid, dev.pid, driver, dev.product,
                    static_cast<unsigned long>(lineBaud));
}

void connectTaskMain(void*) {
  int attemptsLeft = OPEN_ATTEMPTS;
  for (;;) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
    if (stopping || (bits & NOTIFY_STOP)) break;
    if (bits & NOTIFY_DISCONNECTED) closeBridge("unplugged");
    if (bits & NOTIFY_NEW_DEVICE) attemptsLeft = OPEN_ATTEMPTS;
    tryOpenPending(attemptsLeft);
  }
  closeBridge("link stopped");
  connectTask = nullptr;
  xSemaphoreGive(connectDone);
  vTaskDelete(nullptr);
}

void hostTaskMain(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (!stopping) continue;
    // stop() has deregistered the CDC client (NO_CLIENTS) or called usb_host_lib_unblock().
    // ESP_OK: nothing left to free. Otherwise ALL_FREE arrives once the devices are gone.
    if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) || usb_host_device_free_all() == ESP_OK) break;
  }
  hostTask = nullptr;
  xSemaphoreGive(hostDone);
  vTaskDelete(nullptr);
}

bool ensurePrimitives() {
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!hostDone) hostDone = xSemaphoreCreateBinary();
  if (!connectDone) connectDone = xSemaphoreCreateBinary();
  if (!rxStorage) {
    // +1: a static stream buffer needs one spare byte of storage.
    rxStorage = static_cast<uint8_t*>(heap_caps_malloc(RX_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rxStorage) rxStream = xStreamBufferCreateStatic(RX_BUFFER_BYTES, 1, rxStorage, &rxStreamStruct);
  }
  return lock && hostDone && connectDone && rxStream;
}

}  // namespace

bool start(const uint32_t baud) {
  const State now = currentState.load();
  if (now == State::Waiting || now == State::Connected) return true;
  if (!ensurePrimitives()) {
    AudioModule::logf("USB: out of memory");
    currentState = State::Failed;
    return false;
  }
  lineBaud = baud;
  stopping = false;
  xSemaphoreTake(hostDone, 0);
  xSemaphoreTake(connectDone, 0);
  {
    LockGuard guard;
    pendingValid = false;
  }

  // The root port stays unpowered until the CDC client is registered, so a module that is
  // already plugged in cannot finish enumerating before onNewDevice() is listening.
  usb_host_config_t hostConfig{};
  hostConfig.skip_phy_setup = false;
  hostConfig.root_port_unpowered = true;
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    AudioModule::logf("USB: host install failed: %s", esp_err_to_name(err));
    currentState = State::Failed;
    return false;
  }
  if (xTaskCreate(hostTaskMain, "usb_host", TASK_STACK_BYTES, nullptr, HOST_TASK_PRIORITY, &hostTask) != pdPASS) {
    usb_host_uninstall();
    AudioModule::logf("USB: host task create failed");
    currentState = State::Failed;
    return false;
  }

  cdc_acm_host_driver_config_t cdcConfig{};
  cdcConfig.driver_task_stack_size = TASK_STACK_BYTES;
  cdcConfig.driver_task_priority = CDC_DRIVER_PRIORITY;
  cdcConfig.xCoreID = tskNO_AFFINITY;
  cdcConfig.new_dev_cb = onNewDevice;
  err = cdc_acm_host_install(&cdcConfig);
  if (err != ESP_OK) {
    AudioModule::logf("USB: CDC install failed: %s", esp_err_to_name(err));
    currentState = State::Failed;
    stop();
    return false;
  }

  if (xTaskCreate(connectTaskMain, "usb_link", TASK_STACK_BYTES, nullptr, CONNECT_TASK_PRIORITY, &connectTask) !=
      pdPASS) {
    AudioModule::logf("USB: link task create failed");
    currentState = State::Failed;
    stop();
    return false;
  }

  currentState = State::Waiting;
  err = usb_host_lib_set_root_port_power(true);
  if (err != ESP_OK) AudioModule::logf("USB: root port power failed: %s", esp_err_to_name(err));
  AudioModule::logf("USB: host started, %lu baud", static_cast<unsigned long>(baud));
  return true;
}

void stop() {
  if (currentState == State::Stopped) return;
  stopping = true;

  if (connectTask) {
    xTaskNotify(connectTask, NOTIFY_STOP, eSetBits);
    if (xSemaphoreTake(connectDone, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
      AudioModule::logf("USB: link task did not stop");
    }
  }

  // Deregistering the CDC client wakes the host task with NO_CLIENTS; unblock covers a failed
  // start() where the client never registered. The task then frees the devices and exits.
  cdc_acm_host_uninstall();
  usb_host_lib_unblock();
  if (hostTask && xSemaphoreTake(hostDone, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
    AudioModule::logf("USB: host task did not stop; reboot to recover");
    currentState = State::Failed;
    return;
  }
  const esp_err_t err = usb_host_uninstall();
  if (err != ESP_OK) AudioModule::logf("USB: host uninstall failed: %s", esp_err_to_name(err));
  if (rxStream) xStreamBufferReset(rxStream);
  stopping = false;
  currentState = State::Stopped;
  AudioModule::logf("USB: host stopped");
}

State state() { return currentState.load(); }

bool isConnected() { return currentState.load() == State::Connected; }

DeviceInfo device() {
  if (!lock) return DeviceInfo{};
  LockGuard guard;
  return connectedDevice;
}

Counters counters() {
  Counters out;
  out.rxBytes = rxBytes.load();
  out.txBytes = txBytes.load();
  out.rxDropped = rxDropped.load();
  out.connects = connects.load();
  out.disconnects = disconnects.load();
  out.lineErrors = lineErrors.load();
  return out;
}

size_t write(const uint8_t* data, const size_t length, const uint32_t timeoutMs) {
  if (!lock || !data || length == 0) return 0;
  LockGuard guard;
  if (!handle) return 0;
  if (cdc_acm_host_data_tx_blocking(handle, data, length, timeoutMs) != ESP_OK) return 0;
  txBytes += static_cast<uint32_t>(length);
  return length;
}

size_t read(uint8_t* data, const size_t length, const uint32_t timeoutMs) {
  if (!rxStream || !data || length == 0) return 0;
  return xStreamBufferReceive(rxStream, data, length, pdMS_TO_TICKS(timeoutMs));
}

void clearReceived() {
  if (rxStream) xStreamBufferReset(rxStream);
}

}  // namespace UsbSerialHost

#endif  // INX_HAS_AUDIO_MODULE
