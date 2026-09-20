/**
 * @file test_concurrency_stress.cpp
 * @brief High-Contention Multi-Threaded Concurrency Stress Test for cfuture
 *
 * Runs concurrent producer and consumer threads executing 100,000 cycles under
 * heavy slot contention, randomized drops, and timeouts. Verified with ThreadSanitizer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"
#include "cfuture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

struct Payload
{
    uint32_t sequence_id;
    uint32_t thread_id;
    uint32_t checksum;
};

class ThreadSafeChannel
{
  public:
    void push(cfuture_t f)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_items.push_back(f);
        m_cv.notify_one();
    }

    bool pop(cfuture_t &out_f)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait(lock, [this] { return !m_items.empty() || m_closed; });

        if (m_items.empty())
        {
            return false;
        }

        out_f = m_items.front();
        m_items.pop_front();
        return true;
    }

    void close()
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_closed = true;
        m_cv.notify_all();
    }

  private:
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<cfuture_t> m_items;
    bool m_closed{false};
};

} // namespace

class ConcurrencyStressTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const cfuture_sync_ops_t *posix_ops = cfuture_posix_sync_ops();
        ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, sizeof(Payload), m_slots, m_payload_arena,
                                      posix_ops));
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&m_pool);
    }

    static constexpr uint32_t kCapacity = 32;
    static constexpr uint32_t kTotalCycles = 100000;
    static constexpr uint32_t kNumProducers = 4;
    static constexpr uint32_t kNumConsumers = 4;

    cfuture_slot_t m_slots[kCapacity];
    uint8_t m_payload_arena[kCapacity * sizeof(Payload)];
    cfuture_pool_t m_pool;
};

TEST_F(ConcurrencyStressTest, ConcurrentCreateFulfillConsumeCycles)
{
    std::atomic<uint32_t> completed_cycles{0};
    std::atomic<uint32_t> dropped_cycles{0};
    std::atomic<uint32_t> timeout_cycles{0};
    std::atomic<uint32_t> total_created{0};

    ThreadSafeChannel channel;
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    producers.reserve(kNumProducers);
    consumers.reserve(kNumConsumers);

    // Launch consumer threads
    for (uint32_t cid = 0; cid < kNumConsumers; ++cid)
    {
        consumers.emplace_back(
            [&channel, &completed_cycles, &dropped_cycles, &timeout_cycles]()
            {
                cfuture_t f{};
                while (channel.pop(f))
                {
                    Payload rx{};
                    int32_t err = 0;

                    if (cfuture_wait_for(&f, 100, &rx, &err))
                    {
                        EXPECT_EQ(rx.checksum, rx.sequence_id ^ rx.thread_id ^ 0xA5A5A5A5U);
                        completed_cycles.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        if (err == -42)
                        {
                            dropped_cycles.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            timeout_cycles.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
    }

    // Launch producer threads
    for (uint32_t pid = 0; pid < kNumProducers; ++pid)
    {
        producers.emplace_back(
            [this, pid, &channel, &total_created]()
            {
                uint32_t seq = 0;
                while (total_created.fetch_add(1, std::memory_order_relaxed) < kTotalCycles)
                {
                    cpromise_t p{};
                    cfuture_t f{};

                    while (!cfuture_create(&m_pool, &p, &f))
                    {
                        std::this_thread::yield();
                    }

                    channel.push(f);

                    if ((seq % 50) == 0)
                    {
                        cpromise_drop(&p, -42);
                    }
                    else
                    {
                        Payload pl{seq, pid, seq ^ pid ^ 0xA5A5A5A5U};
                        cpromise_set_value(&p, &pl, 0);
                    }

                    ++seq;
                }
            });
    }

    for (auto &t : producers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    channel.close();

    for (auto &t : consumers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // Wait a brief moment to ensure all late tasks unwind
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Zero memory leaks: All slots must be back in IDLE
    // Relaxed is sufficient here: all producer/consumer threads have already
    // been joined above, which establishes happens-before with this thread.
    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);

    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        EXPECT_EQ(m_pool.slots[i].state.load(std::memory_order_relaxed),
                  (uint_fast32_t)CFUTURE_STATE_IDLE);
    }
}

// ── Generation / ownership hardening ────────────────────────────────────────

TEST_F(ConcurrencyStressTest, ConcurrentDuplicateProducers_ExactlyOneWins)
{
    static constexpr uint32_t kIterations = 5000;

    for (uint32_t i = 0; i < kIterations; ++i)
    {
        cpromise_t promise{};
        cfuture_t future{};
        ASSERT_TRUE(cfuture_create(&m_pool, &promise, &future));
        cpromise_t copy_a = promise;
        cpromise_t copy_b = promise;

        std::atomic<bool> go{false};
        auto producer = [&go](cpromise_t *p, uint32_t id)
        {
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            Payload tx{id, id, ~id};
            cpromise_set_value(p, &tx, 0);
        };

        std::thread thread_a(producer, &copy_a, 0xAAAAAAAAU);
        std::thread thread_b(producer, &copy_b, 0xBBBBBBBBU);
        go.store(true, std::memory_order_release);

        Payload rx{};
        int32_t status = -999;
        const bool ok = cfuture_wait_for(&future, 5000, &rx, &status);
        thread_a.join();
        thread_b.join();

        ASSERT_TRUE(ok);
        ASSERT_EQ(status, 0);
        // A torn payload would mean both duplicates wrote the slot.
        ASSERT_TRUE(rx.sequence_id == 0xAAAAAAAAU || rx.sequence_id == 0xBBBBBBBBU);
        ASSERT_EQ(rx.thread_id, rx.sequence_id);
        ASSERT_EQ(rx.checksum, ~rx.sequence_id);
        ASSERT_EQ(m_pool.allocated_mask.load(std::memory_order_acquire), 0U);
    }
}

TEST_F(ConcurrencyStressTest, StaleProducerHammer_NeverReachesLaterOccupants)
{
    static constexpr uint32_t kIterations = 20000;
    static constexpr uint32_t kPoison = 0xDEADDEADU;

    std::mutex spent_mtx;
    cpromise_t spent{};
    std::atomic<bool> stop{false};

    // Replays already-resolved promise handles while their slots are being reused.
    std::thread hammer(
        [&]()
        {
            while (!stop.load(std::memory_order_acquire))
            {
                cpromise_t replay{};
                {
                    std::lock_guard<std::mutex> lock(spent_mtx);
                    replay = spent;
                }
                Payload poison{kPoison, kPoison, kPoison};
                cpromise_set_value(&replay, &poison, -1);
            }
        });

    for (uint32_t i = 0; i < kIterations; ++i)
    {
        cpromise_t promise{};
        cfuture_t future{};
        ASSERT_TRUE(cfuture_create(&m_pool, &promise, &future));
        cpromise_t copy = promise;

        Payload tx{i, 0U, ~i};
        cpromise_set_value(&promise, &tx, 0);
        {
            std::lock_guard<std::mutex> lock(spent_mtx);
            spent = copy;
        }

        Payload rx{};
        int32_t status = -999;
        ASSERT_TRUE(cfuture_wait_for(&future, 5000, &rx, &status));
        ASSERT_EQ(status, 0);
        ASSERT_EQ(rx.sequence_id, i);
        ASSERT_EQ(rx.checksum, ~i);
    }

    stop.store(true, std::memory_order_release);
    hammer.join();
    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_acquire), 0U);
}
