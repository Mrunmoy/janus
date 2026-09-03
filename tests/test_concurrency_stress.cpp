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
        std::unique_lock<std::mutex> lock(mtx_);
        items_.push_back(f);
        cv_.notify_one();
    }

    bool pop(cfuture_t &out_f)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !items_.empty() || closed_; });

        if (items_.empty())
        {
            return false;
        }

        out_f = items_.front();
        items_.pop_front();
        return true;
    }

    void close()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<cfuture_t> items_;
    bool closed_{false};
};

} // namespace

class ConcurrencyStressTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const cfuture_sync_ops_t *posix_ops = cfuture_posix_sync_ops();
        ASSERT_TRUE(cfuture_pool_init(&pool, kCapacity, sizeof(Payload), slots, payload_arena,
                                      posix_ops));
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&pool);
    }

    static constexpr uint32_t kCapacity = 32;
    static constexpr uint32_t kTotalCycles = 100000;
    static constexpr uint32_t kNumProducers = 4;
    static constexpr uint32_t kNumConsumers = 4;

    cfuture_slot_t slots[kCapacity];
    uint8_t payload_arena[kCapacity * sizeof(Payload)];
    cfuture_pool_t pool;
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
        consumers.emplace_back([&channel, &completed_cycles, &dropped_cycles, &timeout_cycles]()
        {
            cfuture_t f;
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
        producers.emplace_back([this, pid, &channel, &total_created]()
        {
            uint32_t seq = 0;
            while (total_created.fetch_add(1, std::memory_order_relaxed) < kTotalCycles)
            {
                cpromise_t p;
                cfuture_t f;

                while (!cfuture_create(&pool, &p, &f))
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

    // Zero memory leaks: All slots must be back in IDLE with ref_count == 0
    EXPECT_EQ(pool.allocated_mask.load(), 0U);

    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        EXPECT_EQ(pool.slots[i].ref_count.load(), 0U);
        EXPECT_EQ(pool.slots[i].state.load(), (uint_fast32_t)CFUTURE_STATE_IDLE);
    }
}
