/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <forward_list>
#include <thread>

#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"
#endif

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

namespace {

// Sleeps for (approximately) the given duration with sub-millisecond precision,
// without spinning. Returns false if that is not possible, in which case the
// caller falls back to the spin wait.
class PreciseSleeper {
 public:
  PreciseSleeper() {
#if REX_PLATFORM_WIN32
    // High-resolution waitable timers (Windows 10 1803+) do not depend on the
    // process timer resolution (timeBeginPeriod).
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
#endif
  }
  ~PreciseSleeper() {
#if REX_PLATFORM_WIN32
    if (timer_) {
      CloseHandle(timer_);
    }
#endif
  }
  PreciseSleeper(const PreciseSleeper&) = delete;
  PreciseSleeper& operator=(const PreciseSleeper&) = delete;

  bool Sleep(std::chrono::nanoseconds duration) {
#if REX_PLATFORM_WIN32
    if (!timer_) {
      return false;
    }
    LARGE_INTEGER due;
    // Negative = relative, in 100 ns units.
    due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(duration.count() / 100));
    if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
      return false;
    }
    return WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0;
#else
    std::this_thread::sleep_for(duration);
    return true;
#endif
  }

 private:
#if REX_PLATFORM_WIN32
  HANDLE timer_ = nullptr;
#endif
};

}  // namespace

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    // The spin wait strategy burns 0.1-0.2 ms of pause loops on every wait
    // before even checking the timeout, and the 1 ms KeTimeStampBundle timer
    // makes it wait 1000 times per second (12-15% of a core in practice).
    // Instead, sleep until the next timer is due (at most 1 ms at a time, so a
    // newly queued timer is noticed as quickly as the spin wait's own 1 ms
    // sleeps would) and then poll the ring buffer without blocking.
    constexpr auto kMaxSleep = std::chrono::milliseconds(1);
    PreciseSleeper sleeper;
    bool precise_sleep = true;

    while (!stop_token.stop_requested()) {
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available;
        if (precise_sleep && !wait_queue_.empty()) {
          auto remaining = wait_queue_.front()->due_ - clock::now();
          if (remaining > clock::duration::zero()) {
            precise_sleep = sleeper.Sleep(std::min<clock::duration>(remaining, kMaxSleep));
          }
          available = claim_strategy_.last_published_after(next_sequence - 1);
        } else {
          available = claim_strategy_.wait_until_published(
              next_sequence, next_sequence - 1,
              wait_queue_.empty() ? clock::time_point::max() : wait_queue_.front()->due_);
        }

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  dp::spin_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
