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
        auto sync_ops = cfuture::testing::MockSyncController::instance().get_sync_ops();
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
    Motor_promise_t promise;
    Motor_future_t future;

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

    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}

TEST_F(TypedPoolTest, TypedDropPropagatesErrorCode)
{
    Motor_promise_t promise;
    Motor_future_t future;

    ASSERT_TRUE(Motor_create(&pool, &promise, &future));

    Motor_promise_drop(&promise, -42);

    MotorTelemetry rx{};
    int32_t err = 0;
    EXPECT_FALSE(Motor_future_wait(&future, 100, &rx, &err));
    EXPECT_EQ(err, -42);

    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}

TEST_F(TypedPoolTest, TypedAbandon)
{
    Motor_promise_t promise;
    Motor_future_t future;

    ASSERT_TRUE(Motor_create(&pool, &promise, &future));

    Motor_future_abandon(&future);
    EXPECT_FALSE(Motor_promise_is_active(&promise));

    MotorTelemetry tx{0.0f, 0.0f, 0};
    Motor_promise_set(&promise, &tx, 0);

    EXPECT_EQ(pool.allocated_mask.load(), 0U);
}
