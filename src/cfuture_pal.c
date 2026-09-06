/**
 * @file cfuture_pal.c
 * @brief Platform Abstraction Layer (PAL) Default Implementations
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "cfuture_pal.h"

#if defined(__arm__) || defined(__thumb__) || defined(__TARGET_ARCH_ARM) || defined(_M_ARM)
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_6M__) ||           \
    defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__)
#define CFUTURE_PAL_ARM_CORTEX_M 1
#endif
#endif

#if defined(CFUTURE_PAL_ARM_CORTEX_M)

/* Cortex-M: weak symbols allow target board glue to provide HAL_GetTick() or custom timer */
__attribute__((weak)) uint32_t cfuture_pal_time_ms(void)
{
    extern uint32_t HAL_GetTick(void) __attribute__((weak));
    if (HAL_GetTick)
    {
        return HAL_GetTick();
    }

    /* Fallback monotonic counter when no hardware clock is linked:
     * Advances each call so finite timeouts are guaranteed to terminate
     * rather than hanging indefinitely on unfulfilled promises. */
    static uint32_t s_fallback_tick = 0;
    return ++s_fallback_tick;
}

__attribute__((weak)) void cfuture_pal_cpu_relax(void)
{
    /* ARM Thumb-2 YIELD hint: relaxes instruction pipeline without check-then-sleep race */
    __asm__ volatile("yield" ::: "memory");
}

#elif defined(_WIN32) || defined(_WIN64)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint32_t cfuture_pal_time_ms(void)
{
    return (uint32_t)GetTickCount64();
}

void cfuture_pal_cpu_relax(void)
{
    YieldProcessor();
}

#else

/* POSIX / Linux / macOS */
#include <sched.h>
#include <time.h>

__attribute__((weak)) uint32_t cfuture_pal_time_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    {
        return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
    }
    return 0U;
}

__attribute__((weak)) void cfuture_pal_cpu_relax(void)
{
    sched_yield();
}

#endif
