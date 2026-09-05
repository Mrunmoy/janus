/**
 * @file cfuture_osal.h
 * @brief Operating System Abstraction Layer (OSAL) for libcfuture
 *
 * Defines the pluggable synchronization operations table injected into
 * cfuture_pool_t for thread sleeping and scheduling under an RTOS or host OS.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_OSAL_H
#define CFUTURE_OSAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Pluggable OSAL synchronization interface table (Dependency Injection).
     *
     * Injects platform synchronization primitives (POSIX, FreeRTOS, ThreadX, Zephyr)
     * with zero #ifdefs in core logic. When NULL, cfuture operates in bare-metal
     * mode using the Platform Abstraction Layer (PAL).
     */
    typedef struct
    {
        /** Allocates/initializes a synchronization primitive. */
        void *(*event_create)(void);
        /** Destroys/releases a synchronization primitive. */
        void (*event_destroy)(void *event_handle);
        /** Signals the event from task context. */
        void (*event_set)(void *event_handle);
        /** Waits for the event to be signaled, with timeout in ms. Returns true if signaled. */
        bool (*event_wait)(void *event_handle, uint32_t timeout_ms);
        /** Resets the event to unsignaled state prior to slot reuse (optional, can be NULL). */
        void (*event_reset)(void *event_handle);
        /** Signals the event from ISR context (optional; falls back to event_set if NULL). */
        void (*event_set_from_isr)(void *event_handle);
    } cfuture_sync_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_OSAL_H */
