/**
 * @file cfuture_win32.h
 * @brief Native Windows Win32 Event Synchronization Adapter for cfuture
 *
 * Provides a zero-heap synchronization adapter for Windows developers using
 * Win32 kernel event objects (CreateEvent, SetEvent, WaitForSingleObject). Events are
 * manual-reset: they stay signaled until event_reset, which the core calls on slot
 * reuse and when it sees a stale signal during a wait. Handles come from a process-wide
 * table of 64; the table's lazy initialisation is not thread-safe, so initialise the first
 * pool from one thread. On non-Windows builds cfuture_win32_sync_ops() returns NULL, which
 * cfuture_pool_init() treats as polling mode. Not built or tested in this repository
 * (its workflow is Linux-only).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_WIN32_H
#define CFUTURE_WIN32_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Retrieves the singleton Win32 synchronization operations table.
     *
     * @return Pointer to statically allocated cfuture_sync_ops_t table.
     */
    const cfuture_sync_ops_t *cfuture_win32_sync_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_WIN32_H */
