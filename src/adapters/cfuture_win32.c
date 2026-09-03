/**
 * @file cfuture_win32.c
 * @brief Native Windows Win32 Event Synchronization Adapter Implementation
 *
 * Implements cfuture_sync_ops_t using Win32 event primitives.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_win32.h"

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>

#define CFUTURE_WIN32_MAX_EVENTS 64U

typedef struct
{
    HANDLE handle;
    bool in_use;
} cfuture_win32_event_t;

static cfuture_win32_event_t s_win32_events[CFUTURE_WIN32_MAX_EVENTS];
static CRITICAL_SECTION s_win32_cs;
static bool s_win32_cs_init = false;

static void cfuture_win32_ensure_init(void)
{
    if (!s_win32_cs_init)
    {
        InitializeCriticalSection(&s_win32_cs);
        s_win32_cs_init = true;
    }
}

static void *win32_event_create(void)
{
    cfuture_win32_ensure_init();
    EnterCriticalSection(&s_win32_cs);

    for (uint32_t i = 0; i < CFUTURE_WIN32_MAX_EVENTS; ++i)
    {
        if (!s_win32_events[i].in_use)
        {
            HANDLE h = CreateEventA(NULL, TRUE, FALSE, NULL);
            if (!h)
            {
                LeaveCriticalSection(&s_win32_cs);
                return NULL;
            }

            s_win32_events[i].handle = h;
            s_win32_events[i].in_use = true;
            LeaveCriticalSection(&s_win32_cs);
            return &s_win32_events[i];
        }
    }

    LeaveCriticalSection(&s_win32_cs);
    return NULL;
}

static void win32_event_destroy(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_win32_ensure_init();
    EnterCriticalSection(&s_win32_cs);

    cfuture_win32_event_t *ev = (cfuture_win32_event_t *)event_handle;
    if (ev->handle)
    {
        CloseHandle(ev->handle);
        ev->handle = NULL;
    }
    ev->in_use = false;

    LeaveCriticalSection(&s_win32_cs);
}

static void win32_event_set(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_win32_event_t *ev = (cfuture_win32_event_t *)event_handle;
    SetEvent(ev->handle);
}

static bool win32_event_wait(void *event_handle, uint32_t timeout_ms)
{
    if (!event_handle)
    {
        return false;
    }

    cfuture_win32_event_t *ev = (cfuture_win32_event_t *)event_handle;
    DWORD timeout = (timeout_ms == UINT32_MAX) ? INFINITE : (DWORD)timeout_ms;
    DWORD res = WaitForSingleObject(ev->handle, timeout);

    return (res == WAIT_OBJECT_0);
}

static void win32_event_reset(void *event_handle)
{
    if (!event_handle)
    {
        return;
    }

    cfuture_win32_event_t *ev = (cfuture_win32_event_t *)event_handle;
    ResetEvent(ev->handle);
}

static const cfuture_sync_ops_t s_win32_ops = {
    .event_create = win32_event_create,
    .event_destroy = win32_event_destroy,
    .event_set = win32_event_set,
    .event_wait = win32_event_wait,
    .event_reset = win32_event_reset,
    .event_set_from_isr = win32_event_set,
};

const cfuture_sync_ops_t *cfuture_win32_sync_ops(void)
{
    return &s_win32_ops;
}

#else

/* Stub for non-Windows platforms so file can be compiled everywhere cleanly */
const cfuture_sync_ops_t *cfuture_win32_sync_ops(void)
{
    return NULL;
}

#endif /* defined(_WIN32) || defined(_WIN64) */
