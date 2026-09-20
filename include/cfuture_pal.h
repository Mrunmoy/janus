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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Reads the monotonic hardware clock in milliseconds.
     *
     * Times every cfuture_wait_for() deadline in polling mode, and with an OSAL event
     * backend whenever cfuture_pal_clock_is_real() returns true.
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
     * @brief Reports whether cfuture_pal_time_ms() returns real elapsed milliseconds.
     *
     * Default implementations return true on POSIX and Win32, and on ARM Cortex-M only when
     * HAL_GetTick() is linked (otherwise cfuture_pal_time_ms() merely counts calls).
     *
     * cfuture_wait_for() uses this to pick its time base when an OSAL event backend is
     * injected: with a real clock the deadline is timed by the clock and the backend's wait
     * result is only a wakeup hint; without one, the backend's own timeout is the time base.
     * A target that overrides cfuture_pal_time_ms() with a real timer should override this
     * weak function too (return true); leaving it false is safe, it only forgoes the
     * tolerance for backends that return early.
     *
     * @return true if the PAL clock advances in real milliseconds.
     */
    bool cfuture_pal_clock_is_real(void);

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
