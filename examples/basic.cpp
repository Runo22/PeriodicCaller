#include <periodic_caller/PeriodicCaller.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
namespace pc = periodic_caller;

static void heartbeat() { std::puts("heartbeat (free function, 500 ms)"); }

static void print(const char* name, const pc::TaskStats& s) {
    using us = std::chrono::duration<double, std::micro>;
    std::printf("%-10s runs=%-5llu missed=%-4llu overrun=%-4llu lateness avg=%8.1f us max=%8.1f us"
                "  duration max=%9.1f us\n",
                name, static_cast<unsigned long long>(s.runs),
                static_cast<unsigned long long>(s.missed_ticks),
                static_cast<unsigned long long>(s.overrun_skips), us(s.avg_lateness).count(),
                us(s.max_lateness).count(), us(s.max_duration).count());
}

int main() {
    // Optional: {.spin_threshold = 200us} for microsecond precision,
    //           {.use_mmcss = true} for stability under heavy system load.
    pc::PeriodicCaller caller;
    std::printf("high-resolution timer: %s\n", caller.uses_high_resolution_timer() ? "yes" : "no");

    std::atomic<int> fast_count{0};
    const auto fast = caller.add(1ms, [&] { ++fast_count; });  // lambda, inline
    const auto slow = caller.add(10ms, [] { /* e.g. poll a device */ });
    const auto beat = caller.add(500ms, heartbeat, {.run_immediately = true});  // function

    // A long task (30 ms work every 20 ms) on its own thread: it cannot delay
    // the 1 ms task, and overlapping ticks are skipped.
    const auto heavy = caller.add(20ms, [] { std::this_thread::sleep_for(30ms); },
                                  {.execution_mode = pc::ExecutionMode::Dedicated,
                                   .overlap_policy = pc::OverlapPolicy::Skip});

    // The main thread is free; it is not blocked by the scheduler.
    std::this_thread::sleep_for(2s);
    caller.stop();

    print("1ms", *caller.stats(fast));
    print("10ms", *caller.stats(slow));
    print("500ms", *caller.stats(beat));
    print("heavy", *caller.stats(heavy));
    std::printf("1ms task ran %d times in ~2 s\n", fast_count.load());
}
