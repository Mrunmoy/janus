/**
 * @file mock_sync_ops.hpp
 * @brief Mock OSAL Synchronization Controller with Fault Injection
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MOCK_SYNC_OPS_HPP
#define MOCK_SYNC_OPS_HPP

#include "cfuture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace cfuture::testing
{

struct MockEvent
{
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled{false};
    uint32_t set_count{0};
    uint32_t wait_count{0};
    uint32_t reset_count{0};
    uint32_t isr_set_count{0};
};

class MockSyncController
{
  public:
    static constexpr size_t kMaxEvents = 64;

    static MockSyncController &instance()
    {
        static MockSyncController s_instance;
        return s_instance;
    }

    void reset()
    {
        create_count.store(0, std::memory_order_relaxed);
        destroy_count.store(0, std::memory_order_relaxed);
        fail_create_after.store(UINT32_MAX, std::memory_order_relaxed);
        force_create_failure.store(false, std::memory_order_relaxed);
        spurious_wakeups.store(false, std::memory_order_relaxed);
        fail_wait.store(false, std::memory_order_relaxed);

        for (size_t i = 0; i < kMaxEvents; ++i)
        {
            events[i].signaled = false;
            events[i].set_count = 0;
            events[i].wait_count = 0;
            events[i].reset_count = 0;
            events[i].isr_set_count = 0;
        }
        next_event_idx.store(0, std::memory_order_relaxed);
    }

    static void *mockEventCreate()
    {
        MockSyncController &self = instance();
        if (self.force_create_failure.load(std::memory_order_relaxed))
        {
            return nullptr;
        }

        uint32_t current_creates = self.create_count.fetch_add(1, std::memory_order_relaxed);
        if (current_creates >= self.fail_create_after.load(std::memory_order_relaxed))
        {
            return nullptr;
        }

        size_t idx = self.next_event_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx < kMaxEvents)
        {
            self.events[idx].signaled = false;
            return &self.events[idx];
        }
        return nullptr;
    }

    static void mockEventDestroy(void *handle)
    {
        if (!handle)
        {
            return;
        }
        MockSyncController &self = instance();
        self.destroy_count.fetch_add(1, std::memory_order_relaxed);
    }

    static void mockEventSet(void *handle)
    {
        if (!handle)
        {
            return;
        }
        MockEvent *ev = static_cast<MockEvent *>(handle);
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = true;
        ev->set_count++;
        ev->cv.notify_all();
    }

    [[nodiscard]] static bool mockEventWait(void *handle, uint32_t timeout_ms)
    {
        if (!handle)
        {
            return false;
        }

        MockSyncController &self = instance();
        if (self.fail_wait.load(std::memory_order_relaxed))
        {
            return false;
        }

        MockEvent *ev = static_cast<MockEvent *>(handle);
        std::unique_lock<std::mutex> lock(ev->mtx);
        ev->wait_count++;

        if (self.spurious_wakeups.load(std::memory_order_relaxed))
        {
            return false;
        }

        if (ev->signaled)
        {
            return true;
        }

        if (timeout_ms == 0)
        {
            return false;
        }

        if (timeout_ms == UINT32_MAX)
        {
            ev->cv.wait(lock, [&] { return ev->signaled; });
            return ev->signaled;
        }

        return ev->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return ev->signaled; });
    }

    static void mockEventReset(void *handle)
    {
        if (!handle)
        {
            return;
        }
        MockEvent *ev = static_cast<MockEvent *>(handle);
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = false;
        ev->reset_count++;
    }

    static void mockEventSetFromIsr(void *handle)
    {
        if (!handle)
        {
            return;
        }
        MockEvent *ev = static_cast<MockEvent *>(handle);
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = true;
        ev->isr_set_count++;
        ev->cv.notify_all();
    }

    cfuture_sync_ops_t getSyncOps()
    {
        cfuture_sync_ops_t ops{};
        ops.event_create = &MockSyncController::mockEventCreate;
        ops.event_destroy = &MockSyncController::mockEventDestroy;
        ops.event_set = &MockSyncController::mockEventSet;
        ops.event_wait = &MockSyncController::mockEventWait;
        ops.event_reset = &MockSyncController::mockEventReset;
        ops.event_set_from_isr = &MockSyncController::mockEventSetFromIsr;
        return ops;
    }

    std::atomic<uint32_t> create_count{0};
    std::atomic<uint32_t> destroy_count{0};
    std::atomic<size_t> next_event_idx{0};

    std::atomic<uint32_t> fail_create_after{UINT32_MAX};
    std::atomic<bool> force_create_failure{false};
    std::atomic<bool> spurious_wakeups{false};
    std::atomic<bool> fail_wait{false};

    MockEvent events[kMaxEvents];

  private:
    MockSyncController() = default;
};

} // namespace cfuture::testing

#endif // MOCK_SYNC_OPS_HPP
