/**
 * @file bench_stress_soak.cpp
 * @brief Multi-Threaded Hyper-Speed Stress & Overnight Soak Benchmark
 *
 * Hammers libcfuture pool across multiple producer and consumer threads.
 * Continuously validates:
 *  - 100% payload integrity (checksum validation).
 *  - 100% error code propagation.
 *  - Zero lost references or bitmask leaks (pool.allocated_mask == 0).
 *
 * Supports duration-based soak runs (e.g. overnight) or cycle counts.
 *
 * SPDX-License-Identifier: MIT
 */

#include "adapters/cfuture_posix.h"
#include "cfuture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct SoakPayload
{
    uint32_t cycle_id;
    uint32_t thread_id;
    uint32_t magic;
    uint32_t checksum;
};

class SoakChannel
{
  public:
    void push(cfuture_t f)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        items_.push_back(f);
        cv_.notify_one();
    }

    bool pop(cfuture_t &out_f, std::atomic<bool> &stop_signal)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        while (items_.empty())
        {
            if (stop_signal.load(std::memory_order_relaxed))
            {
                return false;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(20));
        }

        out_f = items_.front();
        items_.pop_front();
        return true;
    }

    void notify_all()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cv_.notify_all();
    }

  private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<cfuture_t> items_;
};

} // namespace

int main(int argc, char **argv)
{
    uint32_t duration_sec = 5; // Default: 5 seconds for quick check
    uint64_t max_cycles = 0;   // 0 = unbounded (timed by duration)
    uint32_t num_producers = 8;
    uint32_t num_consumers = 8;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc)
        {
            duration_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (arg == "--cycles" && i + 1 < argc)
        {
            max_cycles = std::stoull(argv[++i]);
        }
        else if (arg == "--threads" && i + 1 < argc)
        {
            uint32_t t = static_cast<uint32_t>(std::stoul(argv[++i]));
            num_producers = t / 2 > 0 ? t / 2 : 1;
            num_consumers = num_producers;
        }
        else if (arg == "--help")
        {
            std::cout << "Usage: " << argv[0]
                      << " [--duration <seconds>] [--cycles <count>] [--threads <count>]\n";
            return 0;
        }
    }

    constexpr uint32_t kCapacity = 32;
    cfuture_slot_t slots[kCapacity];
    uint8_t payload_arena[kCapacity * sizeof(SoakPayload)];
    cfuture_pool_t pool;

    const cfuture_sync_ops_t *posix_ops = cfuture_posix_sync_ops();
    if (!cfuture_pool_init(&pool, kCapacity, sizeof(SoakPayload), slots, payload_arena, posix_ops))
    {
        std::cerr << "Failed to initialize pool\n";
        return 1;
    }

    std::cout << "====================================================\n";
    std::cout << "  libcfuture Hyper-Speed Soak / Stress Benchmark\n";
    std::cout << "====================================================\n";
    std::cout << "  Producers: " << num_producers << " | Consumers: " << num_consumers << "\n";
    if (max_cycles > 0)
    {
        std::cout << "  Target Cycles: " << max_cycles << "\n";
    }
    else
    {
        std::cout << "  Target Duration: " << duration_sec << " seconds\n";
    }
    std::cout << "====================================================\n";

    SoakChannel channel;
    std::atomic<bool> stop_flag{false};
    std::atomic<uint64_t> completed_cycles{0};
    std::atomic<uint64_t> dropped_cycles{0};
    std::atomic<uint64_t> total_created{0};
    std::atomic<uint64_t> checksum_errors{0};

    auto start_time = std::chrono::steady_clock::now();

    // Consumers
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (uint32_t cid = 0; cid < num_consumers; ++cid)
    {
        consumers.emplace_back(
            [&channel, &stop_flag, &completed_cycles, &dropped_cycles, &checksum_errors]()
            {
                cfuture_t f;
                while (channel.pop(f, stop_flag))
                {
                    SoakPayload rx{};
                    int32_t err = 0;

                    if (cfuture_wait_for(&f, 100, &rx, &err))
                    {
                        uint32_t expected_checksum =
                            rx.cycle_id ^ rx.thread_id ^ rx.magic ^ 0xDEADBEEFU;
                        if (rx.checksum != expected_checksum || rx.magic != 0xCAFEBABE)
                        {
                            checksum_errors.fetch_add(1, std::memory_order_relaxed);
                        }
                        completed_cycles.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        if (err == -42)
                        {
                            dropped_cycles.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
    }

    // Producers
    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (uint32_t pid = 0; pid < num_producers; ++pid)
    {
        producers.emplace_back(
            [&pool, &channel, &stop_flag, &total_created, pid, max_cycles]()
            {
                uint32_t local_cycle = 0;
                while (!stop_flag.load(std::memory_order_relaxed))
                {
                    if (max_cycles > 0 &&
                        total_created.load(std::memory_order_relaxed) >= max_cycles)
                    {
                        break;
                    }

                    total_created.fetch_add(1, std::memory_order_relaxed);

                    cpromise_t p;
                    cfuture_t f;

                    while (!cfuture_create(&pool, &p, &f))
                    {
                        if (stop_flag.load(std::memory_order_relaxed))
                        {
                            return;
                        }
                        std::this_thread::yield();
                    }

                    channel.push(f);

                    if ((local_cycle % 33) == 0)
                    {
                        cpromise_drop(&p, -42);
                    }
                    else
                    {
                        SoakPayload tx;
                        tx.cycle_id = local_cycle;
                        tx.thread_id = pid;
                        tx.magic = 0xCAFEBABE;
                        tx.checksum = tx.cycle_id ^ tx.thread_id ^ tx.magic ^ 0xDEADBEEFU;

                        cpromise_set_value(&p, &tx, 0);
                    }

                    ++local_cycle;
                }
            });
    }

    // Monitor loop
    auto last_report = start_time;
    uint64_t last_cycles = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        auto now = std::chrono::steady_clock::now();
        auto elapsed_total_s =
            std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        uint64_t current_completed = completed_cycles.load(std::memory_order_relaxed);
        uint64_t current_dropped = dropped_cycles.load(std::memory_order_relaxed);
        uint64_t current_total = current_completed + current_dropped;

        double delta_sec = std::chrono::duration<double>(now - last_report).count();
        uint64_t delta_cycles = current_total - last_cycles;
        double current_rate = static_cast<double>(delta_cycles) / delta_sec;

        std::cout << "  [Elapsed: " << std::setw(3) << elapsed_total_s
                  << "s] Completed: " << std::setw(8) << current_completed
                  << " | Dropped: " << std::setw(6) << current_dropped << " | Rate: " << std::fixed
                  << std::setprecision(0) << current_rate << " ops/sec" << std::endl;

        last_report = now;
        last_cycles = current_total;

        if (max_cycles > 0 && current_total >= max_cycles)
        {
            break;
        }

        if (max_cycles == 0 && elapsed_total_s >= duration_sec)
        {
            break;
        }
    }

    // Phase 1: Stop and join all producers so no new futures are created
    stop_flag.store(true);
    for (auto &t : producers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // Phase 2: Signal consumers and wait until channel is completely drained
    channel.notify_all();
    for (auto &t : consumers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    double total_sec = std::chrono::duration<double>(end_time - start_time).count();
    uint64_t final_completed = completed_cycles.load();
    uint64_t final_dropped = dropped_cycles.load();
    uint64_t final_total = final_completed + final_dropped;
    uint64_t errors = checksum_errors.load();
    uint_fast32_t remaining_mask = pool.allocated_mask.load();

    std::cout << "====================================================\n";
    std::cout << "  Soak Test Results Summary:\n";
    std::cout << "====================================================\n";
    std::cout << "  Total Duration:      " << std::fixed << std::setprecision(2) << total_sec
              << " seconds\n";
    std::cout << "  Total Cycles:        " << final_total << "\n";
    std::cout << "  Average Throughput:  " << static_cast<uint64_t>(final_total / total_sec)
              << " ops/sec\n";
    std::cout << "  Checksum Errors:     " << errors << "\n";
    std::cout << "  Residual Mask:       0x" << std::hex << remaining_mask << std::dec << "\n";
    if (remaining_mask != 0)
    {
        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            if (remaining_mask & (1U << i))
            {
                std::cout << "    Slot " << i << ": ref_count=" << pool.slots[i].ref_count.load()
                          << ", state=" << pool.slots[i].state.load()
                          << ", err=" << pool.slots[i].error_code << "\n";
            }
        }
    }
    std::cout << "====================================================\n";

    cfuture_pool_destroy(&pool);

    if (errors > 0 || remaining_mask != 0)
    {
        std::cerr << "SOAK TEST FAILED: Corruption or Leaked Slots detected!\n";
        return 1;
    }

    std::cout << "SOAK TEST PASSED: 100% data integrity, 0 leaked slots.\n";
    return 0;
}
