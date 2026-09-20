/**
 * @file cfuture.c
 * @brief Zero-Heap Lock-Free Future/Promise Implementation for Embedded C
 *
 * Implements the core static pool bitmask allocation, dual-owner reference
 * tracking, immediate non-blocking timeout unwinding, and ISR safety.
 *
 * Dual ownership (hold bits CONSUMER | PRODUCER in cfuture_slot_t::owner):
 * - Slot is allocated with both hold bits set (one per handle).
 * - Each side may only clear its own bit, so a duplicated or replayed handle can
 *   never release the other side's hold.
 * - Producer keeps its hold until event signaling is completely dispatched.
 *   This ensures the consumer cannot recycle or reallocate the slot while the
 *   producer is preempted inside or around cfuture_notify_consumer.
 * - Whichever party clears the last hold bit recycles the slot.
 *
 * Generation tagging:
 * - owner also carries the slot generation, bumped on every recycle. A side must
 *   claim the slot (CAS on owner) before touching it; a handle from a previous
 *   occupancy fails that CAS instead of aliasing the slot's next occupant.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cfuture.h"

#include <string.h>

/** Producer has begun resolving; only one set_value()/drop() may own the slot write. */
#define CFUTURE_CLAIM_RESOLVING ((uint_fast32_t)0x04U)
/** Consumer has begun wait_for()/abandon(); only one may consume the result. */
#define CFUTURE_CLAIM_WAITING ((uint_fast32_t)0x08U)

#define CFUTURE_PRODUCER_SIDE (CFUTURE_HOLD_PRODUCER | CFUTURE_CLAIM_RESOLVING)
#define CFUTURE_CONSUMER_SIDE (CFUTURE_HOLD_CONSUMER | CFUTURE_CLAIM_WAITING)

/* While one side claims, the other side can change owner at most twice (its own
 * claim, then its release), so a third strong-CAS attempt always decides. */
#define CFUTURE_CLAIM_MAX_ATTEMPTS ((uint32_t)3U)

/**
 * @brief Recycles a slot back to the pool: advances its generation and clears its bitmask bit.
 *
 * The generation is bumped before the occupancy bit is cleared, so whoever claims
 * the slot next always observes the new generation. Generation 0 is skipped on
 * wraparound because handles treat it as always-invalid.
 *
 * @param pool    The pool container.
 * @param slot_id The index of the slot to release.
 */
static inline void cfuture_slot_recycle(cfuture_pool_t *pool, uint8_t slot_id)
{
    cfuture_slot_t *slot = &pool->slots[slot_id];
    uint_fast32_t cur_gen =
        atomic_load_explicit(&slot->owner, memory_order_relaxed) >> CFUTURE_GEN_SHIFT;
    uint_fast32_t next_gen = (cur_gen >= (uint_fast32_t)CFUTURE_GEN_MASK) ? 1U : (cur_gen + 1U);

    atomic_store_explicit(&slot->state, (uint_fast32_t)CFUTURE_STATE_IDLE, memory_order_release);
    atomic_store_explicit(&slot->owner, next_gen << CFUTURE_GEN_SHIFT, memory_order_release);

    /* Release ordering guarantees all previous slot writes are visible before recycling */
    atomic_fetch_and_explicit(&pool->allocated_mask, ~((uint_fast32_t)1U << slot_id),
                              memory_order_release);
}

/**
 * @brief Clears one side's hold (and claim) bits and recycles the slot if it was the last holder.
 *
 * @param pool      The pool container.
 * @param slot_id   The index of the slot.
 * @param side_bits CFUTURE_PRODUCER_SIDE or CFUTURE_CONSUMER_SIDE. Caller must own the claim.
 */
static inline void cfuture_slot_release_side(cfuture_pool_t *pool, uint8_t slot_id,
                                             uint_fast32_t side_bits)
{
    cfuture_slot_t *slot = &pool->slots[slot_id];
    uint_fast32_t prev = atomic_fetch_and_explicit(&slot->owner, ~side_bits, memory_order_acq_rel);

    if (((prev & ~side_bits) & CFUTURE_HOLD_BOTH) == CFUTURE_HOLD_NONE)
    {
        cfuture_slot_recycle(pool, slot_id);
    }
}

/**
 * @brief Exclusively claims one side of a live slot for a generation-tagged handle.
 *
 * @param pool       The pool container.
 * @param slot_id    Slot index from the handle.
 * @param generation Generation tag from the handle.
 * @param hold_bit   CFUTURE_HOLD_PRODUCER or CFUTURE_HOLD_CONSUMER.
 * @param claim_bit  Matching CFUTURE_CLAIM_* bit.
 * @return true if this caller now exclusively owns that side; false if the handle is
 *         malformed, stale, already released, or a duplicate already claimed the side.
 */
static bool cfuture_slot_try_claim(cfuture_pool_t *pool, uint8_t slot_id, uint32_t generation,
                                   uint_fast32_t hold_bit, uint_fast32_t claim_bit)
{
    if (!pool || !pool->slots || slot_id >= pool->capacity || generation == 0U)
    {
        return false;
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];
    uint_fast32_t owner = atomic_load_explicit(&slot->owner, memory_order_acquire);

    for (uint32_t attempt = 0; attempt < CFUTURE_CLAIM_MAX_ATTEMPTS; ++attempt)
    {
        if ((owner >> CFUTURE_GEN_SHIFT) != (uint_fast32_t)generation || (owner & hold_bit) == 0U ||
            (owner & claim_bit) != 0U)
        {
            return false;
        }

        if (atomic_compare_exchange_strong_explicit(&slot->owner, &owner, owner | claim_bit,
                                                    memory_order_acq_rel, memory_order_acquire))
        {
            return true;
        }
    }

    return false;
}

#define CFUTURE_CAS_MAX_RETRIES ((uint32_t)1000U)

#if defined(_MSC_VER)
#include <intrin.h>
static inline int cfuture_ctz32(uint32_t mask)
{
    unsigned long index = 0UL;
    _BitScanForward(&index, mask);
    return (int)index;
}
#else
static inline int cfuture_ctz32(uint32_t mask)
{
    return __builtin_ctz(mask);
}
#endif

/**
 * @brief Claims the first available free slot in the pool using a lock-free CAS loop.
 *
 * @param pool Pointer to pool container.
 * @return Slot index (0..31) on success, or CFUTURE_INVALID_SLOT if full.
 */
static uint8_t cfuture_pool_claim_slot(cfuture_pool_t *pool)
{
    uint_fast32_t valid_mask = (pool->capacity == 32U)
                                   ? ((uint_fast32_t)0xFFFFFFFFU)
                                   : (((uint_fast32_t)1U << pool->capacity) - 1U);

    uint_fast32_t current_mask = atomic_load_explicit(&pool->allocated_mask, memory_order_relaxed);

    for (uint32_t retries = 0; retries < CFUTURE_CAS_MAX_RETRIES; ++retries)
    {
        uint_fast32_t available = (~current_mask) & valid_mask;
        if (available == 0U)
        {
            return CFUTURE_INVALID_SLOT;
        }

        int bit = cfuture_ctz32((uint32_t)available);
        uint_fast32_t new_mask = current_mask | ((uint_fast32_t)1U << bit);

        if (atomic_compare_exchange_weak_explicit(&pool->allocated_mask, &current_mask, new_mask,
                                                  memory_order_acq_rel, memory_order_relaxed))
        {
            return (uint8_t)bit;
        }
    }

    return CFUTURE_INVALID_SLOT;
}

/**
 * @brief Dispatches the synchronization event to wake up the waiting consumer.
 *
 * @param pool     The pool container.
 * @param slot     The slot being signaled.
 * @param from_isr True if called from interrupt context.
 */
static inline void cfuture_notify_consumer(cfuture_pool_t *pool, cfuture_slot_t *slot,
                                           bool from_isr)
{
    if (pool->sync_ops.event_set && slot->event_handle)
    {
        if (from_isr && pool->sync_ops.event_set_from_isr)
        {
            pool->sync_ops.event_set_from_isr(slot->event_handle);
        }
        else
        {
            pool->sync_ops.event_set(slot->event_handle);
        }
    }
}

/**
 * @brief Shared resolution implementation for set_value/drop from task and ISR callers.
 *
 * @param promise      The promise handle.
 * @param payload      Pointer to payload to copy into slot arena (optional).
 * @param status_code  Result status code to store.
 * @param target_state CFUTURE_STATE_COMPLETED or CFUTURE_STATE_DROPPED.
 * @param from_isr     True if called from interrupt context.
 */
static void cpromise_resolve_impl(cpromise_t *promise, const void *payload, int32_t status_code,
                                  cfuture_state_t target_state, bool from_isr)
{
    if (!promise)
    {
        return;
    }

    cfuture_pool_t *pool = promise->pool;
    uint8_t slot_id = promise->slot_id;
    uint32_t generation = promise->generation;

    promise->pool = NULL;
    promise->slot_id = CFUTURE_INVALID_SLOT;
    promise->generation = 0U;

    if (!cfuture_slot_try_claim(pool, slot_id, generation, CFUTURE_HOLD_PRODUCER,
                                CFUTURE_CLAIM_RESOLVING))
    {
        return;
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];

    /* Consumer timed out or abandoned: skip the write, just drop the producer hold below. */
    if (atomic_load_explicit(&slot->state, memory_order_acquire) ==
        (uint_fast32_t)CFUTURE_STATE_PENDING)
    {
        if (payload && pool->payload_size > 0U && slot->payload)
        {
            memcpy(slot->payload, payload, pool->payload_size);
        }

        slot->status_code = status_code;

        uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                    (uint_fast32_t)target_state,
                                                    memory_order_release, memory_order_acquire))
        {
            cfuture_notify_consumer(pool, slot, from_isr);
        }
    }

    /* Producer releases its hold only AFTER signaling is completed.
     * Prevents slot reuse/reset while producer is preempted inside notification. */
    cfuture_slot_release_side(pool, slot_id, CFUTURE_PRODUCER_SIDE);
}

/**
 * @brief Consumes a completed or dropped result, copying data and releasing the consumer hold.
 *
 * @param pool        The pool container.
 * @param slot_id     Slot index.
 * @param state       Observed state (COMPLETED or DROPPED).
 * @param out_payload Buffer to receive result payload (optional).
 * @param out_status  Pointer to receive result status code (optional).
 * @return True if completed successfully, false if dropped.
 */
static bool cfuture_consume_result(cfuture_pool_t *pool, uint8_t slot_id, uint_fast32_t state,
                                   void *out_payload, int32_t *out_status)
{
    const cfuture_slot_t *slot = &pool->slots[slot_id];

    if (state == (uint_fast32_t)CFUTURE_STATE_COMPLETED)
    {
        if (out_payload && slot->payload && pool->payload_size > 0U)
        {
            memcpy(out_payload, slot->payload, pool->payload_size);
        }

        if (out_status)
        {
            *out_status = slot->status_code;
        }

        cfuture_slot_release_side(pool, slot_id, CFUTURE_CONSUMER_SIDE);
        return true;
    }

    if (out_status)
    {
        *out_status = slot->status_code;
    }

    cfuture_slot_release_side(pool, slot_id, CFUTURE_CONSUMER_SIDE);
    return false;
}

bool cfuture_pool_init(cfuture_pool_t *pool, uint32_t capacity, size_t payload_size,
                       cfuture_slot_t *slots_buf, uint8_t *payload_buf,
                       const cfuture_sync_ops_t *sync_ops)
{
    if (!pool || capacity == 0U || capacity > CFUTURE_MAX_CAPACITY || !slots_buf)
    {
        return false;
    }

    if (payload_size > 0U && !payload_buf)
    {
        return false;
    }

    pool->capacity = capacity;
    pool->payload_size = payload_size;
    pool->slots = slots_buf;
    pool->payload_arena = payload_buf;
    atomic_store_explicit(&pool->allocated_mask, 0U, memory_order_relaxed);

    if (sync_ops)
    {
        pool->sync_ops = *sync_ops;
    }
    else
    {
        memset(&pool->sync_ops, 0, sizeof(pool->sync_ops));
    }

    for (uint32_t i = 0; i < capacity; ++i)
    {
        atomic_store_explicit(&slots_buf[i].owner, (uint_fast32_t)1U << CFUTURE_GEN_SHIFT,
                              memory_order_relaxed);
        atomic_store_explicit(&slots_buf[i].state, (uint_fast32_t)CFUTURE_STATE_IDLE,
                              memory_order_relaxed);
        slots_buf[i].status_code = 0;
        slots_buf[i].payload =
            (payload_buf && payload_size > 0U) ? (payload_buf + (i * payload_size)) : NULL;

        if (pool->sync_ops.event_create)
        {
            slots_buf[i].event_handle = pool->sync_ops.event_create();
            if (!slots_buf[i].event_handle)
            {
                if (pool->sync_ops.event_destroy)
                {
                    for (uint32_t j = 0; j < i; ++j)
                    {
                        if (slots_buf[j].event_handle)
                        {
                            pool->sync_ops.event_destroy(slots_buf[j].event_handle);
                            slots_buf[j].event_handle = NULL;
                        }
                    }
                }

                pool->slots = NULL;
                pool->capacity = 0U;
                pool->payload_size = 0U;
                pool->payload_arena = NULL;
                atomic_store_explicit(&pool->allocated_mask, 0U, memory_order_relaxed);
                return false;
            }
        }
        else
        {
            slots_buf[i].event_handle = NULL;
        }
    }

    return true;
}

void cfuture_pool_destroy(cfuture_pool_t *pool)
{
    if (!pool || !pool->slots)
    {
        return;
    }

    if (pool->sync_ops.event_destroy)
    {
        for (uint32_t i = 0; i < pool->capacity; ++i)
        {
            if (pool->slots[i].event_handle)
            {
                pool->sync_ops.event_destroy(pool->slots[i].event_handle);
                pool->slots[i].event_handle = NULL;
            }
        }
    }

    atomic_store_explicit(&pool->allocated_mask, 0U, memory_order_relaxed);
    pool->capacity = 0U;
    pool->slots = NULL;
    pool->payload_size = 0U;
    pool->payload_arena = NULL;
}

bool cfuture_create(cfuture_pool_t *pool, cpromise_t *out_promise, cfuture_t *out_future)
{
    if (!pool || !pool->slots || !out_promise || !out_future)
    {
        return false;
    }

    uint8_t slot_id = cfuture_pool_claim_slot(pool);
    if (slot_id == CFUTURE_INVALID_SLOT)
    {
        return false;
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];

    if (pool->sync_ops.event_reset && slot->event_handle)
    {
        pool->sync_ops.event_reset(slot->event_handle);
    }

    /* The bitmask claim makes this thread the slot's only writer until owner is published. */
    uint_fast32_t generation =
        atomic_load_explicit(&slot->owner, memory_order_acquire) >> CFUTURE_GEN_SHIFT;

    slot->status_code = 0;
    atomic_store_explicit(&slot->state, (uint_fast32_t)CFUTURE_STATE_PENDING, memory_order_release);
    atomic_store_explicit(&slot->owner, (generation << CFUTURE_GEN_SHIFT) | CFUTURE_HOLD_BOTH,
                          memory_order_release);

    out_promise->slot_id = slot_id;
    out_promise->generation = (uint32_t)generation;
    out_promise->pool = pool;

    out_future->slot_id = slot_id;
    out_future->generation = (uint32_t)generation;
    out_future->pool = pool;

    return true;
}

bool cpromise_is_active(const cpromise_t *promise)
{
    if (!promise || !promise->pool || !promise->pool->slots ||
        promise->slot_id >= promise->pool->capacity || promise->generation == 0U)
    {
        return false;
    }

    cfuture_slot_t *slot = &promise->pool->slots[promise->slot_id];
    uint_fast32_t before = atomic_load_explicit(&slot->owner, memory_order_acquire);
    uint_fast32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);
    uint_fast32_t owner = atomic_load_explicit(&slot->owner, memory_order_acquire);

    /* state only belongs to this handle if the generation held still across its read;
     * otherwise a recycled slot's fresh PENDING could be mistaken for ours. Hold/claim
     * bits may legitimately move in between (e.g. the consumer starting its wait). */
    return ((before >> CFUTURE_GEN_SHIFT) == (uint_fast32_t)promise->generation) &&
           ((owner >> CFUTURE_GEN_SHIFT) == (uint_fast32_t)promise->generation) &&
           ((owner & (CFUTURE_HOLD_BOTH | CFUTURE_CLAIM_RESOLVING)) == CFUTURE_HOLD_BOTH) &&
           (st == (uint_fast32_t)CFUTURE_STATE_PENDING);
}

void cpromise_set_value(cpromise_t *promise, const void *payload, int32_t status_code)
{
    cpromise_resolve_impl(promise, payload, status_code, CFUTURE_STATE_COMPLETED, false);
}

void cpromise_drop(cpromise_t *promise, int32_t status_code)
{
    cpromise_resolve_impl(promise, NULL, status_code, CFUTURE_STATE_DROPPED, false);
}

void cpromise_set_value_from_isr(cpromise_t *promise, const void *payload, int32_t status_code)
{
    cpromise_resolve_impl(promise, payload, status_code, CFUTURE_STATE_COMPLETED, true);
}

void cpromise_drop_from_isr(cpromise_t *promise, int32_t status_code)
{
    cpromise_resolve_impl(promise, NULL, status_code, CFUTURE_STATE_DROPPED, true);
}

bool cfuture_wait_for(cfuture_t *future, uint32_t timeout_ms, void *out_payload,
                      int32_t *out_status)
{
    cfuture_pool_t *pool = future ? future->pool : NULL;
    uint8_t slot_id = future ? future->slot_id : CFUTURE_INVALID_SLOT;
    uint32_t generation = future ? future->generation : 0U;

    if (future)
    {
        future->pool = NULL;
        future->slot_id = CFUTURE_INVALID_SLOT;
        future->generation = 0U;
    }

    if (!cfuture_slot_try_claim(pool, slot_id, generation, CFUTURE_HOLD_CONSUMER,
                                CFUTURE_CLAIM_WAITING))
    {
        if (out_status)
        {
            *out_status = CFUTURE_ERR_INVALID;
        }
        return false;
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];

    uint_fast32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);

    if (st == (uint_fast32_t)CFUTURE_STATE_PENDING)
    {
        /* Only the slot state and the PAL clock decide when the wait is over. An OSAL
         * wait may return early without a signal (spurious condvar wakeup, RTOS wait
         * error, adapter that caps long waits), so its result is only a wakeup hint.
         * Expiry is strict (elapsed > timeout) because the millisecond clock truncates:
         * a wait may overshoot by one tick but never fires early. */
        const bool use_event = pool->sync_ops.event_wait && slot->event_handle;
        const uint32_t start_ms = cfuture_pal_time_ms();
        uint32_t remaining_ms = timeout_ms;

        while (timeout_ms != 0U && atomic_load_explicit(&slot->state, memory_order_acquire) ==
                                       (uint_fast32_t)CFUTURE_STATE_PENDING)
        {
            if (timeout_ms != UINT32_MAX)
            {
                uint32_t elapsed_ms = cfuture_pal_time_ms() - start_ms;
                if (elapsed_ms > timeout_ms)
                {
                    break;
                }
                remaining_ms = (timeout_ms - elapsed_ms) + 1U;
            }

            if (!use_event || !pool->sync_ops.event_wait(slot->event_handle, remaining_ms))
            {
                /* Polling mode, or an unsignaled return: yield rather than spin hot. */
                cfuture_pal_cpu_relax();
            }
        }

        st = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (st == (uint_fast32_t)CFUTURE_STATE_PENDING)
        {
            uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
            if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                        (uint_fast32_t)CFUTURE_STATE_TIMEOUT,
                                                        memory_order_acq_rel, memory_order_acquire))
            {
                if (out_status)
                {
                    *out_status = CFUTURE_ERR_TIMEOUT;
                }

                cfuture_slot_release_side(pool, slot_id, CFUTURE_CONSUMER_SIDE);
                return false;
            }
            else
            {
                st = atomic_load_explicit(&slot->state, memory_order_acquire);
            }
        }
    }

    if (st == (uint_fast32_t)CFUTURE_STATE_COMPLETED || st == (uint_fast32_t)CFUTURE_STATE_DROPPED)
    {
        return cfuture_consume_result(pool, slot_id, st, out_payload, out_status);
    }

    if (out_status)
    {
        *out_status = (st == (uint_fast32_t)CFUTURE_STATE_TIMEOUT) ? CFUTURE_ERR_TIMEOUT
                                                                   : CFUTURE_ERR_ABANDONED;
    }

    cfuture_slot_release_side(pool, slot_id, CFUTURE_CONSUMER_SIDE);
    return false;
}

void cfuture_abandon(cfuture_t *future)
{
    if (!future)
    {
        return;
    }

    cfuture_pool_t *pool = future->pool;
    uint8_t slot_id = future->slot_id;
    uint32_t generation = future->generation;

    future->pool = NULL;
    future->slot_id = CFUTURE_INVALID_SLOT;
    future->generation = 0U;

    if (!cfuture_slot_try_claim(pool, slot_id, generation, CFUTURE_HOLD_CONSUMER,
                                CFUTURE_CLAIM_WAITING))
    {
        return;
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];

    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                            (uint_fast32_t)CFUTURE_STATE_ABANDONED,
                                            memory_order_acq_rel, memory_order_acquire);

    cfuture_slot_release_side(pool, slot_id, CFUTURE_CONSUMER_SIDE);
}

bool cfuture_cancel(cpromise_t *promise, cfuture_t *future)
{
    if (!promise || !future || !promise->pool || promise->pool != future->pool ||
        promise->slot_id != future->slot_id || promise->generation != future->generation ||
        !promise->pool->slots || promise->slot_id >= promise->pool->capacity ||
        promise->generation == 0U)
    {
        return false;
    }

    cfuture_pool_t *pool = promise->pool;
    uint8_t slot_id = promise->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    /* Both holds present and neither side claimed, in one transition: a worker that
     * already started resolving (or a waiter) makes this fail rather than race. */
    uint_fast32_t tag = (uint_fast32_t)promise->generation << CFUTURE_GEN_SHIFT;
    uint_fast32_t expected = tag | CFUTURE_HOLD_BOTH;
    if (!atomic_compare_exchange_strong_explicit(&slot->owner, &expected, tag | CFUTURE_HOLD_NONE,
                                                 memory_order_acq_rel, memory_order_acquire))
    {
        return false;
    }

    cfuture_slot_recycle(pool, slot_id);

    promise->pool = NULL;
    promise->slot_id = CFUTURE_INVALID_SLOT;
    promise->generation = 0U;
    future->pool = NULL;
    future->slot_id = CFUTURE_INVALID_SLOT;
    future->generation = 0U;

    return true;
}
