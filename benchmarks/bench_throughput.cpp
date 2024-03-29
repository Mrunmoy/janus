/**
 * @file bench_throughput.cpp
 * @brief Micro-Benchmarks for cfuture Creation and Fulfillment Latency
 *
 * Measures nanoseconds per allocation, fulfill, and wait operation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"
#include "cfuture.h"

#include <chrono>
#include <iomanip>
#include <iostream>

int main()
{
    constexpr uint32_t kCapacity = 32;
    constexpr uint32_t kIterations = 100000;

    cfuture_slot_t slots[kCapacity];
    uint32_t payload_arena[kCapacity];
    cfuture_pool_t pool;

    const cfuture_sync_ops_t *posix_ops = cfuture_posix_sync_ops();
    if (!cfuture_pool_init(&pool, kCapacity, sizeof(uint32_t), slots,
                           reinterpret_cast<uint8_t *>(payload_arena), posix_ops))
    {
        std::cerr << "Failed to initialize pool\n";
        return 1;
    }

    std::cout << "====================================================\n";
    std::cout << "  libcfuture Micro-Benchmark (Iterations: " << kIterations << ")\n";
    std::cout << "====================================================\n";

    // 1. Benchmark: Pure Allocation & Immediate Release
    {
        auto start = std::chrono::steady_clock::now();

        for (uint32_t i = 0; i < kIterations; ++i)
        {
            cpromise_t p;
            cfuture_t f;
            if (cfuture_create(&pool, &p, &f))
            {
                cfuture_abandon(&f);
                cpromise_drop(&p, 0);
            }
        }

        auto end = std::chrono::steady_clock::now();
        auto duration_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        double ns_per_op = static_cast<double>(duration_ns) / kIterations;
        double ops_per_sec = (1e9 / ns_per_op);

        std::cout << "  Allocation + Drop Cycle: " << std::fixed << std::setprecision(1)
                  << ns_per_op << " ns/op (" << static_cast<uint64_t>(ops_per_sec) << " ops/sec)\n";
    }

    // 2. Benchmark: Full Synchronous Create -> Fulfill -> Wait -> Recycle Cycle
    {
        auto start = std::chrono::steady_clock::now();

        for (uint32_t i = 0; i < kIterations; ++i)
        {
            cpromise_t p;
            cfuture_t f;
            if (cfuture_create(&pool, &p, &f))
            {
                uint32_t val = i;
                cpromise_set_value(&p, &val, 0);

                uint32_t out_val = 0;
                cfuture_wait_for(&f, 10, &out_val, nullptr);
            }
        }

        auto end = std::chrono::steady_clock::now();
        auto duration_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        double ns_per_op = static_cast<double>(duration_ns) / kIterations;
        double ops_per_sec = (1e9 / ns_per_op);

        std::cout << "  Complete Roundtrip Cycle: " << std::fixed << std::setprecision(1)
                  << ns_per_op << " ns/op (" << static_cast<uint64_t>(ops_per_sec) << " ops/sec)\n";
    }

    std::cout << "====================================================\n";

    cfuture_pool_destroy(&pool);
    return 0;
}
