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
     * with zero OS #ifdefs in core logic. Every pointer is null-checked: event mode needs
     * event_create, event_wait and event_set; if event_create or event_wait is NULL the pool
     * silently runs in polling mode (PAL clock + cfuture_pal_cpu_relax()).
     *
     * The event MUST latch: a set that arrives before the wait starts must make that wait
     * return (binary-semaphore semantics), because the producer can resolve between the
     * waiter's state check and its event_wait call. Auto-reset and manual-reset events both
     * qualify; manual-reset events additionally need event_reset.
     */
    typedef struct
    {
        /** Allocates/initializes one event per slot; called from cfuture_pool_init(). */
        void *(*event_create)(void);
        /** Destroys/releases an event (optional). */
        void (*event_destroy)(void *event_handle);
        /** Signals the event. Called from the producer task, and from interrupt context when
         *  event_set_from_isr is NULL. Needed whenever event_wait is set. */
        void (*event_set)(void *event_handle);
        /** Waits for the event to be signaled, with timeout in ms (UINT32_MAX = forever).
         *  Returns true if signaled. The backend's timed wait is the library's time base in
         *  event mode: a false return from a finite wait ends cfuture_wait_for() as a timeout
         *  and is not retried, so return false only once timeout_ms has elapsed and absorb
         *  spurious wakeups inside the adapter. A true return with nothing resolved is
         *  treated as a stale signal (reset, then waited on again a bounded number of
         *  times). A false return from a UINT32_MAX wait is retried. */
        bool (*event_wait)(void *event_handle, uint32_t timeout_ms);
        /** Resets the event to unsignaled state. Called from cfuture_create() (any creator
         *  task) before slot reuse, and from the waiting task when a wait reports a signal
         *  although nothing resolved (optional, can be NULL; provide it for manual-reset
         *  events). */
        void (*event_reset)(void *event_handle);
        /** Signals the event from ISR context (optional; falls back to event_set if NULL). */
        void (*event_set_from_isr)(void *event_handle);
    } cfuture_sync_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_OSAL_H */
