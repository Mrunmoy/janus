/**
 * @file cfuture_polling.h
 * @brief Zero-Heap Bare-Metal Spin-Polling Synchronization Adapter
 *
 * Designed for bare-metal microcontrollers without an RTOS, or simple superloops.
 * Uses atomic flag polling with zero dynamic memory allocation.
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

/** Maximum concurrent polling events supported in the static pool. */
#define CFUTURE_POLL_MAX_EVENTS ((uint32_t)64U)

    /**
     * @brief Returns the singleton bare-metal polling synchronization operations table.
     *
     * @return Pointer to static cfuture_sync_ops_t structure.
     */
    const cfuture_sync_ops_t *cfuture_polling_sync_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_POLLING_H */
