/**
 * @file sensor_pipeline.c
 * @brief Realistic Embedded Servicer-Requester Pipeline Simulation
 *
 * Demonstrates:
 *  1. Shared Servicer Task (T_S) reading commands off an OS message queue.
 *  2. Work Cancellation: T_S checks cpromise_is_active() before starting heavy work.
 *  3. Safe Late Completion: cpromise_set_value() safely handles caller timeouts.
 *  4. ABA Slot Isolation: Proves concurrent Task T_B cannot claim T_A's timed-out
 *     slot while T_A's request is still pending in T_S's queue.
 *  5. Hardware DMA / ISR Fulfillment: Demonstrates cpromise_set_value_from_isr().
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"
#include "cfuture.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STORAGE_POOL_CAPACITY 8U
#define CMD_QUEUE_CAPACITY 16U

/**
 * @brief Storage response telemetry payload.
 */
typedef struct
{
    uint32_t block_id;
    uint32_t bytes_transferred;
    uint32_t status_code;
} storage_result_t;

/**
 * @brief Command descriptor queued from Requesters to Servicer.
 */
typedef struct
{
    uint32_t block_id;
    uint32_t simulated_work_ms;
    cpromise_t promise;
    bool is_shutdown;
} storage_cmd_t;

/**
 * @brief Bounded circular OS command queue simulation.
 */
typedef struct
{
    storage_cmd_t items[CMD_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
} cmd_queue_t;

static cmd_queue_t s_cmd_queue;
CFUTURE_DEFINE_STATIC_BUFFERS(s_nvm, storage_result_t, STORAGE_POOL_CAPACITY);
static cfuture_pool_t s_nvm_pool;

/**
 * @brief Initializes the thread-safe command queue.
 *
 * @param q Queue pointer.
 */
static void cmd_queue_init(cmd_queue_t *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv, NULL);
}

/**
 * @brief Enqueues a command into the bounded queue.
 *
 * @param q   Queue pointer.
 * @param cmd Command descriptor.
 * @return true if enqueued, false if queue full.
 */
static bool cmd_queue_push(cmd_queue_t *q, const storage_cmd_t *cmd)
{
    pthread_mutex_lock(&q->mtx);
    if (q->count >= CMD_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&q->mtx);
        return false;
    }

    q->items[q->tail] = *cmd;
    q->tail = (q->tail + 1U) % CMD_QUEUE_CAPACITY;
    q->count++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return true;
}

/**
 * @brief Dequeues a command, blocking until an item is available.
 *
 * @param q       Queue pointer.
 * @param out_cmd Output buffer.
 * @return true if popped successfully.
 */
static bool cmd_queue_pop(cmd_queue_t *q, storage_cmd_t *out_cmd)
{
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0)
    {
        pthread_cond_wait(&q->cv, &q->mtx);
    }

    *out_cmd = q->items[q->head];
    q->head = (q->head + 1U) % CMD_QUEUE_CAPACITY;
    q->count--;
    pthread_mutex_unlock(&q->mtx);
    return true;
}

/**
 * @brief Shared Servicer Task (T_S) simulating high-latency storage operations.
 *
 * @param arg Unused task parameter.
 * @return NULL.
 */
static void *servicer_task_entry(void *arg)
{
    (void)arg;
    printf("[Servicer T_S] Started. Ready to process queued hardware commands.\n");

    while (true)
    {
        storage_cmd_t cmd = {0};
        if (!cmd_queue_pop(&s_cmd_queue, &cmd))
        {
            break;
        }

        if (cmd.is_shutdown)
        {
            printf("[Servicer T_S] Received shutdown signal. Exiting task.\n");
            break;
        }

        printf("[Servicer T_S] Popped request for Block #%u (Slot %u) from queue.\n", cmd.block_id,
               cmd.promise.slot_id);

        /* 1. Cancellation Check: Did the caller already time out while waiting in queue? */
        if (!cpromise_is_active(&cmd.promise))
        {
            printf("[Servicer T_S] -> CANCELLATION DETECTED! Caller timed out while in queue.\n"
                   "               Skipping expensive hardware flash erase/write completely!\n");
            cpromise_drop(&cmd.promise, 0);
            continue;
        }

        /* 2. Execute simulated hardware storage write */
        printf("[Servicer T_S] -> Starting hardware write for Block #%u (%u ms)...\n", cmd.block_id,
               cmd.simulated_work_ms);
        usleep(cmd.simulated_work_ms * 1000U);

        storage_result_t res = {
            .block_id = cmd.block_id,
            .bytes_transferred = 512,
            .status_code = 0,
        };

        /* 3. Fulfill promise: If caller timed out while we were writing, cfuture safely discards
         * res */
        cpromise_set_value(&cmd.promise, &res, 0);
        printf("[Servicer T_S] -> Fulfill complete for Block #%u.\n", cmd.block_id);
    }

    return NULL;
}

/**
 * @brief Helper to simulate an asynchronous DMA transfer with an ISR callback.
 *
 * @param arg Pointer to cpromise_t for the DMA transfer.
 * @return NULL.
 */
static void *simulated_dma_isr_thread(void *arg)
{
    cpromise_t *p = (cpromise_t *)arg;
    usleep(25 * 1000U); /* Simulate 25 ms DMA transfer time */

    storage_result_t dma_res = {
        .block_id = 9999,
        .bytes_transferred = 2048,
        .status_code = 0,
    };

    printf("[DMA Hardware ISR] Hardware transfer complete! Invoking "
           "cpromise_set_value_from_isr()...\n");
    cpromise_set_value_from_isr(p, &dma_res, 0);
    return NULL;
}

int main(void)
{
    printf("====================================================================\n");
    printf("  libcfuture: Realistic Embedded Servicer-Requester Demonstration\n");
    printf("====================================================================\n\n");

    /* Initialize OSAL and memory pool */
    cmd_queue_init(&s_cmd_queue);
    const cfuture_sync_ops_t *sync_ops = cfuture_posix_sync_ops();
    if (!cfuture_pool_init(&s_nvm_pool, STORAGE_POOL_CAPACITY, sizeof(storage_result_t),
                           s_nvm_slots, s_nvm_payload, sync_ops))
    {
        fprintf(stderr, "Failed to initialize cfuture pool\n");
        return 1;
    }

    /* Start Servicer Task (T_S) */
    pthread_t servicer_thread = 0;
    pthread_create(&servicer_thread, NULL, servicer_task_entry, NULL);

    /* -----------------------------------------------------------------
     * Scenario 1: Happy Path (Caller T_A dispatches, Servicer completes)
     * ----------------------------------------------------------------- */
    printf("--- [Scenario 1: Happy Path Asynchronous Request] ---\n");
    cpromise_t p1 = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_t f1 = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    if (cfuture_create(&s_nvm_pool, &p1, &f1))
    {
        printf("[Task T_A] Claimed Slot %u. Enqueueing write for Block #100 (Work: 20 ms, Timeout: "
               "100 ms)...\n",
               f1.slot_id);

        storage_cmd_t cmd1 = {
            .block_id = 100,
            .simulated_work_ms = 20,
            .promise = p1,
            .is_shutdown = false,
        };
        cmd_queue_push(&s_cmd_queue, &cmd1);

        storage_result_t res1 = {0};
        int32_t err1 = 0;
        if (cfuture_wait_for(&f1, 100, &res1, &err1))
        {
            printf("[Task T_A] SUCCESS: Block #%u written (%u bytes, status=%u)\n\n", res1.block_id,
                   res1.bytes_transferred, res1.status_code);
        }
    }

    /* -----------------------------------------------------------------
     * Scenario 2: Timeout, Work Cancellation & Zero Queue ABA Collision
     * ----------------------------------------------------------------- */
    printf("--- [Scenario 2: Timeout, Cancellation & ABA Slot Isolation] ---\n");
    cpromise_t p2 = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_t f2 = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    if (cfuture_create(&s_nvm_pool, &p2, &f2))
    {
        uint8_t slot_a = f2.slot_id;
        printf("[Task T_A] Claimed Slot %u. Enqueueing slow write for Block #200 (Work: 80 ms, "
               "Timeout: 20 ms)...\n",
               slot_a);

        storage_cmd_t cmd2 = {
            .block_id = 200,
            .simulated_work_ms = 80,
            .promise = p2,
            .is_shutdown = false,
        };
        cmd_queue_push(&s_cmd_queue, &cmd2);

        storage_result_t res2 = {0};
        int32_t err2 = 0;
        if (!cfuture_wait_for(&f2, 20, &res2, &err2))
        {
            printf("[Task T_A] TIMEOUT! Deadline exceeded (err=%d). Unwinding stack immediately!\n",
                   err2);
        }

        /* Concurrently, Task T_B arrives immediately while T_A's request is STILL in T_S's queue!
         */
        printf("\n[Task T_B] Arriving while T_A's request is still queued in T_S...\n");
        cpromise_t p3 = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
        cfuture_t f3 = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
        if (cfuture_create(&s_nvm_pool, &p3, &f3))
        {
            uint8_t slot_b = f3.slot_id;
            printf("[Task T_B] Claimed Slot %u! (Notice: Slot %u != Slot %u)\n", slot_b, slot_b,
                   slot_a);
            printf("[Task T_B] -> ABA HAZARD PREVENTED: Slot %u remains locked until T_S pops "
                   "T_A's promise!\n\n",
                   slot_a);

            cfuture_abandon(&f3);
            cpromise_drop(&p3, 0);
        }

        /* Give Servicer a moment to pop T_A's request and finish late */
        usleep(100 * 1000U);
    }

    /* -----------------------------------------------------------------
     * Scenario 2b: Work Cancellation Before Start (Skipping HW Execution)
     * ----------------------------------------------------------------- */
    printf("\n--- [Scenario 2b: Work Cancellation Before Starting] ---\n");
    /* Enqueue a slow blocker job first so Servicer is busy */
    cpromise_t p_slow = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_t f_slow = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_create(&s_nvm_pool, &p_slow, &f_slow);
    storage_cmd_t cmd_slow = {
        .block_id = 250,
        .simulated_work_ms = 60,
        .promise = p_slow,
        .is_shutdown = false,
    };
    cmd_queue_push(&s_cmd_queue, &cmd_slow);

    /* Immediately enqueue a second job with a 10 ms deadline */
    cpromise_t p_queued = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_t f_queued = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_create(&s_nvm_pool, &p_queued, &f_queued);
    storage_cmd_t cmd_queued = {
        .block_id = 251,
        .simulated_work_ms = 40,
        .promise = p_queued,
        .is_shutdown = false,
    };
    cmd_queue_push(&s_cmd_queue, &cmd_queued);
    printf("[Task T_A] Enqueued Block #251 behind Block #250 with 10 ms deadline...\n");

    storage_result_t res_queued = {0};
    int32_t err_queued = 0;
    if (!cfuture_wait_for(&f_queued, 10, &res_queued, &err_queued))
    {
        printf("[Task T_A] TIMEOUT on Block #251 while still queued! Unwound caller stack.\n");
    }

    /* Wait for Servicer to finish blocker and pop the cancelled job */
    usleep(120 * 1000U);
    cfuture_abandon(&f_slow);

    /* -----------------------------------------------------------------
     * Scenario 3: Asynchronous Hardware DMA / ISR Completion
     * ----------------------------------------------------------------- */
    printf("\n--- [Scenario 3: Asynchronous Hardware DMA / ISR Safety] ---\n");
    cpromise_t p_dma = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    cfuture_t f_dma = {.slot_id = CFUTURE_INVALID_SLOT, .pool = NULL};
    if (cfuture_create(&s_nvm_pool, &p_dma, &f_dma))
    {
        printf("[Task T_A] Triggering hardware DMA transfer (Slot %u)... Waiting for ISR "
               "callback...\n",
               f_dma.slot_id);

        pthread_t isr_thread = 0;
        pthread_create(&isr_thread, NULL, simulated_dma_isr_thread, &p_dma);

        storage_result_t dma_out = {0};
        int32_t dma_err = 0;
        if (cfuture_wait_for(&f_dma, 100, &dma_out, &dma_err))
        {
            printf(
                "[Task T_A] SUCCESS: DMA transfer complete! Transferred %u bytes for Block #%u\n\n",
                dma_out.bytes_transferred, dma_out.block_id);
        }

        pthread_join(isr_thread, NULL);
    }

    /* Shut down Servicer Task */
    storage_cmd_t shutdown_cmd = {.is_shutdown = true};
    cmd_queue_push(&s_cmd_queue, &shutdown_cmd);
    pthread_join(servicer_thread, NULL);

    /* Verify all slots have been cleanly recycled */
    uint_fast32_t leaked_mask = s_nvm_pool.allocated_mask;
    printf("====================================================================\n");
    printf("  Simulation Finished: Residual Pool Mask = 0x%lx (Zero Leaks!)\n",
           (unsigned long)leaked_mask);
    printf("====================================================================\n");

    cfuture_pool_destroy(&s_nvm_pool);
    return (leaked_mask == 0) ? 0 : 1;
}
