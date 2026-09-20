/**
 * @file cfuture_pal.h
 * @brief Platform Abstraction Layer (PAL) for libcfuture
 *
 * Provides hardware- and platform-level primitives:
 * - Monotonic elapsed time in milliseconds
 * - A CPU relax hint for polling loops (Thumb-2 YIELD on ARM Cortex-M, which executes as a NOP)
 *
 * Designed for microcontrollers and multi-threaded systems with zero dynamic memory allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_PAL_H
#define CFUTURE_PAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Reads the monotonic hardware clock in milliseconds.
     *
     * Times cfuture_wait_for() deadlines in polling mode only. With an OSAL event backend
     * the backend's own timeout is the time base and this clock is not consulted.
     * Default implementations provide POSIX clock_gettime() or Win32 GetTickCount64().
     * On ARM Cortex-M, weakly calls HAL_GetTick() if linked, or advances a monotonic
     * fallback counter to guarantee bounded timeout termination if unlinked. That
     * fallback counts calls, not milliseconds: timeouts still end, but are not accurate.
     * Embedded targets can override this weak function with their own hardware timer.
     *
     * @return Monotonic elapsed time in milliseconds.
     */
    uint32_t cfuture_pal_time_ms(void);

    /**
     * @brief Relaxes the CPU core while waiting for events.
     *
     * Called on every iteration of a polling wait. Defaults: Thumb-2 YIELD on ARM Cortex-M
     * (a NOP hint: it saves no power and does not run an RTOS scheduler; chosen over WFI to
     * avoid its check-then-sleep race), sched_yield() on POSIX, and YieldProcessor() on Win32
     * (a spin-wait pause hint that does not give up the timeslice).
     *
     * Polling mode therefore busy-waits. Under a priority-preemptive RTOS, override this weak
     * function with the RTOS yield/delay call (or inject an event backend) if the producer
     * can be a lower-priority task; otherwise that task never runs while a waiter polls.
     */
    void cfuture_pal_cpu_relax(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_PAL_H */
