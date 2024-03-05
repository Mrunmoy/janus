#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <gtest/gtest.h>

class LifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfuture::testing::MockSyncController::instance().reset();
        auto sync_ops = cfuture::testing::MockSyncController::instance().get_sync_ops();
        ASSERT_TRUE(cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, payload_arena, &sync_ops));
    }

    void TearDown() override {
        cfuture_pool_destroy(&pool);
    }

    static constexpr uint32_t kCapacity = 4;
    static constexpr size_t kPayloadSize = sizeof(uint32_t);

    cfuture_slot_t slots[kCapacity];
    uint8_t payload_arena[kCapacity * kPayloadSize];
    cfuture_pool_t pool;
};

TEST_F(LifecycleTest, CreateAndFulfill_NormalFlow) {
    cpromise_t promise;
    cfuture_t future;

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    EXPECT_TRUE(cpromise_is_active(&promise));
    EXPECT_EQ(pool.slots[0].ref_count.load(), 2U);

    uint32_t send_val = 0xDEADBEEF;
    cpromise_set_value(&promise, &send_val, 0);

    // Producer handle invalidated
    EXPECT_EQ(promise.pool, nullptr);
    EXPECT_EQ(promise.slot_id, CFUTURE_INVALID_SLOT);

    // Producer dropped ref: refcount is now 1
    EXPECT_EQ(pool.slots[0].ref_count.load(), 1U);
    EXPECT_EQ(pool.slots[0].state.load(), (uint_fast32_t)CFUTURE_STATE_COMPLETED);

    uint32_t recv_val = 0;
    int32_t err = -999;
    EXPECT_TRUE(cfuture_wait_for(&future, 100, &recv_val, &err));

    EXPECT_EQ(recv_val, send_val);
    EXPECT_EQ(err, 0);

    // Consumer handle invalidated
    EXPECT_EQ(future.pool, nullptr);
    EXPECT_EQ(future.slot_id, CFUTURE_INVALID_SLOT);

    // Both parties released: slot recycled to pool
    EXPECT_EQ(pool.slots[0].ref_count.load(), 0U);
    EXPECT_EQ(pool.slots[0].state.load(), (uint_fast32_t)CFUTURE_STATE_IDLE);
    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}

TEST_F(LifecycleTest, SlotRecyclingAllowsReallocation) {
    // Fill all slots
    cpromise_t promises[kCapacity];
    cfuture_t futures[kCapacity];

    for (uint32_t i = 0; i < kCapacity; ++i) {
        ASSERT_TRUE(cfuture_create(&pool, &promises[i], &futures[i]));
    }
    EXPECT_FALSE(cfuture_create(&pool, &promises[0], &futures[0]));

    // Complete slot 2
    uint32_t val = 42;
    cpromise_set_value(&promises[2], &val, 0);
    uint32_t out_val = 0;
    EXPECT_TRUE(cfuture_wait_for(&futures[2], 50, &out_val, nullptr));

    // Slot 2 should be recycled: allocated_mask bit 2 should be 0
    EXPECT_EQ((pool.allocated_mask.load() & (1U << 2)), 0U);

    // Re-allocating should now succeed and reuse slot 2
    cpromise_t new_p;
    cfuture_t new_f;
    ASSERT_TRUE(cfuture_create(&pool, &new_p, &new_f));
    EXPECT_EQ(new_p.slot_id, 2U);
    EXPECT_EQ(new_f.slot_id, 2U);

    // Clean up remaining slots
    for (uint32_t i = 0; i < kCapacity; ++i) {
        if (i == 2) {
            cfuture_abandon(&new_f);
            cpromise_drop(&new_p, 0);
        } else {
            cfuture_abandon(&futures[i]);
            cpromise_drop(&promises[i], 0);
        }
    }
    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}

TEST_F(LifecycleTest, ProducerDropsPromise_PropagatesErrorCode) {
    cpromise_t promise;
    cfuture_t future;

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    int32_t error_reason = 0x1234;
    cpromise_drop(&promise, error_reason);

    int32_t received_error = 0;
    EXPECT_FALSE(cfuture_wait_for(&future, 100, nullptr, &received_error));
    EXPECT_EQ(received_error, error_reason);

    // Slot properly recycled
    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}

TEST_F(LifecycleTest, ConsumerAbandonsFuture_WorkerRecyclesOnCompletion) {
    cpromise_t promise;
    cfuture_t future;

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    // Caller abandons without waiting
    cfuture_abandon(&future);
    EXPECT_EQ(future.pool, nullptr);

    // Caller dropped ref: refcount is 1, state is ABANDONED
    EXPECT_EQ(pool.slots[0].ref_count.load(), 1U);
    EXPECT_EQ(pool.slots[0].state.load(), (uint_fast32_t)CFUTURE_STATE_ABANDONED);

    // Worker checks status: should be inactive
    EXPECT_FALSE(cpromise_is_active(&promise));

    // Worker completes later: worker is the "last one out" and recycles slot
    uint32_t val = 999;
    cpromise_set_value(&promise, &val, 0);

    EXPECT_EQ(pool.slots[0].ref_count.load(), 0U);
    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}

TEST_F(LifecycleTest, ZeroPayloadFuture) {
    cfuture_slot_t zero_slots[2];
    cfuture_pool_t zero_pool;
    auto sync_ops = cfuture::testing::MockSyncController::instance().get_sync_ops();
    ASSERT_TRUE(cfuture_pool_init(&zero_pool, 2, 0, zero_slots, nullptr, &sync_ops));

    cpromise_t p;
    cfuture_t f;
    ASSERT_TRUE(cfuture_create(&zero_pool, &p, &f));

    cpromise_set_value(&p, nullptr, 0);

    int32_t err = -1;
    EXPECT_TRUE(cfuture_wait_for(&f, 100, nullptr, &err));
    EXPECT_EQ(err, 0);
    EXPECT_EQ(zero_pool.allocated_mask.load(), 0U);

    cfuture_pool_destroy(&zero_pool);
}

struct SensorReading {
    float temperature;
    float humidity;
    uint32_t timestamp;
    uint8_t flags;
};

TEST_F(LifecycleTest, StructPayloadIntegrity) {
    cfuture_slot_t struct_slots[2];
    uint8_t arena[2 * sizeof(SensorReading)];
    cfuture_pool_t struct_pool;
    auto sync_ops = cfuture::testing::MockSyncController::instance().get_sync_ops();
    ASSERT_TRUE(cfuture_pool_init(&struct_pool, 2, sizeof(SensorReading), struct_slots, arena, &sync_ops));

    cpromise_t p;
    cfuture_t f;
    ASSERT_TRUE(cfuture_create(&struct_pool, &p, &f));

    SensorReading tx{24.5f, 60.2f, 12345678U, 0x07};
    cpromise_set_value(&p, &tx, 0);

    SensorReading rx{};
    int32_t err = -1;
    EXPECT_TRUE(cfuture_wait_for(&f, 50, &rx, &err));
    EXPECT_EQ(err, 0);

    EXPECT_FLOAT_EQ(rx.temperature, 24.5f);
    EXPECT_FLOAT_EQ(rx.humidity, 60.2f);
    EXPECT_EQ(rx.timestamp, 12345678U);
    EXPECT_EQ(rx.flags, 0x07);

    EXPECT_EQ(struct_pool.allocated_mask.load(), 0U);
    cfuture_pool_destroy(&struct_pool);
}

TEST_F(LifecycleTest, WaitRejectsNullOrInvalidFuture) {
    int32_t err = 0;
    EXPECT_FALSE(cfuture_wait_for(nullptr, 10, nullptr, &err));
    EXPECT_EQ(err, CFUTURE_ERR_INVALID);

    cfuture_t invalid_f{CFUTURE_INVALID_SLOT, nullptr};
    EXPECT_FALSE(cfuture_wait_for(&invalid_f, 10, nullptr, &err));
    EXPECT_EQ(err, CFUTURE_ERR_INVALID);
}
