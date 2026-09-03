#ifndef MOCK_SYNC_OPS_HPP
#define MOCK_SYNC_OPS_HPP

#include "cfuture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace cfuture::testing {

struct MockEvent {
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled{false};
    uint32_t set_count{0};
    uint32_t wait_count{0};
    uint32_t reset_count{0};
    uint32_t isr_set_count{0};
};

class MockSyncController {
public:
    static constexpr size_t kMaxEvents = 64;

    static MockSyncController &instance() {
        static MockSyncController s_instance;
        return s_instance;
    }

    void reset() {
        create_count.store(0);
        destroy_count.store(0);
        for (size_t i = 0; i < kMaxEvents; ++i) {
            events[i].signaled = false;
            events[i].set_count = 0;
            events[i].wait_count = 0;
            events[i].reset_count = 0;
            events[i].isr_set_count = 0;
        }
        next_event_idx.store(0);
    }

    static void *mock_event_create() {
        auto &self = instance();
        self.create_count.fetch_add(1, std::memory_order_relaxed);
        size_t idx = self.next_event_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx < kMaxEvents) {
            self.events[idx].signaled = false;
            return &self.events[idx];
        }
        return nullptr;
    }

    static void mock_event_destroy(void *handle) {
        if (!handle) return;
        auto &self = instance();
        self.destroy_count.fetch_add(1, std::memory_order_relaxed);
    }

    static void mock_event_set(void *handle) {
        if (!handle) return;
        auto *ev = static_cast<MockEvent *>(handle);
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = true;
        ev->set_count++;
        ev->cv.notify_all();
    }

    static bool mock_event_wait(void *handle, uint32_t timeout_ms) {
        if (!handle) return false;
        auto *ev = static_cast<MockEvent *>(handle);
        std::unique_lock<std::mutex> lock(ev->mtx);
        ev->wait_count++;
        if (ev->signaled) {
            return true;
        }
        if (timeout_ms == 0) {
            return false;
        }
        if (timeout_ms == UINT32_MAX) {
            ev->cv.wait(lock, [&] { return ev->signaled; });
            return ev->signaled;
        }
        return ev->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return ev->signaled; });
    }

    static void mock_event_reset(void *handle) {
        if (!handle) return;
        auto *ev = static_cast<MockEvent *>(handle);
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = false;
        ev->reset_count++;
    }

    static void mock_event_set_from_isr(void *handle) {
        if (!handle) return;
        auto *ev = static_cast<MockEvent *>(handle);
        std::lock_guard<std::mutex> lock(ev->mtx);
        ev->signaled = true;
        ev->isr_set_count++;
        ev->cv.notify_all();
    }

    cfuture_sync_ops_t get_sync_ops() {
        cfuture_sync_ops_t ops;
        ops.event_create = &MockSyncController::mock_event_create;
        ops.event_destroy = &MockSyncController::mock_event_destroy;
        ops.event_set = &MockSyncController::mock_event_set;
        ops.event_wait = &MockSyncController::mock_event_wait;
        ops.event_reset = &MockSyncController::mock_event_reset;
        ops.event_set_from_isr = &MockSyncController::mock_event_set_from_isr;
        return ops;
    }

    std::atomic<uint32_t> create_count{0};
    std::atomic<uint32_t> destroy_count{0};
    std::atomic<size_t> next_event_idx{0};
    MockEvent events[kMaxEvents];

private:
    MockSyncController() = default;
};

} // namespace cfuture::testing

#endif // MOCK_SYNC_OPS_HPP
