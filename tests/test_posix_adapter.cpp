/**
 * @file test_posix_adapter.cpp
 * @brief Direct tests of the POSIX cfuture_sync_ops_t adapter, independent of the pool.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

class PosixAdapterTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ops = cfuture_posix_sync_ops();
        ASSERT_NE(m_ops, nullptr);
        m_event = m_ops->event_create();
        ASSERT_NE(m_event, nullptr);
    }

    void TearDown() override
    {
        m_ops->event_destroy(m_event);
    }

    static int64_t elapsedMs(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    }

    const cfuture_sync_ops_t *m_ops{nullptr};
    void *m_event{nullptr};
};

TEST_F(PosixAdapterTest, SetBeforeWaitIsLatchedAndConsumedOnce)
{
    m_ops->event_set(m_event);
    EXPECT_TRUE(m_ops->event_wait(m_event, 0));
    EXPECT_FALSE(m_ops->event_wait(m_event, 0));
}

TEST_F(PosixAdapterTest, ResetClearsLatchedSignal)
{
    m_ops->event_set(m_event);
    m_ops->event_reset(m_event);
    EXPECT_FALSE(m_ops->event_wait(m_event, 0));
}

TEST_F(PosixAdapterTest, TimedWaitExpiresNoEarlierThanRequested)
{
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(m_ops->event_wait(m_event, 40));
    EXPECT_GE(elapsedMs(t0), 40);
}

TEST_F(PosixAdapterTest, TimedWaitWakesOnSet)
{
    std::thread setter(
        [this]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            m_ops->event_set(m_event);
        });

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(m_ops->event_wait(m_event, 5000));
    EXPECT_LT(elapsedMs(t0), 2000);
    setter.join();
}

TEST_F(PosixAdapterTest, InfiniteWaitWakesOnSet)
{
    std::thread setter(
        [this]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            m_ops->event_set_from_isr(m_event);
        });

    EXPECT_TRUE(m_ops->event_wait(m_event, UINT32_MAX));
    setter.join();
}

TEST_F(PosixAdapterTest, NullHandlesAreTolerated)
{
    m_ops->event_set(nullptr);
    m_ops->event_reset(nullptr);
    m_ops->event_destroy(nullptr);
    EXPECT_FALSE(m_ops->event_wait(nullptr, 10));
}

TEST_F(PosixAdapterTest, SetWaitPingPongNeverLosesAWakeup)
{
    static constexpr uint32_t kRounds = 20000;
    std::atomic<uint32_t> produced{0};

    std::thread setter(
        [&]()
        {
            for (uint32_t i = 0; i < kRounds; ++i)
            {
                // Wait for the consumer to have eaten the previous signal.
                while (produced.load(std::memory_order_acquire) != i)
                {
                    std::this_thread::yield();
                }
                m_ops->event_set(m_event);
            }
        });

    for (uint32_t i = 0; i < kRounds; ++i)
    {
        ASSERT_TRUE(m_ops->event_wait(m_event, 10000)) << "lost wakeup at round " << i;
        produced.store(i + 1U, std::memory_order_release);
    }
    setter.join();
}
