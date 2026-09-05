/**
 * @file cfuture_polling.h
 * @brief Bare-Metal Polling Synchronization Adapter
 *
 * In libcfuture with PAL, passing NULL for sync_ops in cfuture_pool_init()
 * automatically performs PAL-backed state polling with CPU relax (Thumb-2 YIELD hint).
 *
 * This adapter is provided for backward compatibility with existing code expecting
 * cfuture_polling_sync_ops().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_POLLING_H
#define CFUTURE_POLLING_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Returns the singleton bare-metal polling synchronization operations table.
     *
     * Returns a sync_ops table configured for zero-allocation PAL polling.
     *
     * @return Pointer to static cfuture_sync_ops_t structure.
     */
    const cfuture_sync_ops_t *cfuture_polling_sync_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_POLLING_H */
