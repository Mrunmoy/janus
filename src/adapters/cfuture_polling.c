/**
 * @file cfuture_polling.c
 * @brief Zero-Heap Bare-Metal Spin-Polling Adapter Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_polling.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__STDC_NO_ATOMICS__)
#error "cfuture polling requires atomic support"
#else
#include <stdatomic.h>
#endif

typedef struct
{
    atomic_bool signaled;
    atomic_bool in_use;
} cfuture_poll_event_t;

static cfuture_poll_event_t s_poll_events[CFUTURE_POLL_MAX_EVENTS];

static void *poll_event_create(void)
{
    for (uint32_t i = 0; i < CFUTURE_POLL_MAX_EVENTS; ++i)
    {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&s_poll_events[i].in_use, &expected, true,
                                                    memory_order_acq_rel, memory_order_relaxed))
        {
            atomic_store_explicit(&s_poll_events[i].signaled, false, memory_order_relaxed);
            return &s_poll_events[i];
        }
    }

    return NULL;
}

static void poll_event_destroy(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_poll_event_t *ev = (cfuture_poll_event_t *)event_handle;
    atomic_store_explicit(&ev->signaled, false, memory_order_relaxed);
    atomic_store_explicit(&ev->in_use, false, memory_order_release);
}

static void poll_event_set(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_poll_event_t *ev = (cfuture_poll_event_t *)event_handle;
    atomic_store_explicit(&ev->signaled, true, memory_order_release);
}

static bool poll_event_wait(void *event_handle, uint32_t timeout_ms)
{
    if (!event_handle)
    {
        return false;
    }

    cfuture_poll_event_t *ev = (cfuture_poll_event_t *)event_handle;

    if (timeout_ms == 0U)
    {
        if (atomic_load_explicit(&ev->signaled, memory_order_acquire))
        {
            atomic_store_explicit(&ev->signaled, false, memory_order_relaxed);
            return true;
        }

        return false;
    }

    /* Calibrated bounded spin-wait ceiling to guarantee no infinite hang */
    uint32_t spins = (timeout_ms == UINT32_MAX) ? 10000000U : (timeout_ms * 1000U);
    for (uint32_t i = 0; i < spins; ++i)
    {
        if (atomic_load_explicit(&ev->signaled, memory_order_acquire))
        {
            atomic_store_explicit(&ev->signaled, false, memory_order_relaxed);
            return true;
        }
    }

    return false;
}

static void poll_event_reset(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_poll_event_t *ev = (cfuture_poll_event_t *)event_handle;
    atomic_store_explicit(&ev->signaled, false, memory_order_relaxed);
}

static const cfuture_sync_ops_t s_polling_ops = {
    .event_create = poll_event_create,
    .event_destroy = poll_event_destroy,
    .event_set = poll_event_set,
    .event_wait = poll_event_wait,
    .event_reset = poll_event_reset,
    .event_set_from_isr = poll_event_set,
};

const cfuture_sync_ops_t *cfuture_polling_sync_ops(void)
{
    return &s_polling_ops;
}
