/**
 * @file AbsWorker.cpp
 * @brief One long-lived task for screens' Audiobookshelf network work.
 */

#include "AbsWorker.h"

#if INX_HAS_AUDIO_MODULE

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <memory>

namespace AbsWorker {

namespace {

constexpr uint32_t STACK_BYTES = 12288;  // TLS
constexpr UBaseType_t QUEUE_LENGTH = 4;

struct Job {
  std::function<void()> body;
};

QueueHandle_t queue = nullptr;

void workerMain(void*) {
  for (;;) {
    Job* raw = nullptr;
    if (xQueueReceive(queue, &raw, portMAX_DELAY) != pdTRUE) continue;
    std::unique_ptr<Job> job(raw);
    job->body();
  }
}

}  // namespace

bool post(std::function<void()> body) {
  if (!queue) {
    queue = xQueueCreate(QUEUE_LENGTH, sizeof(Job*));  // before the task, which reads it
    if (!queue) return false;
    if (xTaskCreatePinnedToCoreWithCaps(workerMain, "abs_ui", STACK_BYTES, nullptr, 1, nullptr, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      vQueueDelete(queue);
      queue = nullptr;
      return false;
    }
  }
  auto* job = new Job{std::move(body)};
  if (xQueueSend(queue, &job, 0) == pdTRUE) return true;
  delete job;
  return false;
}

}  // namespace AbsWorker

#endif  // INX_HAS_AUDIO_MODULE
