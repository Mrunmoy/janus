/**
 * @file cfuture_posix.c
 * @brief Zero-Heap POSIX Synchronization Adapter Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "adapters/cfuture_posix.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define CFUTURE_POSIX_MAX_SPURIOUS_WAKEUPS ((uint32_t)1000U)

typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool signaled;
    bool in_use;
} cfuture_posix_event_t;

static cfuture_posix_event_t s_events[CFUTURE_POSIX_MAX_EVENTS];
static pthread_mutex_t s_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_initialized = false;

/**
 * @brief Idempotently initializes the static POSIX event pool.
 */
static void cfuture_posix_ensure_init(void)
{
    if (!s_initialized)
    {
        for (uint32_t i = 0; i < CFUTURE_POSIX_MAX_EVENTS; ++i)
        {
            pthread_mutex_init(&s_events[i].mutex, NULL);
            pthread_cond_init(&s_events[i].cond, NULL);
            s_events[i].signaled = false;
            s_events[i].in_use = false;
        }

        s_initialized = true;
    }
}

/**
 * @brief Calculates an absolute timespec deadline from a relative millisecond duration.
 *
 * @param timeout_ms Relative timeout in milliseconds.
 * @return Absolute timespec deadline.
 */
static struct timespec cfuture_posix_calc_deadline(uint32_t timeout_ms)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_sec += (time_t)(timeout_ms / 1000U);
    ts.tv_nsec += (long)((timeout_ms % 1000U) * 1000000UL);

    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    return ts;
}

/**
 * @brief Performs a bounded timed condition wait against a target timespec.
 *
 * @param ev The event primitive.
 * @param ts The absolute deadline.
 * @return True if signaled before deadline, false otherwise.
 */
static bool cfuture_posix_timed_wait_loop(cfuture_posix_event_t *ev, const struct timespec *ts)
{
    for (uint32_t wakeups = 0; wakeups < CFUTURE_POSIX_MAX_SPURIOUS_WAKEUPS; ++wakeups)
    {
        if (ev->signaled)
        {
            ev->signaled = false;
            return true;
        }

        int ret = pthread_cond_timedwait(&ev->cond, &ev->mutex, ts);
        if (ret != 0)
        {
            break;
        }
    }

    if (ev->signaled)
    {
        ev->signaled = false;
        return true;
    }

    return false;
}

static void *posix_event_create(void)
{
    pthread_mutex_lock(&s_pool_mutex);
    cfuture_posix_ensure_init();

    void *handle = NULL;
    for (uint32_t i = 0; i < CFUTURE_POSIX_MAX_EVENTS; ++i)
    {
        if (!s_events[i].in_use)
        {
            s_events[i].in_use = true;
            s_events[i].signaled = false;
            handle = &s_events[i];
            break;
        }
    }

    pthread_mutex_unlock(&s_pool_mutex);
    return handle;
}

static void posix_event_destroy(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    pthread_mutex_lock(&s_pool_mutex);

    cfuture_posix_event_t *ev = (cfuture_posix_event_t *)event_handle;
    ev->signaled = false;
    ev->in_use = false;

    pthread_mutex_unlock(&s_pool_mutex);
}

static void posix_event_set(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_posix_event_t *ev = (cfuture_posix_event_t *)event_handle;

    pthread_mutex_lock(&ev->mutex);
    ev->signaled = true;
    pthread_cond_broadcast(&ev->cond);
    pthread_mutex_unlock(&ev->mutex);
}

static bool posix_event_wait(void *event_handle, uint32_t timeout_ms)
{
    if (!event_handle)
    {
        return false;
    }

    cfuture_posix_event_t *ev = (cfuture_posix_event_t *)event_handle;

    pthread_mutex_lock(&ev->mutex);

    if (ev->signaled)
    {
        ev->signaled = false;
        pthread_mutex_unlock(&ev->mutex);
        return true;
    }

    if (timeout_ms == 0U)
    {
        pthread_mutex_unlock(&ev->mutex);
        return false;
    }

    /* Fallback default deadline if waiting indefinitely: 10 minutes max */
    uint32_t effective_ms = (timeout_ms == UINT32_MAX) ? 600000U : timeout_ms;
    struct timespec ts = cfuture_posix_calc_deadline(effective_ms);

    bool success = cfuture_posix_timed_wait_loop(ev, &ts);

    pthread_mutex_unlock(&ev->mutex);
    return success;
}

static void posix_event_reset(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_posix_event_t *ev = (cfuture_posix_event_t *)event_handle;

    pthread_mutex_lock(&ev->mutex);
    ev->signaled = false;
    pthread_mutex_unlock(&ev->mutex);
}

static const cfuture_sync_ops_t s_posix_ops = {
    .event_create = posix_event_create,
    .event_destroy = posix_event_destroy,
    .event_set = posix_event_set,
    .event_wait = posix_event_wait,
    .event_reset = posix_event_reset,
    .event_set_from_isr = posix_event_set,
};

const cfuture_sync_ops_t *cfuture_posix_sync_ops(void)
{
    return &s_posix_ops;
}
