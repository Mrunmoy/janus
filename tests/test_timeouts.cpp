#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

class TimeoutsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        cfuture::testing::MockSyncController::instance().reset();
        auto sync_ops = cfuture::testing::MockSyncController::instance().getSyncOps();
        ASSERT_TRUE(
            cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, payload_arena, &sync_ops));
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&pool);
    }

    static constexpr uint32_t kCapacity = 4;
    static constexpr size_t kPayloadSize = sizeof(uint32_t);

    cfuture_slot_t slots[kCapacity];
    uint8_t payload_arena[kCapacity * kPayloadSize];
    cfuture_pool_t pool;
};

TEST_F(TimeoutsTest, NonBlockingZeroTimeout_ReturnsImmediatelyWhenPending)
{
    cpromise_t promise{};
    cfuture_t future{};

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    uint32_t out_val = 0;
    int32_t err = 0;

    auto t0 = std::chrono::steady_clock::now();
    bool ok = cfuture_wait_for(&future, 0, &out_val, &err);
    auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    EXPECT_FALSE(ok);
    EXPECT_EQ(err, CFUTURE_ERR_TIMEOUT);
    EXPECT_LT(elapsed_us, 5000); // 0 us wait returns virtually instantaneously

    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_TIMEOUT);
    EXPECT_FALSE(cpromise_is_active(&promise));

    // Worker completes later
    uint32_t send_val = 1234;
    cpromise_set_value(&promise, &send_val, 0);

    // Slot is now completely recycled by worker
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_IDLE);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TimeoutsTest, TimedWait_WorkerStalls_CallerTimesOutAndUnwinds)
{
    cpromise_t promise{};
    cfuture_t future{};

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    std::thread worker(
        [&promise]()
        {
            // Worker simulates slow hardware / peripheral response
            std::this_thread::sleep_for(std::chrono::milliseconds(60));

            // Caller should have timed out by now
            EXPECT_FALSE(cpromise_is_active(&promise));

            uint32_t result = 999;
            cpromise_set_value(&promise, &result, 0);
        });

    // Caller waits for 20 ms (well before worker completes at 60 ms)
    uint32_t out_val = 0;
    int32_t err = 0;

    auto t0 = std::chrono::steady_clock::now();
    bool ok = cfuture_wait_for(&future, 20, &out_val, &err);
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    EXPECT_FALSE(ok);
    EXPECT_EQ(err, CFUTURE_ERR_TIMEOUT);
    EXPECT_GE(elapsed_ms, 15);
    EXPECT_LT(elapsed_ms, 55);

    // Future handle was invalidated
    EXPECT_EQ(future.pool, nullptr);

    worker.join();

    // Slot should be fully recycled by the worker upon late completion
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_IDLE);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TimeoutsTest, ZeroTimeout_ReturnsSuccessIfAlreadyCompleted)
{
    cpromise_t promise{};
    cfuture_t future{};

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    // Worker completes BEFORE caller waits
    uint32_t send_val = 0x55AA;
    cpromise_set_value(&promise, &send_val, 0);

    // Non-blocking wait (timeout = 0)
    uint32_t recv_val = 0;
    int32_t err = -1;
    EXPECT_TRUE(cfuture_wait_for(&future, 0, &recv_val, &err));

    EXPECT_EQ(recv_val, send_val);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

// ── OSAL wait contract ──────────────────────────────────────────────────────
// In event mode the backend's own timeout is the time base: a false return from a finite
// wait means "the timeout elapsed" (on-target testing showed that timing the wait with
// the PAL clock instead multiplies timeouts whenever that clock is mis-scaled). A forever
// wait cannot time out, so a false return there is a failed wait and is retried.

TEST_F(TimeoutsTest, FailedWaits_InfiniteWaitStillReceivesValue)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    cfuture::testing::MockSyncController::instance().spurious_wakeups.store(
        true, std::memory_order_relaxed);

    std::thread producer(
        [&promise]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint32_t value = 0xC0FFEEU;
            cpromise_set_value(&promise, &value, 0);
        });

    uint32_t out_val = 0;
    int32_t err = -999;
    const bool ok = cfuture_wait_for(&future, UINT32_MAX, &out_val, &err);
    producer.join();

    EXPECT_TRUE(ok);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(out_val, 0xC0FFEEU);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TimeoutsTest, FalseReturnFromFiniteWait_IsTheTimeoutAndIsNotRetried)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    auto &mock = cfuture::testing::MockSyncController::instance();
    mock.fail_wait.store(true, std::memory_order_relaxed);

    int32_t err = 0;
    EXPECT_FALSE(cfuture_wait_for(&future, 60, nullptr, &err));
    EXPECT_EQ(err, CFUTURE_ERR_TIMEOUT);

    // Exactly one wait, handed the caller's timeout unchanged.
    EXPECT_EQ(mock.last_wait_timeout_ms.load(), 60U);

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TimeoutsTest, BackendTimeoutIsNotMultipliedByASlowPalClock)
{
    // The mock really blocks for the requested time; whatever the PAL clock says must not
    // make the library wait again after the backend reported its timeout.
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    int32_t err = 0;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(cfuture_wait_for(&future, 80, nullptr, &err));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    EXPECT_EQ(err, CFUTURE_ERR_TIMEOUT);
    EXPECT_GE(elapsed_ms, 80);
    EXPECT_LT(elapsed_ms, 160);
    EXPECT_EQ(cfuture::testing::MockSyncController::instance().events[0].wait_count, 1U);

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
}

TEST_F(TimeoutsTest, StaleLatchedSignal_IsClearedInsteadOfHotSpinning)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    // Manual-reset style backends keep reporting "signaled" until someone resets them.
    auto &mock = cfuture::testing::MockSyncController::instance();
    mock.getSyncOps().event_set(pool.slots[0].event_handle);

    int32_t err = 0;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(cfuture_wait_for(&future, 50, nullptr, &err));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    EXPECT_EQ(err, CFUTURE_ERR_TIMEOUT);
    EXPECT_GE(elapsed_ms, 50);
    EXPECT_LE(mock.events[0].wait_count, 4U);

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
}

TEST_F(TimeoutsTest, LargestFiniteTimeout_IsNeverHandedToTheBackendAsForever)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    std::thread producer(
        [&promise]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            uint32_t value = 5U;
            cpromise_set_value(&promise, &value, 0);
        });

    uint32_t out_val = 0;
    EXPECT_TRUE(cfuture_wait_for(&future, UINT32_MAX - 1U, &out_val, nullptr));
    producer.join();

    EXPECT_EQ(out_val, 5U);
    EXPECT_NE(cfuture::testing::MockSyncController::instance().last_wait_timeout_ms.load(),
              UINT32_MAX);
}
