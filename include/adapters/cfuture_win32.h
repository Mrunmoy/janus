/**
 * @file cfuture_win32.h
 * @brief Native Windows Win32 Event Synchronization Adapter for cfuture
 *
 * Provides a zero-heap synchronization adapter for Windows developers using
 * Win32 kernel event objects (CreateEvent, SetEvent, WaitForSingleObject).
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
