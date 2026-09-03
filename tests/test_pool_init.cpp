#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <gtest/gtest.h>

class PoolInitTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        cfuture::testing::MockSyncController::instance().reset();
    }

    static constexpr uint32_t kCapacity = 4;
    static constexpr size_t kPayloadSize = sizeof(uint32_t);

    cfuture_slot_t slots[kCapacity];
    uint8_t payload_arena[kCapacity * kPayloadSize];
    cfuture_pool_t pool;
};

TEST_F(PoolInitTest, RejectsNullPoolPointer)
{
    EXPECT_FALSE(
        cfuture_pool_init(nullptr, kCapacity, kPayloadSize, slots, payload_arena, nullptr));
}

TEST_F(PoolInitTest, RejectsZeroCapacity)
{
    EXPECT_FALSE(cfuture_pool_init(&pool, 0, kPayloadSize, slots, payload_arena, nullptr));
}

TEST_F(PoolInitTest, RejectsCapacityExceedingMaximum)
{
    cfuture_slot_t large_slots[CFUTURE_MAX_CAPACITY + 1]{};
    EXPECT_FALSE(
        cfuture_pool_init(&pool, CFUTURE_MAX_CAPACITY + 1, 0, large_slots, nullptr, nullptr));
}

TEST_F(PoolInitTest, RejectsNullSlotsBuffer)
{
    EXPECT_FALSE(
        cfuture_pool_init(&pool, kCapacity, kPayloadSize, nullptr, payload_arena, nullptr));
}

TEST_F(PoolInitTest, RejectsNullPayloadBufferWhenPayloadSizeNonZero)
{
    EXPECT_FALSE(cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, nullptr, nullptr));
}

TEST_F(PoolInitTest, AcceptsZeroPayloadSizeWithNullPayloadBuffer)
{
    EXPECT_TRUE(cfuture_pool_init(&pool, kCapacity, 0, slots, nullptr, nullptr));
    EXPECT_EQ(pool.capacity, kCapacity);
    EXPECT_EQ(pool.payload_size, 0U);
    EXPECT_EQ(pool.payload_arena, nullptr);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_relaxed), 0U);
}

TEST_F(PoolInitTest, InitializesSlotsAndInvokesEventCreate)
{
    cfuture_sync_ops_t sync_ops = cfuture::testing::MockSyncController::instance().getSyncOps();
    ASSERT_TRUE(cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, payload_arena, &sync_ops));

    EXPECT_EQ(cfuture::testing::MockSyncController::instance().create_count.load(
                  std::memory_order_relaxed),
              kCapacity);

    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        EXPECT_EQ(pool.slots[i].ref_count.load(std::memory_order_relaxed), 0U);
        EXPECT_EQ(pool.slots[i].state.load(std::memory_order_relaxed),
                  (uint_fast32_t)CFUTURE_STATE_IDLE);
        EXPECT_EQ(pool.slots[i].error_code, 0);
        EXPECT_NE(pool.slots[i].event_handle, nullptr);
        EXPECT_EQ(pool.slots[i].payload, payload_arena + (i * kPayloadSize));
    }

    cfuture_pool_destroy(&pool);
    EXPECT_EQ(cfuture::testing::MockSyncController::instance().destroy_count.load(
                  std::memory_order_relaxed),
              kCapacity);
}

TEST_F(PoolInitTest, CreateAllocatesSequentialSlotsUntilFull)
{
    cfuture_sync_ops_t sync_ops = cfuture::testing::MockSyncController::instance().getSyncOps();
    ASSERT_TRUE(cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, payload_arena, &sync_ops));

    cpromise_t promises[kCapacity]{};
    cfuture_t futures[kCapacity]{};

    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        EXPECT_TRUE(cfuture_create(&pool, &promises[i], &futures[i]));
        EXPECT_EQ(promises[i].slot_id, i);
        EXPECT_EQ(promises[i].pool, &pool);
        EXPECT_EQ(futures[i].slot_id, i);
        EXPECT_EQ(futures[i].pool, &pool);

        // Slot state should be PENDING, refcount 2
        EXPECT_EQ(pool.slots[i].ref_count.load(std::memory_order_relaxed), 2U);
        EXPECT_EQ(pool.slots[i].state.load(std::memory_order_relaxed),
                  (uint_fast32_t)CFUTURE_STATE_PENDING);
    }

    // Pool mask should be fully set for kCapacity bits
    uint32_t expected_mask = (1U << kCapacity) - 1U;
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_relaxed), expected_mask);

    // Pool is now full, next allocation should fail
    cpromise_t extra_promise{};
    cfuture_t extra_future{};
    EXPECT_FALSE(cfuture_create(&pool, &extra_promise, &extra_future));

    cfuture_pool_destroy(&pool);
}

TEST_F(PoolInitTest, CreateRejectsNullArguments)
{
    ASSERT_TRUE(cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, payload_arena, nullptr));

    cpromise_t p{};
    cfuture_t f{};

    EXPECT_FALSE(cfuture_create(nullptr, &p, &f));
    EXPECT_FALSE(cfuture_create(&pool, nullptr, &f));
    EXPECT_FALSE(cfuture_create(&pool, &p, nullptr));
}
