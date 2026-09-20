/**
 * @file test_pal_fallback.cpp
 * @brief cfuture_wait_for() behaviour when the PAL clock is not a real millisecond clock.
 *
 * Models a port whose PAL clock merely counts calls (Cortex-M with no tick linked) or is
 * mis-scaled, while the injected OSAL backend blocks for real: in event mode the backend's
 * timeout is the time base, so the wait must not be multiplied. This binary overrides a
 * weak PAL symbol, so it must stay separate from the other suites.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cfuture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

extern "C"
{
    // Same algorithm as the Cortex-M fallback in src/cfuture_pal.c.
    uint32_t cfuture_pal_time_ms(void)
    {
        static std::atomic<uint32_t> s_calls{0};
        return s_calls.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }
}

namespace
{

// An honest backend: blocks for the requested time unless signaled, auto-reset.
struct HonestEvent
{
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled{false};
    uint32_t wait_calls{0};
};

HonestEvent g_events[4];
size_t g_next_event = 0;

void *honestCreate()
{
    return &g_events[g_next_event++ % 4];
}

void honestDestroy(void *)
{
}

void honestSet(void *handle)
{
    HonestEvent *ev = static_cast<HonestEvent *>(handle);
    std::lock_guard<std::mutex> lock(ev->mtx);
    ev->signaled = true;
    ev->cv.notify_all();
}

bool honestWait(void *handle, uint32_t timeout_ms)
{
    HonestEvent *ev = static_cast<HonestEvent *>(handle);
    std::unique_lock<std::mutex> lock(ev->mtx);
    ev->wait_calls++;
    const bool ok =
        ev->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return ev->signaled; });
    ev->signaled = false;
    return ok;
}

int64_t elapsedMs(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
        .count();
}

} // namespace

class PalFallbackTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        g_next_event = 0;
        for (HonestEvent &ev : g_events)
        {
            ev.signaled = false;
            ev.wait_calls = 0;
        }

        cfuture_sync_ops_t ops{};
        ops.event_create = &honestCreate;
        ops.event_destroy = &honestDestroy;
        ops.event_set = &honestSet;
        ops.event_wait = &honestWait;
        ASSERT_TRUE(cfuture_pool_init(&pool, 1, sizeof(uint32_t), slots, arena, &ops));
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&pool);
    }

    cfuture_slot_t slots[1];
    uint8_t arena[sizeof(uint32_t)];
    cfuture_pool_t pool;
};

TEST_F(PalFallbackTest, TimeoutIsBoundedByTheBackendNotMultiplied)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    int32_t status = 0;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(cfuture_wait_for(&future, 200, nullptr, &status));
    const int64_t elapsed = elapsedMs(t0);

    EXPECT_EQ(status, CFUTURE_ERR_TIMEOUT);
    EXPECT_GE(elapsed, 200);
    EXPECT_LT(elapsed, 700) << "a call-counting clock must not multiply the wait";

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
}

TEST_F(PalFallbackTest, ValueIsStillDeliveredPromptly)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    std::thread producer(
        [&promise]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            uint32_t value = 0xFEEDU;
            cpromise_set_value(&promise, &value, 0);
        });

    uint32_t out = 0;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(cfuture_wait_for(&future, 5000, &out, nullptr));
    EXPECT_LT(elapsedMs(t0), 1500);
    producer.join();
    EXPECT_EQ(out, 0xFEEDU);
}

TEST_F(PalFallbackTest, StaleSignalNeitherShortensNorMultipliesTheWait)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    honestSet(pool.slots[0].event_handle); // stale: nothing resolved

    int32_t status = 0;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(cfuture_wait_for(&future, 100, nullptr, &status));
    const int64_t elapsed = elapsedMs(t0);

    EXPECT_EQ(status, CFUTURE_ERR_TIMEOUT);
    EXPECT_GE(elapsed, 100);
    EXPECT_LT(elapsed, 600);

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
}

TEST(PalFallbackPollingTest, PollingModeStillTerminates)
{
    cfuture_slot_t slots[1];
    uint8_t arena[sizeof(uint32_t)];
    cfuture_pool_t pool;
    ASSERT_TRUE(cfuture_pool_init(&pool, 1, sizeof(uint32_t), slots, arena, nullptr));

    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    int32_t status = 0;
    EXPECT_FALSE(cfuture_wait_for(&future, 50, nullptr, &status));
    EXPECT_EQ(status, CFUTURE_ERR_TIMEOUT);

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
    cfuture_pool_destroy(&pool);
}
