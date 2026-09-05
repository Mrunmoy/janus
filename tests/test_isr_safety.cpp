#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <gtest/gtest.h>

class IsrSafetyTest : public ::testing::Test
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

TEST_F(IsrSafetyTest, IsrFulfill_InvokesIsrSyncHookAndDeliversPayload)
{
    cpromise_t promise{};
    cfuture_t future{};

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    // Simulated ISR (e.g. DMA complete interrupt)
    uint32_t isr_data = 0xCAFEBABE;
    cpromise_set_value_from_isr(&promise, &isr_data, 0);

    // Verify DI event_set_from_isr was called
    EXPECT_EQ(cfuture::testing::MockSyncController::instance().events[0].isr_set_count, 1U);

    uint32_t out_val = 0;
    int32_t err = -1;
    EXPECT_TRUE(cfuture_wait_for(&future, 50, &out_val, &err));

    EXPECT_EQ(out_val, isr_data);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(IsrSafetyTest, IsrDrop_InvokesIsrSyncHookAndPropagatesError)
{
    cpromise_t promise{};
    cfuture_t future{};

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    int32_t isr_error_code = -110;
    cpromise_drop_from_isr(&promise, isr_error_code);

    EXPECT_EQ(cfuture::testing::MockSyncController::instance().events[0].isr_set_count, 1U);

    int32_t out_err = 0;
    EXPECT_FALSE(cfuture_wait_for(&future, 50, nullptr, &out_err));
    EXPECT_EQ(out_err, isr_error_code);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(IsrSafetyTest, IsrFulfill_AfterCallerTimeout_SafelyRecyclesSlot)
{
    cpromise_t promise{};
    cfuture_t future{};

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    // Caller times out immediately
    int32_t err = 0;
    EXPECT_FALSE(cfuture_wait_for(&future, 0, nullptr, &err));
    EXPECT_EQ(err, CFUTURE_ERR_TIMEOUT);

    // Caller timed out -> state is TIMEOUT
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_TIMEOUT);

    // Hardware ISR fires late
    uint32_t isr_data = 0x12345678;
    cpromise_set_value_from_isr(&promise, &isr_data, 0);

    // ISR recycles slot to IDLE
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_IDLE);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}
