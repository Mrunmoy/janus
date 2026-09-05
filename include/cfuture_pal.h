/**
 * @file cfuture_pal.h
 * @brief Platform Abstraction Layer (PAL) for libcfuture
 *
 * Provides hardware- and platform-level primitives:
 * - Monotonic elapsed time in milliseconds
 * - Low-power CPU relax / yield (e.g. Thumb-2 YIELD hint on ARM Cortex-M)
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
     * Used for bare-metal polling timeout calculation without an RTOS timer.
     * Default implementations provide POSIX clock_gettime() or Win32 GetTickCount64().
     * On ARM Cortex-M, weakly calls HAL_GetTick() if linked, or advances a monotonic
     * fallback counter to guarantee bounded timeout termination if unlinked.
     * Embedded targets can override this weak function with their own hardware timer.
     *
     * @return Monotonic elapsed time in milliseconds.
     */
    uint32_t cfuture_pal_time_ms(void);

    /**
     * @brief Relaxes the CPU core while waiting for events.
     *
     * On ARM Cortex-M microcontrollers, this executes the Thumb-2 YIELD hint instruction
     * to relax the instruction pipeline without the check-then-sleep race condition
     * of WFI. On host/desktop platforms, this yields execution to other threads.
     */
    void cfuture_pal_cpu_relax(void);

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_PAL_H */
