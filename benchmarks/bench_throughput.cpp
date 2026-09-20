/**
 * @file bench_throughput.cpp
 * @brief Micro-Benchmarks for cfuture Creation and Fulfillment Latency
 *
 * Measures the uncontended, single-threaded call overhead of two composite cycles, once
 * with the POSIX event backend and once in polling mode:
 *   - create -> abandon -> drop
 *   - create -> set_value -> wait_for (the value is already there, so nothing blocks)
 * These are API call costs, not wake-up latencies.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"
#include "cfuture.h"

#include <chrono>
#include <iomanip>
#include <iostream>

namespace
{

constexpr uint32_t kCapacity = 32;
constexpr uint32_t kIterations = 100000;

void report(const char *label, std::chrono::steady_clock::time_point start)
{
    const int64_t duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
    const double ns_per_op = static_cast<double>(duration_ns) / kIterations;
    std::cout << "  " << label << ": " << std::fixed << std::setprecision(1) << ns_per_op
              << " ns/op (" << static_cast<uint64_t>(1e9 / ns_per_op) << " ops/sec)\n";
}

int runMode(const char *mode, const cfuture_sync_ops_t *ops)
{
    cfuture_slot_t slots[kCapacity]{};
    uint32_t payload_arena[kCapacity]{};
    cfuture_pool_t pool{};

    if (!cfuture_pool_init(&pool, kCapacity, sizeof(uint32_t), slots,
                           reinterpret_cast<uint8_t *>(payload_arena), ops))
    {
        std::cerr << "Failed to initialize pool\n";
        return 1;
    }

    std::cout << "  [" << mode << "]\n";

    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kIterations; ++i)
        {
            cpromise_t p{};
            cfuture_t f{};
            if (cfuture_create(&pool, &p, &f))
            {
                cfuture_abandon(&f);
                cpromise_drop(&p, 0);
            }
        }
        report("Create -> Abandon -> Drop Cycle", start);
    }

    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kIterations; ++i)
        {
            cpromise_t p{};
            cfuture_t f{};
            if (cfuture_create(&pool, &p, &f))
            {
                uint32_t val = i;
                cpromise_set_value(&p, &val, 0);

                uint32_t out_val = 0;
                (void)cfuture_wait_for(&f, 10, &out_val, nullptr);
            }
        }
        report("Create -> Fulfill -> Wait Cycle", start);
    }

    cfuture_pool_destroy(&pool);
    return 0;
}

} // namespace

int main()
{
    std::cout << "====================================================\n";
    std::cout << "  libcfuture Micro-Benchmark (Iterations: " << kIterations << ")\n";
    std::cout << "  Uncontended single-thread call overhead; nothing blocks.\n";
    std::cout << "====================================================\n";

    int rc = runMode("POSIX event backend", cfuture_posix_sync_ops());
    rc |= runMode("Polling mode (no OSAL events)", nullptr);

    std::cout << "====================================================\n";
    return rc;
}
