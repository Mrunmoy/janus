/**
 * @file cfuture_zephyr.h
 * @brief Zero-Heap Zephyr RTOS k_event Synchronization Adapter for cfuture
 *
 * Implements an OSAL synchronization backend using Zephyr's native k_event kernel objects.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_ZEPHYR_H
#define CFUTURE_ZEPHYR_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(CONFIG_ZEPHYR) || defined(ZEPHYR_INCLUDE_ZEPHYR_H_)
#include <zephyr/kernel.h>

#define CFUTURE_ZEPHYR_EVENT_MASK ((uint32_t)0x01U)

static inline void *cfuture_zephyr_event_create(void)
{
    /* In Zephyr, static k_event arrays are commonly provided */
    return NULL;
}

static inline void cfuture_zephyr_event_destroy(void *event_handle)
{
    ARG_UNUSED(event_handle);
}

static inline void cfuture_zephyr_event_set(void *event_handle)
{
    if (event_handle)
    {
        k_event_post((struct k_event *)event_handle, CFUTURE_ZEPHYR_EVENT_MASK);
    }
}

static inline bool cfuture_zephyr_event_wait(void *event_handle, uint32_t timeout_ms)
{
    if (!event_handle)
    {
        return false;
    }

    k_timeout_t timeout = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
    uint32_t events =
        k_event_wait((struct k_event *)event_handle, CFUTURE_ZEPHYR_EVENT_MASK, true, timeout);

    return (events & CFUTURE_ZEPHYR_EVENT_MASK) != 0U;
}

static inline void cfuture_zephyr_event_reset(void *event_handle)
{
    if (event_handle)
    {
        k_event_set_masked((struct k_event *)event_handle, 0, CFUTURE_ZEPHYR_EVENT_MASK);
    }
}

static inline cfuture_sync_ops_t cfuture_zephyr_sync_ops(void)
{
    cfuture_sync_ops_t ops = {
        .event_create = cfuture_zephyr_event_create,
        .event_destroy = cfuture_zephyr_event_destroy,
        .event_set = cfuture_zephyr_event_set,
        .event_wait = cfuture_zephyr_event_wait,
        .event_reset = cfuture_zephyr_event_reset,
        .event_set_from_isr = cfuture_zephyr_event_set,
    };

    return ops;
}

#endif /* CONFIG_ZEPHYR */

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_ZEPHYR_H */
