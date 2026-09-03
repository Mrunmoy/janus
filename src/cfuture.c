/**
 * @file cfuture.c
 * @brief Zero-Heap Lock-Free Future/Promise Implementation (Janus)
 *
 * Implements the core static pool bitmask allocation, dual-owner 2->1->0 refcounting,
 * immediate non-blocking timeout unwinding, and ISR safety.
 *
 * Copyright (c) 2026 Mrunmoy Samal. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "cfuture.h"

#include <string.h>

/**
 * @brief Atomically recycles a slot bit back into the pool's allocated bitmask.
 *
 * Invoked by the "last one out the door" when ref_count transitions 1 -> 0.
 */
static inline void cfuture_slot_recycle(cfuture_pool_t *pool, uint8_t slot_id) {
    if (!pool || slot_id >= pool->capacity) {
        return;
    }
    cfuture_slot_t *slot = &pool->slots[slot_id];
    atomic_store_explicit(&slot->state, (uint_fast32_t)CFUTURE_STATE_IDLE, memory_order_relaxed);
    atomic_store_explicit(&slot->ref_count, 0U, memory_order_relaxed);

    /* Release ordering ensures all previous slot writes are visible before recycling */
    atomic_fetch_and_explicit(&pool->allocated_mask, ~((uint_fast32_t)1U << slot_id),
                              memory_order_release);
}

bool cfuture_pool_init(cfuture_pool_t *pool, uint32_t capacity, size_t payload_size,
                       cfuture_slot_t *slots_buf, uint8_t *payload_buf,
                       const cfuture_sync_ops_t *sync_ops) {
    if (!pool || capacity == 0U || capacity > CFUTURE_MAX_CAPACITY || !slots_buf) {
        return false;
    }
    if (payload_size > 0U && !payload_buf) {
        return false;
    }

    pool->capacity = capacity;
    pool->payload_size = payload_size;
    pool->slots = slots_buf;
    pool->payload_arena = payload_buf;
    atomic_store_explicit(&pool->allocated_mask, 0U, memory_order_relaxed);

    if (sync_ops) {
        pool->sync_ops = *sync_ops;
    } else {
        memset(&pool->sync_ops, 0, sizeof(pool->sync_ops));
    }

    for (uint32_t i = 0; i < capacity; ++i) {
        atomic_store_explicit(&slots_buf[i].ref_count, 0U, memory_order_relaxed);
        atomic_store_explicit(&slots_buf[i].state, (uint_fast32_t)CFUTURE_STATE_IDLE,
                              memory_order_relaxed);
        slots_buf[i].error_code = 0;
        slots_buf[i].payload =
            (payload_buf && payload_size > 0U) ? (payload_buf + (i * payload_size)) : NULL;

        if (pool->sync_ops.event_create) {
            slots_buf[i].event_handle = pool->sync_ops.event_create();
        } else {
            slots_buf[i].event_handle = NULL;
        }
    }

    return true;
}

void cfuture_pool_destroy(cfuture_pool_t *pool) {
    if (!pool || !pool->slots) {
        return;
    }
    if (pool->sync_ops.event_destroy) {
        for (uint32_t i = 0; i < pool->capacity; ++i) {
            if (pool->slots[i].event_handle) {
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

bool cfuture_create(cfuture_pool_t *pool, cpromise_t *out_promise, cfuture_t *out_future) {
    if (!pool || !pool->slots || !out_promise || !out_future) {
        return false;
    }

    uint_fast32_t valid_mask =
        (pool->capacity == 32U) ? ((uint_fast32_t)0xFFFFFFFFU) : (((uint_fast32_t)1U << pool->capacity) - 1U);

    uint_fast32_t current_mask = atomic_load_explicit(&pool->allocated_mask, memory_order_relaxed);
    uint8_t slot_id = CFUTURE_INVALID_SLOT;

    while (1) {
        uint_fast32_t available = (~current_mask) & valid_mask;
        if (available == 0U) {
            return false; /* Pool is full */
        }
        int bit = __builtin_ctz((unsigned int)available);
        uint_fast32_t new_mask = current_mask | ((uint_fast32_t)1U << bit);
        if (atomic_compare_exchange_weak_explicit(&pool->allocated_mask, &current_mask, new_mask,
                                                 memory_order_acq_rel, memory_order_relaxed)) {
            slot_id = (uint8_t)bit;
            break;
        }
    }

    cfuture_slot_t *slot = &pool->slots[slot_id];

    /* Reset event if reset hook provided */
    if (pool->sync_ops.event_reset && slot->event_handle) {
        pool->sync_ops.event_reset(slot->event_handle);
    }

    slot->error_code = 0;
    atomic_store_explicit(&slot->state, (uint_fast32_t)CFUTURE_STATE_PENDING, memory_order_relaxed);
    atomic_store_explicit(&slot->ref_count, 2U, memory_order_release);

    out_promise->slot_id = slot_id;
    out_promise->pool = pool;

    out_future->slot_id = slot_id;
    out_future->pool = pool;

    return true;
}

bool cpromise_is_active(const cpromise_t *promise) {
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity) {
        return false;
    }
    cfuture_slot_t *slot = &promise->pool->slots[promise->slot_id];
    uint_fast32_t ref = atomic_load_explicit(&slot->ref_count, memory_order_acquire);
    uint_fast32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);
    return (ref == 2U) && (st == (uint_fast32_t)CFUTURE_STATE_PENDING);
}

void cpromise_set_value(cpromise_t *promise, const void *payload, int32_t error_code) {
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity) {
        return;
    }
    cfuture_pool_t *pool = promise->pool;
    uint8_t slot_id = promise->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    /* Invalidate caller promise handle immediately */
    promise->pool = NULL;
    promise->slot_id = CFUTURE_INVALID_SLOT;

    /* If caller already timed out or abandoned, don't copy, just drop ref */
    uint_fast32_t current_state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (current_state >= (uint_fast32_t)CFUTURE_STATE_TIMEOUT) {
        uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
        if (prev_ref == 1U) {
            cfuture_slot_recycle(pool, slot_id);
        }
        return;
    }

    /* Copy payload into pool slot arena */
    if (payload && slot->payload && pool->payload_size > 0U) {
        memcpy(slot->payload, payload, pool->payload_size);
    }
    slot->error_code = error_code;

    /* Atomically publish state transition from PENDING to COMPLETED */
    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                (uint_fast32_t)CFUTURE_STATE_COMPLETED,
                                                memory_order_release, memory_order_acquire)) {
        /* Published successfully; signal waiting consumer */
        if (pool->sync_ops.event_set && slot->event_handle) {
            pool->sync_ops.event_set(slot->event_handle);
        }
    }

    /* Producer drops its reference */
    uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
    if (prev_ref == 1U) {
        /* Consumer had timed out or abandoned right as we completed */
        cfuture_slot_recycle(pool, slot_id);
    }
}

void cpromise_drop(cpromise_t *promise, int32_t error_code) {
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity) {
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
                                                memory_order_release, memory_order_acquire)) {
        if (pool->sync_ops.event_set && slot->event_handle) {
            pool->sync_ops.event_set(slot->event_handle);
        }
    }

    uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
    if (prev_ref == 1U) {
        cfuture_slot_recycle(pool, slot_id);
    }
}

void cpromise_set_value_from_isr(cpromise_t *promise, const void *payload, int32_t error_code) {
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity) {
        return;
    }
    cfuture_pool_t *pool = promise->pool;
    uint8_t slot_id = promise->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    promise->pool = NULL;
    promise->slot_id = CFUTURE_INVALID_SLOT;

    uint_fast32_t current_state = atomic_load_explicit(&slot->state, memory_order_acquire);
    if (current_state >= (uint_fast32_t)CFUTURE_STATE_TIMEOUT) {
        uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
        if (prev_ref == 1U) {
            cfuture_slot_recycle(pool, slot_id);
        }
        return;
    }

    if (payload && slot->payload && pool->payload_size > 0U) {
        memcpy(slot->payload, payload, pool->payload_size);
    }
    slot->error_code = error_code;

    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                (uint_fast32_t)CFUTURE_STATE_COMPLETED,
                                                memory_order_release, memory_order_acquire)) {
        if (pool->sync_ops.event_set_from_isr && slot->event_handle) {
            pool->sync_ops.event_set_from_isr(slot->event_handle);
        } else if (pool->sync_ops.event_set && slot->event_handle) {
            pool->sync_ops.event_set(slot->event_handle);
        }
    }

    uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
    if (prev_ref == 1U) {
        cfuture_slot_recycle(pool, slot_id);
    }
}

void cpromise_drop_from_isr(cpromise_t *promise, int32_t error_code) {
    if (!promise || !promise->pool || promise->slot_id >= promise->pool->capacity) {
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
                                                memory_order_release, memory_order_acquire)) {
        if (pool->sync_ops.event_set_from_isr && slot->event_handle) {
            pool->sync_ops.event_set_from_isr(slot->event_handle);
        } else if (pool->sync_ops.event_set && slot->event_handle) {
            pool->sync_ops.event_set(slot->event_handle);
        }
    }

    uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
    if (prev_ref == 1U) {
        cfuture_slot_recycle(pool, slot_id);
    }
}

bool cfuture_wait_for(cfuture_t *future, uint32_t timeout_ms, void *out_payload,
                      int32_t *out_error) {
    if (!future || !future->pool || future->slot_id >= future->pool->capacity) {
        if (out_error) {
            *out_error = CFUTURE_ERR_INVALID;
        }
        return false;
    }

    cfuture_pool_t *pool = future->pool;
    uint8_t slot_id = future->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    /* Invalidate future handle immediately */
    future->pool = NULL;
    future->slot_id = CFUTURE_INVALID_SLOT;

    uint_fast32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);

    /* Wait if still pending */
    if (st == (uint_fast32_t)CFUTURE_STATE_PENDING) {
        if (pool->sync_ops.event_wait && slot->event_handle) {
            pool->sync_ops.event_wait(slot->event_handle, timeout_ms);
        }

        /* Check state after wait / timeout */
        st = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (st == (uint_fast32_t)CFUTURE_STATE_PENDING) {
            /* Still pending: try atomic CAS to TIMEOUT */
            uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
            if (atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                                        (uint_fast32_t)CFUTURE_STATE_TIMEOUT,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                /* Successfully timed out! Consumer drops reference */
                if (out_error) {
                    *out_error = CFUTURE_ERR_TIMEOUT;
                }
                uint_fast32_t prev_ref =
                    atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
                if (prev_ref == 1U) {
                    /* Worker already finished right after our CAS */
                    cfuture_slot_recycle(pool, slot_id);
                }
                return false;
            } else {
                /* CAS failed: worker completed concurrently */
                st = expected;
            }
        }
    }

    /* Result is ready (COMPLETED or DROPPED) */
    if (st == (uint_fast32_t)CFUTURE_STATE_COMPLETED) {
        if (out_payload && slot->payload && pool->payload_size > 0U) {
            memcpy(out_payload, slot->payload, pool->payload_size);
        }
        if (out_error) {
            *out_error = slot->error_code;
        }
        uint_fast32_t prev_ref =
            atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
        if (prev_ref == 1U) {
            cfuture_slot_recycle(pool, slot_id);
        }
        return true;
    }

    if (st == (uint_fast32_t)CFUTURE_STATE_DROPPED) {
        if (out_error) {
            *out_error = slot->error_code;
        }
        uint_fast32_t prev_ref =
            atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
        if (prev_ref == 1U) {
            cfuture_slot_recycle(pool, slot_id);
        }
        return false;
    }

    /* Any other state (TIMEOUT, ABANDONED, IDLE) */
    if (out_error) {
        *out_error = (st == (uint_fast32_t)CFUTURE_STATE_TIMEOUT) ? CFUTURE_ERR_TIMEOUT
                                                                 : CFUTURE_ERR_ABANDONED;
    }
    uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
    if (prev_ref == 1U) {
        cfuture_slot_recycle(pool, slot_id);
    }
    return false;
}

void cfuture_abandon(cfuture_t *future) {
    if (!future || !future->pool || future->slot_id >= future->pool->capacity) {
        return;
    }
    cfuture_pool_t *pool = future->pool;
    uint8_t slot_id = future->slot_id;
    cfuture_slot_t *slot = &pool->slots[slot_id];

    future->pool = NULL;
    future->slot_id = CFUTURE_INVALID_SLOT;

    uint_fast32_t expected = (uint_fast32_t)CFUTURE_STATE_PENDING;
    atomic_compare_exchange_strong_explicit(&slot->state, &expected,
                                            (uint_fast32_t)CFUTURE_STATE_ABANDONED,
                                            memory_order_acq_rel, memory_order_acquire);

    uint_fast32_t prev_ref = atomic_fetch_sub_explicit(&slot->ref_count, 1U, memory_order_acq_rel);
    if (prev_ref == 1U) {
        cfuture_slot_recycle(pool, slot_id);
    }
}
