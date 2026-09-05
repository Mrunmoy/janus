/**
 * @file cfuture_polling.c
 * @brief Bare-Metal Polling Adapter Implementation
 *
 * In libcfuture with PAL, state polling is handled directly against slot->state
 * without requiring intermediate event allocations or signaled flags.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_polling.h"

#include <stddef.h>

static const cfuture_sync_ops_t s_polling_ops = {
    .event_create = NULL,
    .event_destroy = NULL,
    .event_set = NULL,
    .event_wait = NULL,
    .event_reset = NULL,
    .event_set_from_isr = NULL,
};

const cfuture_sync_ops_t *cfuture_polling_sync_ops(void)
{
    return &s_polling_ops;
}
