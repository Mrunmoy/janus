/**
 * @file test_stress_chaos.cpp
 * @brief Randomised multi-threaded chaos test for cfuture.
 *
 * Requesters and workers exchange promises through a small bounded queue while a
 * tiny pool forces constant slot reuse. Every iteration picks a random scenario
 * (long wait, short timeout, abandon, worker drop, duplicated promise, duplicated
 * future, cancel on queue-full) and then replays spent handles against whatever
 * now occupies their old slot. Each scenario asserts exactly what the API contract
 * allows, so a lost wakeup, false timeout, torn payload, cross-talk between
 * occupants, or leaked slot fails the run.
 *
 * Runtime per variant defaults to 1500 ms; override with CFUTURE_STRESS_MS.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"
#include "cfuture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr uint32_t kPoison = 0xDEADDEADU;
constexpr int32_t kSkippedStatus = -7777;
constexpr uint32_t kPayloadWords = 16;

struct Payload
{
    uint32_t words[kPayloadWords];
};

void fillPayload(Payload &p, uint32_t req_id)
{
    for (uint32_t i = 0; i < kPayloadWords; ++i)
    {
        p.words[i] = req_id ^ (i * 0x9E3779B9U);
    }
}

bool payloadMatches(const Payload &p, uint32_t req_id)
{
    for (uint32_t i = 0; i < kPayloadWords; ++i)
    {
        if (p.words[i] != (req_id ^ (i * 0x9E3779B9U)))
        {
            return false;
        }
    }
    return true;
}

enum class WorkerAction : uint8_t
{
    kComplete,
    kCompleteFromIsr,
    kDrop,
    kCompleteTwice, // worker duplicates its own handle and resolves both copies
};

struct Request
{
    cpromise_t promise;
    uint32_t req_id;
    uint32_t work_us;
    int32_t drop_status;
    WorkerAction action;
};

class BoundedQueue
{
  public:
    explicit BoundedQueue(size_t capacity) : m_capacity(capacity)
    {
    }

    [[nodiscard]] bool tryPush(const Request &req)
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_items.size() >= m_capacity)
        {
            return false;
        }
        m_items.push_back(req);
        m_cv.notify_one();
        return true;
    }

    [[nodiscard]] bool pop(Request &out)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait(lock, [&] { return !m_items.empty() || m_closed; });
        if (m_items.empty())
        {
            return false;
        }
        out = m_items.front();
        m_items.pop_front();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_closed = true;
        m_cv.notify_all();
    }

  private:
    size_t m_capacity;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<Request> m_items;
    bool m_closed{false};
};

struct Rng
{
    uint32_t state;

    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    uint32_t below(uint32_t n)
    {
        return next() % n;
    }
};

struct Counters
{
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> pool_full{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> timed_out{0};
    std::atomic<uint64_t> abandoned{0};
    std::atomic<uint64_t> cancelled{0};
    std::atomic<uint64_t> stale_replays{0};
    std::atomic<uint64_t> dup_waits{0};
};

enum class SyncMode
{
    kPosixEvents,
    kPolling,
    kHostileEvents,
};

// A legal-but-nasty OSAL: waits return early without a signal, report signals that
// never happened, or cut the requested timeout short, and there is no event_reset.
// The core may only trust slot state and the PAL clock, so every invariant must hold.
bool hostileEventWait(void *handle, uint32_t timeout_ms)
{
    thread_local Rng rng{0xC0FFEE11U ^
                         (uint32_t)std::hash<std::thread::id>{}(std::this_thread::get_id())};
    const uint32_t roll = rng.below(16);
    if (roll < 4U)
    {
        return false;
    }
    if (roll < 6U)
    {
        return true;
    }
    if (roll < 10U && timeout_ms > 1U && timeout_ms != UINT32_MAX)
    {
        timeout_ms = 1U + rng.below(timeout_ms);
    }
    return cfuture_posix_sync_ops()->event_wait(handle, timeout_ms);
}

const cfuture_sync_ops_t *hostileSyncOps()
{
    static cfuture_sync_ops_t ops = []
    {
        cfuture_sync_ops_t o = *cfuture_posix_sync_ops();
        o.event_wait = &hostileEventWait;
        o.event_reset = nullptr;
        return o;
    }();
    return &ops;
}

uint32_t stressDurationMs()
{
    const char *env = std::getenv("CFUTURE_STRESS_MS");
    if (env)
    {
        const long parsed = std::strtol(env, nullptr, 10);
        if (parsed > 0)
        {
            return static_cast<uint32_t>(parsed);
        }
    }
    return 1500U;
}

} // namespace

class ChaosStressTest : public ::testing::TestWithParam<SyncMode>
{
  protected:
    static constexpr uint32_t kCapacity = 4;
    static constexpr uint32_t kRequesters = 8;
    static constexpr uint32_t kWorkers = 4;
    static constexpr size_t kQueueDepth = 3;
    static constexpr size_t kStaleRing = 16;
    static constexpr uint32_t kLongTimeoutMs = 20000;

    void SetUp() override
    {
        const cfuture_sync_ops_t *ops = nullptr;
        if (GetParam() == SyncMode::kPosixEvents)
        {
            ops = cfuture_posix_sync_ops();
        }
        else if (GetParam() == SyncMode::kHostileEvents)
        {
            ops = hostileSyncOps();
        }
        ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, sizeof(Payload), m_slots, m_arena, ops));

        // Park every slot just below the generation limit so the run crosses the wrap.
        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            m_slots[i].owner.store((uint_fast32_t)(CFUTURE_GEN_MASK - 100U - i)
                                       << CFUTURE_GEN_SHIFT,
                                   std::memory_order_relaxed);
        }
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&m_pool);
    }

    void fail(const std::string &what)
    {
        std::lock_guard<std::mutex> lock(m_fail_mtx);
        if (m_first_failure.empty())
        {
            m_first_failure = what;
        }
        m_stop.store(true, std::memory_order_release);
    }

#define CHAOS_CHECK(cond, what)                                                                    \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            fail(std::string(what) + " [" #cond "] line " + std::to_string(__LINE__));             \
        }                                                                                          \
    } while (0)

    struct StalePair
    {
        cpromise_t promise;
        cfuture_t future;
    };

    // Spent handles must be inert no matter who occupies their old slot now.
    void replayStale(Rng &rng, StalePair pair)
    {
        m_counters.stale_replays.fetch_add(1, std::memory_order_relaxed);
        Payload poison;
        for (uint32_t i = 0; i < kPayloadWords; ++i)
        {
            poison.words[i] = kPoison;
        }

        switch (rng.below(6))
        {
        case 0:
            cpromise_set_value(&pair.promise, &poison, -1);
            break;
        case 1:
            cpromise_drop(&pair.promise, -2);
            break;
        case 2:
        {
            Payload rx{};
            int32_t status = 0;
            const bool ok = cfuture_wait_for(&pair.future, rng.below(2), &rx, &status);
            CHAOS_CHECK(!ok, "stale future produced a result");
            CHAOS_CHECK(status == CFUTURE_ERR_INVALID, "stale future wait status");
            break;
        }
        case 3:
            cfuture_abandon(&pair.future);
            break;
        case 4:
            CHAOS_CHECK(!cfuture_cancel(&pair.promise, &pair.future), "stale pair cancelled");
            break;
        default:
            CHAOS_CHECK(!cpromise_is_active(&pair.promise), "stale promise reported active");
            break;
        }
    }

    void requesterLoop(uint32_t thread_idx)
    {
        Rng rng{0x1234567U + thread_idx * 7919U};
        StalePair stale[kStaleRing]{};
        size_t stale_count = 0;
        uint32_t seq = 0;

        while (!m_stop.load(std::memory_order_acquire))
        {
            cpromise_t promise{};
            cfuture_t future{};
            if (!cfuture_create(&m_pool, &promise, &future))
            {
                m_counters.pool_full.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                continue;
            }
            m_counters.created.fetch_add(1, std::memory_order_relaxed);

            const uint32_t req_id = (thread_idx << 24) | (++seq & 0x00FFFFFFU);
            const StalePair copies{promise, future};
            const uint32_t scenario = rng.below(100);

            Request req{};
            req.promise = promise;
            req.req_id = req_id;
            req.work_us = 0;
            req.drop_status = -(1000 + (int32_t)(req_id % 100U));
            req.action =
                (rng.below(2) == 0U) ? WorkerAction::kComplete : WorkerAction::kCompleteFromIsr;

            bool expect_drop = false;
            bool short_timeout = false;
            bool abandon_after_push = false;
            bool duplicate_promise = false;
            bool duplicate_future = false;

            if (scenario < 35)
            {
                // long wait, worker completes
            }
            else if (scenario < 50)
            {
                req.action = WorkerAction::kDrop;
                expect_drop = true;
            }
            else if (scenario < 70)
            {
                short_timeout = true;
                // Mostly near-instant work so resolve and timeout collide head-on.
                req.work_us = (rng.below(4) == 0U) ? rng.below(3000) : rng.below(60);
            }
            else if (scenario < 80)
            {
                abandon_after_push = true;
                req.work_us = rng.below(60);
            }
            else if (scenario < 90)
            {
                duplicate_promise = true;
            }
            else if (scenario < 95)
            {
                req.action = WorkerAction::kCompleteTwice;
            }
            else
            {
                duplicate_future = true;
            }

            if (!m_queue.tryPush(req))
            {
                // Nobody else ever saw the promise, so cancel has to succeed.
                CHAOS_CHECK(cfuture_cancel(&promise, &future), "cancel of undispatched pair");
                CHAOS_CHECK(promise.pool == nullptr && future.pool == nullptr,
                            "cancel left handles valid");
                m_counters.cancelled.fetch_add(1, std::memory_order_relaxed);
                stale[stale_count++ % kStaleRing] = copies;
                // Let the workers drain; serialising schedulers (valgrind) starve otherwise.
                std::this_thread::yield();
                continue;
            }

            if (duplicate_promise)
            {
                // Second copy may or may not fit; either way exactly one resolution may land.
                (void)m_queue.tryPush(req);
            }

            if (abandon_after_push)
            {
                cfuture_abandon(&future);
                m_counters.abandoned.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const uint32_t timeout_ms = short_timeout ? rng.below(3) : kLongTimeoutMs;

                std::atomic<int> dup_result{0}; // 1 = got result, 2 = rejected
                std::thread dup_waiter;
                if (duplicate_future)
                {
                    m_counters.dup_waits.fetch_add(1, std::memory_order_relaxed);
                    dup_waiter = std::thread(
                        [&, dup = future]() mutable
                        {
                            Payload rx{};
                            int32_t status = 0;
                            const bool ok = cfuture_wait_for(&dup, kLongTimeoutMs, &rx, &status);
                            if (ok)
                            {
                                CHAOS_CHECK(payloadMatches(rx, req_id), "dup waiter payload");
                                dup_result.store(1, std::memory_order_release);
                            }
                            else
                            {
                                CHAOS_CHECK(status == CFUTURE_ERR_INVALID, "dup waiter status");
                                dup_result.store(2, std::memory_order_release);
                            }
                        });
                }

                Payload rx{};
                int32_t status = -999;
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = cfuture_wait_for(&future, timeout_ms, &rx, &status);
                const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count();

                if (duplicate_future)
                {
                    dup_waiter.join();
                    const int other = dup_result.load(std::memory_order_acquire);
                    if (ok)
                    {
                        CHAOS_CHECK(other == 2, "both duplicate futures got the result");
                        CHAOS_CHECK(payloadMatches(rx, req_id), "dup primary payload");
                    }
                    else
                    {
                        CHAOS_CHECK(status == CFUTURE_ERR_INVALID, "dup primary status");
                        CHAOS_CHECK(other == 1, "neither duplicate future got the result");
                    }
                    m_counters.completed.fetch_add(1, std::memory_order_relaxed);
                }
                else if (ok)
                {
                    CHAOS_CHECK(!expect_drop, "drop scenario returned success");
                    CHAOS_CHECK(status == 0, "success status");
                    CHAOS_CHECK(payloadMatches(rx, req_id), "payload cross-talk or tear");
                    m_counters.completed.fetch_add(1, std::memory_order_relaxed);
                }
                else if (status == CFUTURE_ERR_TIMEOUT)
                {
                    CHAOS_CHECK(short_timeout, "false timeout on a long wait");
                    CHAOS_CHECK(elapsed_us >= (int64_t)timeout_ms * 1000,
                                "timed out before the deadline");
                    m_counters.timed_out.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    CHAOS_CHECK(expect_drop, "unexpected failure status " + std::to_string(status));
                    CHAOS_CHECK(status == req.drop_status, "drop status cross-talk");
                    m_counters.dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }

            stale[stale_count++ % kStaleRing] = copies;

            const size_t filled = (stale_count < kStaleRing) ? stale_count : kStaleRing;
            for (uint32_t r = rng.below(3); r > 0 && filled > 0; --r)
            {
                replayStale(rng, stale[rng.below((uint32_t)filled)]);
            }
        }
    }

    void workerLoop(uint32_t thread_idx)
    {
        Rng rng{0xBEEF00U + thread_idx * 104729U};
        Request req{};

        while (m_queue.pop(req))
        {
            if (req.work_us >= 100U)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(req.work_us));
            }
            else
            {
                // A sleep syscall costs more than this whole window; spin instead.
                for (uint32_t spin = 0; spin < req.work_us; ++spin)
                {
                    std::this_thread::yield();
                }
            }

            // Pre-execution cancellation check, as the README tells servicers to do.
            if (rng.below(2) == 0U && !cpromise_is_active(&req.promise))
            {
                cpromise_drop(&req.promise, kSkippedStatus);
                continue;
            }

            Payload tx;
            fillPayload(tx, req.req_id);

            switch (req.action)
            {
            case WorkerAction::kComplete:
                cpromise_set_value(&req.promise, &tx, 0);
                break;
            case WorkerAction::kCompleteFromIsr:
                cpromise_set_value_from_isr(&req.promise, &tx, 0);
                break;
            case WorkerAction::kDrop:
                cpromise_drop(&req.promise, req.drop_status);
                break;
            case WorkerAction::kCompleteTwice:
            {
                cpromise_t again = req.promise;
                cpromise_set_value(&req.promise, &tx, 0);
                Payload poison;
                for (uint32_t i = 0; i < kPayloadWords; ++i)
                {
                    poison.words[i] = kPoison;
                }
                cpromise_set_value(&again, &poison, -3);
                break;
            }
            }
        }
    }

    cfuture_slot_t m_slots[kCapacity];
    uint8_t m_arena[kCapacity * sizeof(Payload)];
    cfuture_pool_t m_pool;

    BoundedQueue m_queue{kQueueDepth};
    Counters m_counters;
    std::atomic<bool> m_stop{false};
    std::mutex m_fail_mtx;
    std::string m_first_failure;
};

TEST_P(ChaosStressTest, RandomisedScenariosHoldEveryInvariant)
{
    std::vector<std::thread> workers;
    std::vector<std::thread> requesters;

    for (uint32_t i = 0; i < kWorkers; ++i)
    {
        workers.emplace_back([this, i] { workerLoop(i); });
    }
    for (uint32_t i = 0; i < kRequesters; ++i)
    {
        requesters.emplace_back([this, i] { requesterLoop(i); });
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(stressDurationMs());
    while (std::chrono::steady_clock::now() < deadline && !m_stop.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    m_stop.store(true, std::memory_order_release);

    for (std::thread &t : requesters)
    {
        t.join();
    }
    m_queue.close();
    for (std::thread &t : workers)
    {
        t.join();
    }

    EXPECT_TRUE(m_first_failure.empty()) << m_first_failure;

    // Every side has let go: nothing may be leaked or left half-owned.
    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_acquire), 0U);
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        EXPECT_EQ(m_slots[i].state.load(std::memory_order_acquire),
                  (uint_fast32_t)CFUTURE_STATE_IDLE);
        EXPECT_EQ(m_slots[i].owner.load(std::memory_order_acquire) & 0x0FU, 0U);
    }

    const uint64_t created = m_counters.created.load();
    const uint64_t accounted = m_counters.completed.load() + m_counters.dropped.load() +
                               m_counters.timed_out.load() + m_counters.abandoned.load() +
                               m_counters.cancelled.load();
    EXPECT_EQ(created, accounted);
    EXPECT_GT(m_counters.completed.load(), 0U);
    EXPECT_GT(m_counters.stale_replays.load(), 0U);

    std::printf("[chaos] created=%llu completed=%llu dropped=%llu timed_out=%llu abandoned=%llu "
                "cancelled=%llu pool_full=%llu stale_replays=%llu dup_waits=%llu\n",
                (unsigned long long)created, (unsigned long long)m_counters.completed.load(),
                (unsigned long long)m_counters.dropped.load(),
                (unsigned long long)m_counters.timed_out.load(),
                (unsigned long long)m_counters.abandoned.load(),
                (unsigned long long)m_counters.cancelled.load(),
                (unsigned long long)m_counters.pool_full.load(),
                (unsigned long long)m_counters.stale_replays.load(),
                (unsigned long long)m_counters.dup_waits.load());
}

INSTANTIATE_TEST_SUITE_P(SyncModes, ChaosStressTest,
                         ::testing::Values(SyncMode::kPosixEvents, SyncMode::kPolling,
                                           SyncMode::kHostileEvents),
                         [](const ::testing::TestParamInfo<SyncMode> &info)
                         {
                             switch (info.param)
                             {
                             case SyncMode::kPosixEvents:
                                 return "PosixEvents";
                             case SyncMode::kPolling:
                                 return "Polling";
                             default:
                                 return "HostileEvents";
                             }
                         });
