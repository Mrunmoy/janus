/**
 * @file cfuture_posix.h
 * @brief Zero-Heap POSIX Synchronization Adapter for cfuture
 *
 * Provides a production-grade pthread condition-variable backend for host testing
 * and Linux/POSIX targets. Statically pools event primitives with zero dynamic memory.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_POSIX_H
#define CFUTURE_POSIX_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Maximum concurrent POSIX events supported in the static pool. */
#define CFUTURE_POSIX_MAX_EVENTS ((uint32_t)128U)

    /**
     * @brief Returns the singleton POSIX synchronization operations table.
     *
     * @return Pointer to static cfuture_sync_ops_t structure.
     */
    const cfuture_sync_ops_t *cfuture_posix_sync_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_POSIX_H */
