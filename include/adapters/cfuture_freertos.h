/**
 * @file cfuture_freertos.h
 * @brief Zero-Heap FreeRTOS EventGroup Synchronization Adapter for cfuture
 *
 * Implements an OSAL synchronization backend using FreeRTOS EventGroups.
 * Compatible with FreeRTOS 9.x, 10.x, and CMSIS-RTOS2 FreeRTOS wrapper.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_FREERTOS_H
#define CFUTURE_FREERTOS_H

#include "cfuture.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(INC_FREERTOS_H) || defined(FREERTOS_H)
#include "event_groups.h"

#define CFUTURE_FREERTOS_EVENT_BIT ((EventBits_t)0x01U)

static inline void *cfuture_freertos_event_create(void)
{
    return (void *)xEventGroupCreate();
}

static inline void cfuture_freertos_event_destroy(void *event_handle)
{
    if (event_handle)
    {
        vEventGroupDelete((EventGroupHandle_t)event_handle);
    }
}

static inline void cfuture_freertos_event_set(void *event_handle)
{
    if (event_handle)
    {
        xEventGroupSetBits((EventGroupHandle_t)event_handle, CFUTURE_FREERTOS_EVENT_BIT);
    }
}

static inline void cfuture_freertos_event_set_from_isr(void *event_handle)
{
    if (event_handle)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xEventGroupSetBitsFromISR((EventGroupHandle_t)event_handle, CFUTURE_FREERTOS_EVENT_BIT,
                                  &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static inline bool cfuture_freertos_event_wait(void *event_handle, uint32_t timeout_ms)
{
    if (!event_handle)
    {
        return false;
    }

    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits((EventGroupHandle_t)event_handle,
                                           CFUTURE_FREERTOS_EVENT_BIT, pdTRUE, pdFALSE, ticks);

    return (bits & CFUTURE_FREERTOS_EVENT_BIT) != 0U;
}

static inline void cfuture_freertos_event_reset(void *event_handle)
{
    if (event_handle)
    {
        xEventGroupClearBits((EventGroupHandle_t)event_handle, CFUTURE_FREERTOS_EVENT_BIT);
    }
}

static inline cfuture_sync_ops_t cfuture_freertos_sync_ops(void)
{
    cfuture_sync_ops_t ops = {
        .event_create = cfuture_freertos_event_create,
        .event_destroy = cfuture_freertos_event_destroy,
        .event_set = cfuture_freertos_event_set,
        .event_wait = cfuture_freertos_event_wait,
        .event_reset = cfuture_freertos_event_reset,
        .event_set_from_isr = cfuture_freertos_event_set_from_isr,
    };

    return ops;
}

#endif /* INC_FREERTOS_H */

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_FREERTOS_H */
