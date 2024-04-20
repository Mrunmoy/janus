/**
 * @file cfuture_threadx.h
 * @brief Eclipse / Azure RTOS ThreadX Event Flags Adapter for cfuture
 *
 * Provides a zero-heap synchronization adapter for ThreadX using
 * TX_EVENT_FLAGS_GROUP. Compatible with ThreadX tasks, software timers,
 * and Interrupt Service Routines (ISRs).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_THREADX_H
#define CFUTURE_THREADX_H

#include "cfuture.h"

#ifdef __has_include
#if __has_include("tx_api.h")
#include "tx_api.h"
#define CFUTURE_THREADX_AVAILABLE 1
#endif
#endif

#ifndef CFUTURE_THREADX_AVAILABLE
#define CFUTURE_THREADX_AVAILABLE 0
#endif

#if CFUTURE_THREADX_AVAILABLE

#ifdef __cplusplus
extern "C"
{
#endif

#define CFUTURE_THREADX_FLAG_SIGNALED ((ULONG)0x00000001UL)

#ifndef TX_TIMER_TICKS_PER_SECOND
#define TX_TIMER_TICKS_PER_SECOND 100U
#endif

    /**
     * @brief Context structure for a ThreadX event flags group handle.
     */
    typedef struct
    {
        TX_EVENT_FLAGS_GROUP group; /**< ThreadX native event flags group control block. */
        bool in_use;                /**< Pool allocation tracking flag. */
    } cfuture_threadx_event_t;

    /**
     * @brief Allocates and initializes static ThreadX event handles.
     *
     * @param var_name Variable name prefix for static storage.
     * @param capacity Maximum number of concurrent event handles.
     */
#define CFUTURE_DEFINE_THREADX_EVENTS(var_name, capacity)                                          \
    static cfuture_threadx_event_t var_name##_events[(capacity)];                                  \
    static const size_t var_name##_count = (capacity)

    /**
     * @brief Helper to convert milliseconds to ThreadX timer ticks.
     *
     * @param ms Timeout in milliseconds.
     * @return Ticks corresponding to the millisecond timeout.
     */
    static inline ULONG cfuture_threadx_ms_to_ticks(uint32_t ms)
    {
        if (ms == 0U)
        {
            return TX_NO_WAIT;
        }

        if (ms == UINT32_MAX)
        {
            return TX_WAIT_FOREVER;
        }

        ULONG ticks =
            (ULONG)(((uint64_t)ms * (uint64_t)TX_TIMER_TICKS_PER_SECOND + 999ULL) / 1000ULL);
        return (ticks == 0UL) ? 1UL : ticks;
    }

    /**
     * @brief Creates a ThreadX event handle from a static array.
     *
     * @param events Array of cfuture_threadx_event_t.
     * @param count  Array capacity.
     * @return Pointer to event handle, or NULL if exhausted.
     */
    static inline void *cfuture_threadx_event_create_from(cfuture_threadx_event_t *events,
                                                          size_t count)
    {
        if (!events)
        {
            return NULL;
        }

        for (size_t i = 0; i < count; ++i)
        {
            if (!events[i].in_use)
            {
                events[i].in_use = true;
                UINT status = tx_event_flags_create(&events[i].group, (CHAR *)"cfuture");
                if (status != TX_SUCCESS)
                {
                    events[i].in_use = false;
                    return NULL;
                }

                return &events[i];
            }
        }

        return NULL;
    }

    /**
     * @brief Destroys a ThreadX event handle.
     *
     * @param handle Event handle.
     */
    static inline void cfuture_threadx_event_destroy(void *handle)
    {
        if (!handle)
        {
            return;
        }

        cfuture_threadx_event_t *ev = (cfuture_threadx_event_t *)handle;
        tx_event_flags_delete(&ev->group);
        ev->in_use = false;
    }

    /**
     * @brief Sets the ThreadX event flag (signals waiting thread).
     *
     * @param handle Event handle.
     */
    static inline void cfuture_threadx_event_set(void *handle)
    {
        if (!handle)
        {
            return;
        }

        cfuture_threadx_event_t *ev = (cfuture_threadx_event_t *)handle;
        tx_event_flags_set(&ev->group, CFUTURE_THREADX_FLAG_SIGNALED, TX_OR);
    }

    /**
     * @brief Waits for the ThreadX event flag with a timeout.
     *
     * @param handle     Event handle.
     * @param timeout_ms Timeout in milliseconds.
     * @return true if signaled, false if timed out.
     */
    static inline bool cfuture_threadx_event_wait(void *handle, uint32_t timeout_ms)
    {
        if (!handle)
        {
            return false;
        }

        cfuture_threadx_event_t *ev = (cfuture_threadx_event_t *)handle;
        ULONG actual_flags = 0;
        ULONG ticks = cfuture_threadx_ms_to_ticks(timeout_ms);

        UINT status = tx_event_flags_get(&ev->group, CFUTURE_THREADX_FLAG_SIGNALED, TX_OR_CLEAR,
                                         &actual_flags, ticks);

        return (status == TX_SUCCESS);
    }

    /**
     * @brief Resets the ThreadX event flag.
     *
     * @param handle Event handle.
     */
    static inline void cfuture_threadx_event_reset(void *handle)
    {
        if (!handle)
        {
            return;
        }

        cfuture_threadx_event_t *ev = (cfuture_threadx_event_t *)handle;
        ULONG actual_flags = 0;
        tx_event_flags_get(&ev->group, CFUTURE_THREADX_FLAG_SIGNALED, TX_OR_CLEAR, &actual_flags,
                           TX_NO_WAIT);
    }

    /**
     * @brief Sets the ThreadX event flag from an ISR.
     *
     * In ThreadX, tx_event_flags_set is natively callable from ISRs.
     *
     * @param handle Event handle.
     */
    static inline void cfuture_threadx_event_set_from_isr(void *handle)
    {
        cfuture_threadx_event_set(handle);
    }

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_THREADX_AVAILABLE */

#endif /* CFUTURE_THREADX_H */
