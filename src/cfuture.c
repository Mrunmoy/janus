/**
 * @file cfuture.c
 * @brief Zero-Heap Lock-Free Future/Promise Implementation
 *
 * Implements the core static pool bitmask allocation and state-driven lifecycle.
 * Dual ownership is coordinated entirely through atomic transitions on slot->state,
 * eliminating the need for a separate reference counter.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cfuture.h"

#include <string.h>

/**
 * @brief Atomically recycles a slot bit back into the pool's allocated bitmask.
 *
 * @param pool    The pool container.
 * @param slot_id The index of the slot to recycle.
 */
static inline void cfuture_slot_recycle(cfuture_pool_t *pool, uint8_t slot_id)
{
    if (!pool || slot_id >= pool->capacity)
    {
        return;
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];
    atomic_store_explicit(&slot->state, (uint_fast32_t)CFUTURE_STATE_IDLE, memory_order_release);

    /* Release ordering guarantees all previous slot writes are visible before recycling */
    atomic_fetch_and_explicit(&pool->allocated_mask, ~((uint_fast32_t)1U << slot_id),
                              memory_order_release);
}

#define CFUTURE_CAS_MAX_RETRIES ((uint32_t)1000U)

#if defined(_MSC_VER)
#include <intrin.h>
/**
 * @brief Portable count trailing zeros for 32-bit integers on MSVC.
 *
 * @param mask Non-zero 32-bit mask.
 * @return Number of trailing zero bits.
 */
static inline int cfuture_ctz32(uint32_t mask)
{
    unsigned long index = 0UL;
    _BitScanForward(&index, mask);
    return (int)index;
}
#else
/**
 * @brief Portable count trailing zeros for 32-bit integers on GCC/Clang.
 *
 * @param mask Non-zero 32-bit mask.
 * @return Number of trailing zero bits.
 */
static inline int cfuture_ctz32(uint32_t mask)
{
    return __builtin_ctz((unsigned int)mask);
}
#endif

/**
 * @brief Attempts to allocate an unused slot index using lock-free CAS.
 *
 * Bounds retries to prevent unbounded execution in hard real-time systems.
 *
 * @param pool The pool container.
 * @return Allocated slot index, or CFUTURE_INVALID_SLOT if pool is saturated or retries exhausted.
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
    if (from_isr && pool->sync_ops.event_set_from_isr && slot->event_handle)
    {
        pool->sync_ops.event_set_from_isr(slot->event_handle);
    }
    else if (pool->sync_ops.event_set && slot->event_handle)
    {
        pool->sync_ops.event_set(slot->event_handle);
    }
}

/**
 * @brief Shared fulfillment implementation for both task and ISR callers.
 *
 * @param promise    The promise handle.
 * @param payload    Optional result data pointer.
 * @param error_code Result status code.
 * @param from_isr   True if executing inside an ISR.
 */
static void cpromise_fulfill_impl(cpromise_t *promise, const void *payload, int32_t error_code,
                                  bool from_isr)
{
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity)
    {
        return;
    }

    cfuture_pool_t *pool = promise->pool;
    uint8_t slot_id = promise->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    promise->pool = NULL;
    promise->slot_id = CFUTURE_INVALID_SLOT;

    uint_fast32_t current_state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (current_state >= (uint_fast32_t)CFUTURE_STATE_TIMEOUT)
    {
        /* Consumer timed out or abandoned earlier; producer is second to finish, recycle slot */
        cfuture_slot_recycle(pool, slot_id);
        return;
    }

    if (payload && slot->payload && pool->payload_size > 0U)
    {
        memcpy(slot->payload, payload, pool->payload_size);
    }

    slot->error_code = error_code;

    atomic_thread_fence(memory_order_release);

    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                (uint_fast32_t)CFUTURE_STATE_COMPLETED,
                                                memory_order_release, memory_order_acquire))
    {
        /* Producer finished first: notify consumer. Consumer will recycle slot upon read. */
        cfuture_notify_consumer(pool, slot, from_isr);
    }
    else
    {
        /* Consumer timed out or abandoned while payload was copied; producer recycles slot */
        cfuture_slot_recycle(pool, slot_id);
    }
}

/**
 * @brief Shared drop implementation for both task and ISR callers.
 *
 * @param promise    The promise handle.
 * @param error_code Failure reason code.
 * @param from_isr   True if executing inside an ISR.
 */
static void cpromise_drop_impl(cpromise_t *promise, int32_t error_code, bool from_isr)
{
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity)
    {
        return;
    }

    cfuture_pool_t *pool = promise->pool;
    uint8_t slot_id = promise->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    promise->pool = NULL;
    promise->slot_id = CFUTURE_INVALID_SLOT;

    slot->error_code = error_code;

    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                (uint_fast32_t)CFUTURE_STATE_DROPPED,
                                                memory_order_release, memory_order_acquire))
    {
        /* Producer finished first: notify consumer. Consumer will recycle slot upon read. */
        cfuture_notify_consumer(pool, slot, from_isr);
    }
    else
    {
        /* Consumer timed out or abandoned; producer recycles slot */
        cfuture_slot_recycle(pool, slot_id);
    }
}

/**
 * @brief Copies payload and propagates status code upon future resolution.
 *
 * @param pool        The pool container.
 * @param slot_id     The index of the resolved slot.
 * @param state       The resolved state (COMPLETED or DROPPED).
 * @param out_payload Optional destination buffer for payload copy.
 * @param out_error   Optional destination for error code.
 * @return True if completed successfully, false if dropped.
 */
static bool cfuture_consume_result(cfuture_pool_t *pool, uint8_t slot_id, uint_fast32_t state,
                                   void *out_payload, int32_t *out_error)
{
    const cfuture_slot_t *slot = &pool->slots[slot_id];

    if (state == (uint_fast32_t)CFUTURE_STATE_COMPLETED)
    {
        atomic_thread_fence(memory_order_acquire);

        if (out_payload && slot->payload && pool->payload_size > 0U)
        {
            memcpy(out_payload, slot->payload, pool->payload_size);
        }

        if (out_error)
        {
            *out_error = slot->error_code;
        }

        /* Consumer finished second: recycle slot */
        cfuture_slot_recycle(pool, slot_id);
        return true;
    }

    if (out_error)
    {
        *out_error = slot->error_code;
    }

    /* Consumer finished second: recycle slot */
    cfuture_slot_recycle(pool, slot_id);
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
        atomic_store_explicit(&slots_buf[i].state, (uint_fast32_t)CFUTURE_STATE_IDLE,
                              memory_order_relaxed);
        slots_buf[i].error_code = 0;
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

    slot->error_code = 0;
    atomic_store_explicit(&slot->state, (uint_fast32_t)CFUTURE_STATE_PENDING, memory_order_release);

    out_promise->slot_id = slot_id;
    out_promise->pool = pool;

    out_future->slot_id = slot_id;
    out_future->pool = pool;

    return true;
}

bool cpromise_is_active(const cpromise_t *promise)
{
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity)
    {
        return false;
    }

    cfuture_slot_t *slot = &promise->pool->slots[promise->slot_id];
    uint_fast32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);

    return (st == (uint_fast32_t)CFUTURE_STATE_PENDING);
}

void cpromise_set_value(cpromise_t *promise, const void *payload, int32_t error_code)
{
    cpromise_fulfill_impl(promise, payload, error_code, false);
}

void cpromise_drop(cpromise_t *promise, int32_t error_code)
{
    cpromise_drop_impl(promise, error_code, false);
}

void cpromise_set_value_from_isr(cpromise_t *promise, const void *payload, int32_t error_code)
{
    cpromise_fulfill_impl(promise, payload, error_code, true);
}

void cpromise_drop_from_isr(cpromise_t *promise, int32_t error_code)
{
    cpromise_drop_impl(promise, error_code, true);
}

bool cfuture_wait_for(cfuture_t *future, uint32_t timeout_ms, void *out_payload, int32_t *out_error)
{
    if (!future || !future->pool || future->slot_id >= future->pool->capacity)
    {
        if (out_error)
        {
            *out_error = CFUTURE_ERR_INVALID;
        }
        return false;
    }

    cfuture_pool_t *pool = future->pool;
    uint8_t slot_id = future->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    future->pool = NULL;
    future->slot_id = CFUTURE_INVALID_SLOT;

    uint_fast32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);

    if (st == (uint_fast32_t)CFUTURE_STATE_PENDING)
    {
        if (pool->sync_ops.event_wait && slot->event_handle)
        {
            pool->sync_ops.event_wait(slot->event_handle, timeout_ms);
        }
        else
        {
            /* PAL bare-metal polling wait directly on slot->state with CPU relax */
            uint32_t start_ms = cfuture_pal_time_ms();
            while (atomic_load_explicit(&slot->state, memory_order_acquire) ==
                   (uint_fast32_t)CFUTURE_STATE_PENDING)
            {
                if (timeout_ms != UINT32_MAX)
                {
                    uint32_t elapsed = cfuture_pal_time_ms() - start_ms;
                    if (elapsed >= timeout_ms)
                    {
                        break;
                    }
                }
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
                /* Consumer won timeout race. Producer will see TIMEOUT when done and recycle. */
                if (out_error)
                {
                    *out_error = CFUTURE_ERR_TIMEOUT;
                }
                return false;
            }
            else
            {
                /* Producer fulfilled or dropped during the timeout check */
                st = atomic_load_explicit(&slot->state, memory_order_acquire);
            }
        }
    }

    if (st == (uint_fast32_t)CFUTURE_STATE_COMPLETED || st == (uint_fast32_t)CFUTURE_STATE_DROPPED)
    {
        return cfuture_consume_result(pool, slot_id, st, out_payload, out_error);
    }

    if (out_error)
    {
        *out_error = (st == (uint_fast32_t)CFUTURE_STATE_TIMEOUT) ? CFUTURE_ERR_TIMEOUT
                                                                  : CFUTURE_ERR_ABANDONED;
    }

    return false;
}

void cfuture_abandon(cfuture_t *future)
{
    if (!future || !future->pool || future->slot_id >= future->pool->capacity)
    {
        return;
    }

    cfuture_pool_t *pool = future->pool;
    uint8_t slot_id = future->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    future->pool = NULL;
    future->slot_id = CFUTURE_INVALID_SLOT;

    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                (uint_fast32_t)CFUTURE_STATE_ABANDONED,
                                                memory_order_acq_rel, memory_order_acquire))
    {
        /* Consumer won abandon race; producer will see ABANDONED and recycle */
        return;
    }

    /* Producer already completed or dropped; consumer is second to finish, recycle slot */
    cfuture_slot_recycle(pool, slot_id);
}
