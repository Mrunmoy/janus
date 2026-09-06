/**
 * @file cfuture.h
 * @brief Zero-Heap Lock-Free Future/Promise Framework for Embedded C
 *
 * Provides a static-pool-based future/promise concurrency abstraction designed
 * for resource-constrained microcontrollers and multi-threaded systems. Eliminates
 * dangling stack pointers across message queues and guarantees immediate non-blocking
 * timeout unwinding.
 *
 * Key guarantees:
 * - Zero dynamic memory allocation (0 bytes malloc/free).
 * - Compile-time static bounds: MAX_SLOTS capacity enforced via bitmask.
 * - Lock-free, non-blocking single-slot acquisition via atomic CAS on bitmask.
 * - Dual-owner reference tracking (2 -> 1 -> 0) preventing premature slot recycling
 *   while the producer is signaling the consumer event.
 * - Immediate non-blocking timeout unwinding: consumer marks TIMEOUT and exits
 *   instantly without spinning or blocking; deferred slot recycling is safely
 *   handled by the producer upon completion.
 * - Asynchronous ISR safety: promises can be fulfilled directly from hardware
 *   interrupt service routines via dedicated cpromise_*_from_isr() APIs.
 * - Platform Abstraction Layer (PAL) for monotonic time and CPU relax hints.
 * - Operating System Abstraction Layer (OSAL) for pluggable RTOS/Host synchronization.
 * - Strict C11 / C++17 compatibility.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CFUTURE_H
#define CFUTURE_H

#ifdef __cplusplus
#include <atomic>
#include <cstddef>
#include <cstdint>
typedef std::atomic<uint_fast32_t> cfuture_atomic_uint_fast32_t;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__STDC_NO_ATOMICS__)
#error "cfuture requires C11 atomic support or compiler atomic builtins"
#else
#include <stdatomic.h>
typedef atomic_uint_fast32_t cfuture_atomic_uint_fast32_t;
#endif
#endif

#include "cfuture_osal.h"
#include "cfuture_pal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Maximum supported capacity in a single pool (limited by 32-bit atomic mask). */
#define CFUTURE_MAX_CAPACITY ((uint32_t)32U)

/** Sentinel value representing an invalid or released slot ID. */
#define CFUTURE_INVALID_SLOT ((uint8_t)0xFFU)

/** Status and error codes. */
#define CFUTURE_OK ((int32_t)0)
#define CFUTURE_ERR_TIMEOUT ((int32_t)-1)
#define CFUTURE_ERR_DROPPED ((int32_t)-2)
#define CFUTURE_ERR_ABANDONED ((int32_t)-3)
#define CFUTURE_ERR_PARAM ((int32_t)-4)
#define CFUTURE_ERR_FULL ((int32_t)-5)
#define CFUTURE_ERR_INVALID ((int32_t)-6)

    /**
     * @brief Internal lifecycle states of a pool slot.
     */
    typedef enum
    {
        CFUTURE_STATE_IDLE = 0,      /**< Slot is free and unallocated in pool. */
        CFUTURE_STATE_PENDING = 1,   /**< Slot is allocated; worker has not fulfilled yet. */
        CFUTURE_STATE_COMPLETED = 2, /**< Worker successfully fulfilled with a value. */
        CFUTURE_STATE_DROPPED = 3,   /**< Worker dropped the promise without a value. */
        CFUTURE_STATE_TIMEOUT = 4,   /**< Caller timed out while waiting. */
        CFUTURE_STATE_ABANDONED = 5  /**< Caller explicitly abandoned the future. */
    } cfuture_state_t;

    /**
     * @brief Single slot metadata within the static future/promise pool.
     */
    typedef struct
    {
        cfuture_atomic_uint_fast32_t ref_count; /**< Dual-owner refcount: 2 -> 1 -> 0. */
        cfuture_atomic_uint_fast32_t state;     /**< Current state (cfuture_state_t). */
        int32_t status_code; /**< Result status code (0 = success / CFUTURE_OK). */
        void *event_handle;  /**< Injected OSAL synchronization handle. */
        uint8_t *payload;    /**< Pointer into pool payload arena. */
    } cfuture_slot_t;

    /* Forward declaration of pool container. */
    typedef struct cfuture_pool cfuture_pool_t;

    /**
     * @brief Static Future/Promise pool container.
     */
    struct cfuture_pool
    {
        uint32_t capacity;   /**< Max concurrent slots (1..32). */
        size_t payload_size; /**< Payload buffer size per slot in bytes. */
        cfuture_atomic_uint_fast32_t
            allocated_mask;          /**< Atomic bitmask of occupied slot indices. */
        cfuture_sync_ops_t sync_ops; /**< Injected OSAL synchronization table. */
        cfuture_slot_t *slots;       /**< Caller-provided array of slots [capacity]. */
        uint8_t *payload_arena;      /**< Caller-provided arena [capacity * payload_size]. */
    };

    /**
     * @brief Producer handle passed to the Worker task or ISR.
     */
    typedef struct
    {
        uint8_t slot_id;      /**< Index into pool->slots array, or CFUTURE_INVALID_SLOT. */
        cfuture_pool_t *pool; /**< Pointer to originating pool, or NULL if consumed/invalid. */
    } cpromise_t;

    /**
     * @brief Consumer handle held by the Caller task.
     */
    typedef struct
    {
        uint8_t slot_id;      /**< Index into pool->slots array, or CFUTURE_INVALID_SLOT. */
        cfuture_pool_t *pool; /**< Pointer to originating pool, or NULL if consumed/invalid. */
    } cfuture_t;

    /**
     * @brief Initializes a future pool with caller-provided static memory buffers.
     *
     * @param[out] pool         Pointer to pool struct to initialize.
     * @param[in]  capacity     Number of concurrent slots (1..32).
     * @param[in]  payload_size Size of result data per slot in bytes (can be 0).
     * @param[in]  slots_buf    Caller-provided array of cfuture_slot_t of length capacity.
     * @param[in]  payload_buf  Caller-provided byte buffer of size capacity * payload_size (can be
     * NULL if payload_size == 0).
     * @param[in]  sync_ops     Pointer to OSAL interface table, or NULL for PAL polling mode.
     * @return true on success, false if parameters are invalid.
     */
    bool cfuture_pool_init(cfuture_pool_t *pool, uint32_t capacity, size_t payload_size,
                           cfuture_slot_t *slots_buf, uint8_t *payload_buf,
                           const cfuture_sync_ops_t *sync_ops);

    /**
     * @brief Destroys a future pool and cleans up injected synchronization handles.
     *
     * @param[in,out] pool Pointer to initialized pool struct.
     */
    void cfuture_pool_destroy(cfuture_pool_t *pool);

    /**
     * @brief Atomically creates a connected Promise/Future pair from the pool.
     *
     * @param[in,out] pool        The future pool instance.
     * @param[out]    out_promise Receives the producer handle.
     * @param[out]    out_future  Receives the consumer handle.
     * @return true if slot was successfully allocated, false if pool is full or params invalid.
     */
    bool cfuture_create(cfuture_pool_t *pool, cpromise_t *out_promise, cfuture_t *out_future);

    /**
     * @brief Waits for the worker to fulfill the promise within a timeout.
     *
     * @param[in,out] future      The future handle. Invalidated upon return.
     * @param[in]     timeout_ms  Timeout in milliseconds (0 = non-blocking, UINT32_MAX = forever).
     * @param[out]    out_payload Buffer to copy result payload into (optional, can be NULL).
     * @param[out]    out_status  Receives status code (0 = success) or error code (optional, can be
     * NULL).
     * @return true if completed successfully, false if timed out, dropped, or invalid.
     */
    bool cfuture_wait_for(cfuture_t *future, uint32_t timeout_ms, void *out_payload,
                          int32_t *out_status);

    /**
     * @brief Explicitly abandons a future without waiting.
     *
     * @param[in,out] future The future handle. Invalidated upon return.
     */
    void cfuture_abandon(cfuture_t *future);

    /**
     * @brief Checks if the consumer is still actively waiting for the promise.
     *
     * @param[in] promise The promise handle.
     * @return true if caller is still waiting, false if caller timed out or abandoned.
     */
    bool cpromise_is_active(const cpromise_t *promise);

    /**
     * @brief Fulfills the promise with a result value and notifies the consumer.
     *
     * @param[in,out] promise    The promise handle. Invalidated upon return.
     * @param[in]     payload    Result data to copy into pool slot (optional if payload_size == 0).
     * @param[in]     status_code Status code (0 = success / CFUTURE_OK).
     */
    void cpromise_set_value(cpromise_t *promise, const void *payload, int32_t status_code);

    /**
     * @brief Drops the promise without fulfilling (fails the waiting consumer).
     *
     * @param[in,out] promise    The promise handle. Invalidated upon return.
     * @param[in]     status_code Failure reason or status code to store (e.g. CFUTURE_ERR_DROPPED).
     */
    void cpromise_drop(cpromise_t *promise, int32_t status_code);

    /**
     * @brief Fulfills the promise from an Interrupt Service Routine (ISR).
     *
     * @param[in,out] promise    The promise handle. Invalidated upon return.
     * @param[in]     payload    Result data to copy into pool slot (optional if payload_size == 0).
     * @param[in]     status_code Status code (0 = success / CFUTURE_OK).
     */
    void cpromise_set_value_from_isr(cpromise_t *promise, const void *payload, int32_t status_code);

    /**
     * @brief Drops the promise from an Interrupt Service Routine (ISR).
     *
     * @param[in,out] promise    The promise handle. Invalidated upon return.
     * @param[in]     status_code Failure reason or status code to store (e.g. CFUTURE_ERR_DROPPED).
     */
    void cpromise_drop_from_isr(cpromise_t *promise, int32_t status_code);

/**
 * @brief Helper macro to allocate static storage buffers for a pool.
 */
#define CFUTURE_DEFINE_STATIC_BUFFERS(pool_name, payload_type, capacity)                           \
    static cfuture_slot_t pool_name##_slots[(capacity)];                                           \
    static uint8_t pool_name##_payload[(capacity) * sizeof(payload_type)]

/**
 * @brief Macro generating type-safe wrapper functions for a subsystem future/promise.
 */
#define CFUTURE_DEFINE_TYPED_POOL(subsystem_name, payload_type, pool_capacity)                     \
    typedef struct                                                                                 \
    {                                                                                              \
        uint8_t slot_id;                                                                           \
        cfuture_pool_t *pool;                                                                      \
    } subsystem_name##_future_t;                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        uint8_t slot_id;                                                                           \
        cfuture_pool_t *pool;                                                                      \
    } subsystem_name##_promise_t;                                                                  \
    static inline bool subsystem_name##_create(                                                    \
        cfuture_pool_t *pool, subsystem_name##_promise_t *p, subsystem_name##_future_t *f)         \
    {                                                                                              \
        return cfuture_create(pool, (cpromise_t *)p, (cfuture_t *)f);                              \
    }                                                                                              \
    static inline bool subsystem_name##_future_wait(subsystem_name##_future_t *f,                  \
                                                    uint32_t timeout_ms, payload_type *out_val,    \
                                                    int32_t *out_status)                           \
    {                                                                                              \
        return cfuture_wait_for((cfuture_t *)f, timeout_ms, (void *)out_val, out_status);          \
    }                                                                                              \
    static inline void subsystem_name##_future_abandon(subsystem_name##_future_t *f)               \
    {                                                                                              \
        cfuture_abandon((cfuture_t *)f);                                                           \
    }                                                                                              \
    static inline bool subsystem_name##_promise_is_active(const subsystem_name##_promise_t *p)     \
    {                                                                                              \
        return cpromise_is_active((const cpromise_t *)p);                                          \
    }                                                                                              \
    static inline void subsystem_name##_promise_set(subsystem_name##_promise_t *p,                 \
                                                    const payload_type *val, int32_t status_code)  \
    {                                                                                              \
        cpromise_set_value((cpromise_t *)p, (const void *)val, status_code);                       \
    }                                                                                              \
    static inline void subsystem_name##_promise_drop(subsystem_name##_promise_t *p,                \
                                                     int32_t status_code)                          \
    {                                                                                              \
        cpromise_drop((cpromise_t *)p, status_code);                                               \
    }                                                                                              \
    static inline void subsystem_name##_promise_set_from_isr(                                      \
        subsystem_name##_promise_t *p, const payload_type *val, int32_t status_code)               \
    {                                                                                              \
        cpromise_set_value_from_isr((cpromise_t *)p, (const void *)val, status_code);              \
    }                                                                                              \
    static inline void subsystem_name##_promise_drop_from_isr(subsystem_name##_promise_t *p,       \
                                                              int32_t status_code)                 \
    {                                                                                              \
        cpromise_drop_from_isr((cpromise_t *)p, status_code);                                      \
    }

#ifdef __cplusplus
}
#endif

#endif /* CFUTURE_H */
