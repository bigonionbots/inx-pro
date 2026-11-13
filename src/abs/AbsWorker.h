#pragma once

/**
 * @file AbsWorker.h
 * @brief One long-lived task for screens' Audiobookshelf network work.
 */

#if INX_HAS_AUDIO_MODULE

#include <functional>

/**
 * Screens' network work (sign in, load a book, fetch stats, ...) runs on one task with its stack in
 * PSRAM, created on first use and kept: a new 12 KB internal-RAM stack per job often did not fit
 * once streaming had fragmented internal RAM ("Out of memory" on picking a book), and a task with a
 * PSRAM stack should not delete itself anyway. Jobs run one at a time, in order.
 */
namespace AbsWorker {

/** Queues a job (main loop). False if the queue is full or the task could not start. */
bool post(std::function<void()> job);

}  // namespace AbsWorker

#endif  // INX_HAS_AUDIO_MODULE
