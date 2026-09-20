#include "cfuture.h"
#include "mock_sync_ops.hpp"

#include <gtest/gtest.h>

class GenerationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        cfuture::testing::MockSyncController::instance().reset();
        cfuture_sync_ops_t sync_ops = cfuture::testing::MockSyncController::instance().getSyncOps();
        ASSERT_TRUE(
            cfuture_pool_init(&pool, kCapacity, kPayloadSize, slots, payload_arena, &sync_ops));
    }

    void TearDown() override
    {
        cfuture_pool_destroy(&pool);
    }

    // Runs one full create/fulfil/consume cycle so the slot is recycled.
    void cycleOnce(uint32_t value)
    {
        cpromise_t promise{};
        cfuture_t future{};
        ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
        cpromise_set_value(&promise, &value, 0);
        uint32_t out = 0;
        ASSERT_TRUE(cfuture_wait_for(&future, 0, &out, nullptr));
        ASSERT_EQ(out, value);
    }

    static constexpr uint32_t kCapacity = 1;
    static constexpr size_t kPayloadSize = sizeof(uint32_t);

    cfuture_slot_t slots[kCapacity];
    uint8_t payload_arena[kCapacity * kPayloadSize];
    cfuture_pool_t pool;
};

TEST_F(GenerationTest, HandlesCarryMatchingNonZeroGeneration)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    EXPECT_NE(promise.generation, 0U);
    EXPECT_EQ(promise.generation, future.generation);

    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
    EXPECT_EQ(promise.generation, 0U);
    cfuture_abandon(&future);
    EXPECT_EQ(future.generation, 0U);
}

TEST_F(GenerationTest, GenerationAdvancesWhenSlotIsRecycled)
{
    cpromise_t first_p{};
    cfuture_t first_f{};
    ASSERT_TRUE(cfuture_create(&pool, &first_p, &first_f));
    const uint32_t first_gen = first_p.generation;
    cpromise_drop(&first_p, CFUTURE_ERR_DROPPED);
    cfuture_abandon(&first_f);

    cpromise_t second_p{};
    cfuture_t second_f{};
    ASSERT_TRUE(cfuture_create(&pool, &second_p, &second_f));
    EXPECT_EQ(second_p.slot_id, 0U);
    EXPECT_NE(second_p.generation, first_gen);

    cpromise_drop(&second_p, CFUTURE_ERR_DROPPED);
    cfuture_abandon(&second_f);
}

TEST_F(GenerationTest, GenerationWrapSkipsZero)
{
    // Park the idle slot on the last generation so the next recycle wraps.
    pool.slots[0].owner.store((uint_fast32_t)CFUTURE_GEN_MASK << CFUTURE_GEN_SHIFT,
                              std::memory_order_relaxed);

    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    EXPECT_EQ(promise.generation, CFUTURE_GEN_MASK);
    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
    cfuture_abandon(&future);

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    EXPECT_EQ(promise.generation, 1U);
    cpromise_drop(&promise, CFUTURE_ERR_DROPPED);
    cfuture_abandon(&future);
}

TEST_F(GenerationTest, StalePromiseCopyCannotCompleteNextOccupant)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    cpromise_t stale = promise;

    uint32_t value = 1U;
    cpromise_set_value(&promise, &value, 0);
    ASSERT_TRUE(cfuture_wait_for(&future, 0, &value, nullptr));

    cpromise_t next_p{};
    cfuture_t next_f{};
    ASSERT_TRUE(cfuture_create(&pool, &next_p, &next_f));
    ASSERT_EQ(next_p.slot_id, stale.slot_id);

    EXPECT_FALSE(cpromise_is_active(&stale));
    uint32_t poison = 0xBADBADU;
    cpromise_set_value(&stale, &poison, 0);

    EXPECT_EQ(stale.pool, nullptr);
    EXPECT_TRUE(cpromise_is_active(&next_p));
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_PENDING);

    uint32_t real = 42U;
    cpromise_set_value(&next_p, &real, 0);
    uint32_t out = 0;
    EXPECT_TRUE(cfuture_wait_for(&next_f, 0, &out, nullptr));
    EXPECT_EQ(out, 42U);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(GenerationTest, DuplicatePromiseCopyCannotOverwriteOrDoubleRelease)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    cpromise_t duplicate = promise;

    uint32_t first = 7U;
    cpromise_set_value(&promise, &first, 0);

    uint32_t second = 9U;
    cpromise_set_value(&duplicate, &second, 0);

    // Consumer still owns the slot: the duplicate must not have released it.
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 1U);

    uint32_t out = 0;
    int32_t status = -999;
    EXPECT_TRUE(cfuture_wait_for(&future, 0, &out, &status));
    EXPECT_EQ(out, 7U);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(GenerationTest, StaleFutureCopyIsRejected)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    cfuture_t stale = future;

    uint32_t value = 1U;
    cpromise_set_value(&promise, &value, 0);
    ASSERT_TRUE(cfuture_wait_for(&future, 0, &value, nullptr));

    cpromise_t next_p{};
    cfuture_t next_f{};
    ASSERT_TRUE(cfuture_create(&pool, &next_p, &next_f));

    int32_t status = 0;
    EXPECT_FALSE(cfuture_wait_for(&stale, 0, nullptr, &status));
    EXPECT_EQ(status, CFUTURE_ERR_INVALID);

    // The live pair is untouched by the stale wait.
    EXPECT_TRUE(cpromise_is_active(&next_p));

    cpromise_drop(&next_p, CFUTURE_ERR_DROPPED);
    cfuture_abandon(&next_f);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(GenerationTest, DuplicateFutureAbandonDoesNotStealProducerHold)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    cfuture_t duplicate = future;

    cfuture_abandon(&future);
    cfuture_abandon(&duplicate);

    // Producer has not resolved yet, so the slot must still be allocated.
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 1U);
    EXPECT_FALSE(cpromise_is_active(&promise));

    uint32_t value = 5U;
    cpromise_set_value(&promise, &value, 0);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(GenerationTest, ForgedHandleToUnallocatedSlotIsRejected)
{
    // Correct slot index and current generation, but nobody ever allocated the slot.
    const uint32_t idle_gen =
        (uint32_t)(pool.slots[0].owner.load(std::memory_order_acquire) >> CFUTURE_GEN_SHIFT);
    cpromise_t forged_p{0, &pool, idle_gen};
    cfuture_t forged_f{0, &pool, idle_gen};
    cpromise_t forged_p2 = forged_p;
    cfuture_t forged_f2 = forged_f;

    EXPECT_FALSE(cpromise_is_active(&forged_p));
    EXPECT_FALSE(cfuture_cancel(&forged_p2, &forged_f2));

    uint32_t poison = 0xBADBADU;
    cpromise_set_value(&forged_p, &poison, 0);
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_IDLE);

    int32_t status = 0;
    EXPECT_FALSE(cfuture_wait_for(&forged_f, 0, nullptr, &status));
    EXPECT_EQ(status, CFUTURE_ERR_INVALID);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);

    cycleOnce(21U);
}

// ── cfuture_cancel ──────────────────────────────────────────────────────────

TEST_F(GenerationTest, CancelReleasesUndispatchedPair)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));

    EXPECT_TRUE(cfuture_cancel(&promise, &future));

    EXPECT_EQ(promise.pool, nullptr);
    EXPECT_EQ(promise.slot_id, CFUTURE_INVALID_SLOT);
    EXPECT_EQ(future.pool, nullptr);
    EXPECT_EQ(future.slot_id, CFUTURE_INVALID_SLOT);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(pool.slots[0].state.load(std::memory_order_acquire),
              (uint_fast32_t)CFUTURE_STATE_IDLE);

    cycleOnce(11U);
}

TEST_F(GenerationTest, CancelFailsOnceEitherSideHasActed)
{
    cpromise_t promise{};
    cfuture_t future{};
    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    cpromise_t worker_copy = promise;

    uint32_t value = 3U;
    cpromise_set_value(&worker_copy, &value, 0);

    EXPECT_FALSE(cfuture_cancel(&promise, &future));

    // A failed cancel leaves the future usable.
    uint32_t out = 0;
    EXPECT_TRUE(cfuture_wait_for(&future, 0, &out, nullptr));
    EXPECT_EQ(out, 3U);
    EXPECT_EQ(pool.allocated_mask.load(std::memory_order_acquire), 0U);
}

TEST_F(GenerationTest, CancelRejectsInvalidAndMismatchedHandles)
{
    cpromise_t promise{};
    cfuture_t future{};
    EXPECT_FALSE(cfuture_cancel(nullptr, nullptr));
    EXPECT_FALSE(cfuture_cancel(&promise, &future));

    ASSERT_TRUE(cfuture_create(&pool, &promise, &future));
    cfuture_t wrong_gen = future;
    wrong_gen.generation = future.generation + 1U;
    EXPECT_FALSE(cfuture_cancel(&promise, &wrong_gen));

    EXPECT_TRUE(cfuture_cancel(&promise, &future));
}
