#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <gtest/gtest.h>

struct MotorTelemetry
{
    float rpm;
    float current_amps;
    int32_t fault_code;
};

CFUTURE_DEFINE_TYPED_POOL(Motor, MotorTelemetry, 4)

class TypedPoolTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        cfuture::testing::MockSyncController::instance().reset();
        auto sync_ops = cfuture::testing::MockSyncController::instance().getSyncOps();
        ASSERT_TRUE(
            cfuture_pool_init(&pool, 4, sizeof(MotorTelemetry), slots, payload_arena, &sync_ops));
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&pool);
    }

    cfuture_slot_t slots[4];
    uint8_t payload_arena[4 * sizeof(MotorTelemetry)];
    cfuture_pool_t pool;
};

TEST_F(TypedPoolTest, TypedCreateAndFulfill)
{
    Motor_promise_t promise{};
    Motor_future_t future{};

    ASSERT_TRUE(Motor_create(&pool, &promise, &future));
    EXPECT_TRUE(Motor_promise_is_active(&promise));

    MotorTelemetry tx{3500.0f, 1.45f, 0};
    Motor_promise_set(&promise, &tx, 0);

    MotorTelemetry rx{};
    int32_t err = -1;
    EXPECT_TRUE(Motor_future_wait(&future, 100, &rx, &err));

    EXPECT_FLOAT_EQ(rx.rpm, 3500.0f);
    EXPECT_FLOAT_EQ(rx.current_amps, 1.45f);
    EXPECT_EQ(rx.fault_code, 0);
    EXPECT_EQ(err, 0);

    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TypedPoolTest, TypedDropPropagatesErrorCode)
{
    Motor_promise_t promise{};
    Motor_future_t future{};

    ASSERT_TRUE(Motor_create(&pool, &promise, &future));

    Motor_promise_drop(&promise, -42);

    MotorTelemetry rx{};
    int32_t err = 0;
    EXPECT_FALSE(Motor_future_wait(&future, 100, &rx, &err));
    EXPECT_EQ(err, -42);

    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TypedPoolTest, TypedAbandon)
{
    Motor_promise_t promise{};
    Motor_future_t future{};

    ASSERT_TRUE(Motor_create(&pool, &promise, &future));

    Motor_future_abandon(&future);
    EXPECT_FALSE(Motor_promise_is_active(&promise));

    MotorTelemetry tx{0.0f, 0.0f, 0};
    Motor_promise_set(&promise, &tx, 0);

    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TypedPoolTest, TypedFulfillFromIsr)
{
    Motor_promise_t promise{};
    Motor_future_t future{};
    ASSERT_TRUE(Motor_create(&pool, &promise, &future));

    MotorTelemetry tx{1200.0f, 0.5f, 7};
    Motor_promise_set_from_isr(&promise, &tx, 0);
    EXPECT_EQ(promise.pool, nullptr);

    MotorTelemetry rx{};
    int32_t err = -1;
    EXPECT_TRUE(Motor_future_wait(&future, 100, &rx, &err));
    EXPECT_FLOAT_EQ(rx.rpm, 1200.0f);
    EXPECT_EQ(rx.fault_code, 7);
    EXPECT_EQ(err, 0);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TypedPoolTest, TypedDropFromIsr)
{
    Motor_promise_t promise{};
    Motor_future_t future{};
    ASSERT_TRUE(Motor_create(&pool, &promise, &future));

    Motor_promise_drop_from_isr(&promise, CFUTURE_ERR_DROPPED);

    MotorTelemetry rx{};
    int32_t err = 0;
    EXPECT_FALSE(Motor_future_wait(&future, 100, &rx, &err));
    EXPECT_EQ(err, CFUTURE_ERR_DROPPED);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TypedPoolTest, TypedCancelReleasesUndispatchedPair)
{
    Motor_promise_t promise{};
    Motor_future_t future{};
    ASSERT_TRUE(Motor_create(&pool, &promise, &future));
    EXPECT_NE(promise.generation, 0U);

    EXPECT_TRUE(Motor_cancel(&promise, &future));
    EXPECT_EQ(promise.pool, nullptr);
    EXPECT_EQ(future.pool, nullptr);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(TypedPoolTest, TypedWrappersRejectAPoolWithADifferentPayloadSize)
{
    // A pool sized for a smaller payload: a typed wait would overflow the caller's struct
    // the other way round, and here would read past the slot's arena.
    cfuture_slot_t small_slots[2];
    uint8_t small_arena[2 * sizeof(uint8_t)];
    cfuture_pool_t small_pool;
    ASSERT_TRUE(
        cfuture_pool_init(&small_pool, 2, sizeof(uint8_t), small_slots, small_arena, nullptr));

    Motor_promise_t promise{};
    Motor_future_t future{};
    EXPECT_FALSE(Motor_create(&small_pool, &promise, &future));
    EXPECT_EQ(small_pool.allocated_mask.load(std::memory_order_acquire), 0U);

    // A raw pair smuggled into the typed wrappers is refused without being consumed.
    cpromise_t raw_p{};
    cfuture_t raw_f{};
    ASSERT_TRUE(cfuture_create(&small_pool, &raw_p, &raw_f));
    Motor_future_t smuggled{raw_f.slot_id, raw_f.pool, raw_f.generation};
    MotorTelemetry rx{};
    int32_t status = 0;
    EXPECT_FALSE(Motor_future_wait(&smuggled, 0, &rx, &status));
    EXPECT_EQ(status, CFUTURE_ERR_INVALID);
    EXPECT_NE(smuggled.pool, nullptr);

    EXPECT_TRUE(cfuture_cancel(&raw_p, &raw_f));
    cfuture_pool_destroy(&small_pool);
}
