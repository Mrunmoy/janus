/**
 * @file test_error_injection.cpp
 * @brief Error Case, Fault Injection, and Safe Recovery Unit Tests
 *
 * Verifies system behavior under OS resource exhaustion, handle corruption,
 * double operations, saturation recovery, and worker crashes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <gtest/gtest.h>

namespace
{

class ErrorInjectionTest : public ::testing::Test
{
  protected:
    static constexpr uint32_t kCapacity = 8;
    static constexpr uint32_t kPayloadSize = sizeof(uint32_t);

    void SetUp() override
    {
        cfuture::testing::MockSyncController::instance().reset();
        m_sync_ops = cfuture::testing::MockSyncController::instance().getSyncOps();
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&m_pool);
    }

    cfuture_sync_ops_t m_sync_ops;
    cfuture_slot_t m_slots[kCapacity];
    uint32_t m_payload_arena[kCapacity];
    cfuture_pool_t m_pool;
};

TEST_F(ErrorInjectionTest, EventCreateFailure_RollsBackAllocatedEventsWithoutLeak)
{
    // Simulate OS failure on the 4th event allocation
    cfuture::testing::MockSyncController::instance().fail_create_after.store(
        3, std::memory_order_relaxed);

    EXPECT_FALSE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                   reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    // Must have attempted 4 creates (3 succeeded, 1 failed)
    EXPECT_EQ(cfuture::testing::MockSyncController::instance().create_count.load(
                  std::memory_order_relaxed),
              4U);

    // All 3 successfully created events must have been rolled back and destroyed
    EXPECT_EQ(cfuture::testing::MockSyncController::instance().destroy_count.load(
                  std::memory_order_relaxed),
              3U);

    // Pool structure must be sanitized
    EXPECT_EQ(m_pool.slots, nullptr);
    EXPECT_EQ(m_pool.capacity, 0U);
    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, EventCreateFailure_ImmediateFailureRejection)
{
    cfuture::testing::MockSyncController::instance().force_create_failure.store(
        true, std::memory_order_relaxed);

    EXPECT_FALSE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                   reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    EXPECT_EQ(cfuture::testing::MockSyncController::instance().destroy_count.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(m_pool.slots, nullptr);
}

TEST_F(ErrorInjectionTest, NullPointerValidation_AllAPIsHandleGracefully)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    int32_t err = 0;
    uint32_t val = 42;

    // NULL handles to create
    EXPECT_FALSE(cfuture_create(nullptr, &p, &f));
    EXPECT_FALSE(cfuture_create(&m_pool, nullptr, &f));
    EXPECT_FALSE(cfuture_create(&m_pool, &p, nullptr));

    // NULL handles to wait
    EXPECT_FALSE(cfuture_wait_for(nullptr, 10, &val, &err));
    EXPECT_EQ(err, CFUTURE_ERR_INVALID);

    // NULL handles to abandon
    cfuture_abandon(nullptr);

    // NULL handles to promise operations
    EXPECT_FALSE(cpromise_is_active(nullptr));
    cpromise_set_value(nullptr, &val, 0);
    cpromise_drop(nullptr, -1);
    cpromise_set_value_from_isr(nullptr, &val, 0);
    cpromise_drop_from_isr(nullptr, -1);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, OutOfBoundsSlotRejection)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    // Corrupted future with slot_id >= capacity
    cfuture_t corrupted_future = {99, &m_pool};
    int32_t err = 0;
    uint32_t val = 0;

    EXPECT_FALSE(cfuture_wait_for(&corrupted_future, 10, &val, &err));
    EXPECT_EQ(err, CFUTURE_ERR_INVALID);

    cfuture_abandon(&corrupted_future);

    // Corrupted promise with slot_id >= capacity
    cpromise_t corrupted_promise = {99, &m_pool};
    EXPECT_FALSE(cpromise_is_active(&corrupted_promise));
    cpromise_set_value(&corrupted_promise, &val, 0);
    cpromise_drop(&corrupted_promise, -1);
    cpromise_set_value_from_isr(&corrupted_promise, &val, 0);
    cpromise_drop_from_isr(&corrupted_promise, -1);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, InvalidatedHandleRejection)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cfuture_t invalid_f = {CFUTURE_INVALID_SLOT, nullptr};
    int32_t err = 0;
    uint32_t val = 0;

    EXPECT_FALSE(cfuture_wait_for(&invalid_f, 10, &val, &err));
    EXPECT_EQ(err, CFUTURE_ERR_INVALID);

    cpromise_t invalid_p = {CFUTURE_INVALID_SLOT, nullptr};
    EXPECT_FALSE(cpromise_is_active(&invalid_p));
}

TEST_F(ErrorInjectionTest, DoubleWait_FailsGracefully)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    uint32_t tx = 100;
    cpromise_set_value(&p, &tx, 0);

    uint32_t rx = 0;
    int32_t err = -1;

    // First wait succeeds
    EXPECT_TRUE(cfuture_wait_for(&f, 10, &rx, &err));
    EXPECT_EQ(rx, 100U);
    EXPECT_EQ(err, 0);

    // Second wait on same future must fail gracefully
    int32_t err2 = 0;
    EXPECT_FALSE(cfuture_wait_for(&f, 10, &rx, &err2));
    EXPECT_EQ(err2, CFUTURE_ERR_INVALID);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, DoubleFulfill_SafelyNoOps)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    uint32_t tx1 = 111;
    cpromise_set_value(&p, &tx1, 0);

    // Second fulfill attempt on invalidated promise handle
    uint32_t tx2 = 222;
    cpromise_set_value(&p, &tx2, 0);

    uint32_t rx = 0;
    int32_t err = -1;
    EXPECT_TRUE(cfuture_wait_for(&f, 10, &rx, &err));
    EXPECT_EQ(rx, 111U);
    EXPECT_EQ(err, 0);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, DoubleAbandon_SafelyNoOps)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    // First abandon
    cfuture_abandon(&f);

    // Second abandon on same handle
    cfuture_abandon(&f);

    // Worker drops and cleans up
    cpromise_drop(&p, 0);
    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, DoubleDrop_SafelyNoOps)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    // First drop
    cpromise_drop(&p, -10);

    // Second drop on same handle
    cpromise_drop(&p, -20);

    uint32_t rx = 0;
    int32_t err = 0;
    EXPECT_FALSE(cfuture_wait_for(&f, 10, &rx, &err));
    EXPECT_EQ(err, -10);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, PoolSaturation_SelfHealingRecoveryAfterConsume)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t promises[kCapacity]{};
    cfuture_t futures[kCapacity]{};

    // Saturate pool
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        ASSERT_TRUE(cfuture_create(&m_pool, &promises[i], &futures[i]));
    }

    // Allocation must fail when saturated
    cpromise_t extra_p{};
    cfuture_t extra_f{};
    EXPECT_FALSE(cfuture_create(&m_pool, &extra_p, &extra_f));

    // Free slot 2 by fulfilling and consuming
    uint32_t val = 2026;
    cpromise_set_value(&promises[2], &val, 0);

    uint32_t out_val = 0;
    EXPECT_TRUE(cfuture_wait_for(&futures[2], 10, &out_val, nullptr));
    EXPECT_EQ(out_val, 2026U);

    // Pool immediately self-heals: allocation succeeds and reclaims slot 2
    ASSERT_TRUE(cfuture_create(&m_pool, &extra_p, &extra_f));
    EXPECT_EQ(extra_p.slot_id, 2U);

    // Clean up all remaining slots
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        if (i == 2)
        {
            cfuture_abandon(&extra_f);
            cpromise_drop(&extra_p, 0);
        }
        else
        {
            cfuture_abandon(&futures[i]);
            cpromise_drop(&promises[i], 0);
        }
    }

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, WorkerDrop_PropagatesCustomErrorAndRecyclesSlot)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    // Simulate worker peripheral I/O failure
    constexpr int32_t kPeripheralIoError = -5; // -EIO
    cpromise_drop(&p, kPeripheralIoError);

    uint32_t rx = 999;
    int32_t err = 0;
    EXPECT_FALSE(cfuture_wait_for(&f, 10, &rx, &err));
    EXPECT_EQ(err, kPeripheralIoError);

    // Slot bit must be recycled back to pool
    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, WorkerDrop_FromISR_RecyclesSlot)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    constexpr int32_t kIsrDmaError = -16; // -EBUSY
    cpromise_drop_from_isr(&p, kIsrDmaError);

    int32_t err = 0;
    EXPECT_FALSE(cfuture_wait_for(&f, 10, nullptr, &err));
    EXPECT_EQ(err, kIsrDmaError);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(ErrorInjectionTest, SpuriousWakeup_ResilientWait)
{
    ASSERT_TRUE(cfuture_pool_init(&m_pool, kCapacity, kPayloadSize, m_slots,
                                  reinterpret_cast<uint8_t *>(m_payload_arena), &m_sync_ops));

    cpromise_t p{};
    cfuture_t f{};
    ASSERT_TRUE(cfuture_create(&m_pool, &p, &f));

    // Fulfill before wait
    uint32_t tx = 777;
    cpromise_set_value(&p, &tx, 0);

    // Inject spurious wakeup in OSAL layer
    cfuture::testing::MockSyncController::instance().spurious_wakeups.store(
        true, std::memory_order_relaxed);

    uint32_t rx = 0;
    int32_t err = -1;
    // Consumer should still consume the completed payload without error
    EXPECT_TRUE(cfuture_wait_for(&f, 10, &rx, &err));
    EXPECT_EQ(rx, 777U);
    EXPECT_EQ(err, 0);

    EXPECT_EQ(m_pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

} // namespace
