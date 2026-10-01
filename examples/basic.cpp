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
    std::printf("%-8s runs=%-6llu missed=%-4llu lateness avg=%8.1f us  max=%8.1f us\n", name,
                static_cast<unsigned long long>(s.runs),
                static_cast<unsigned long long>(s.missed_ticks), us(s.avg_lateness).count(),
                us(s.max_lateness).count());
}

int main() {
    // Optional hybrid mode: pc::PeriodicCaller caller({.spin_threshold = 200us});
    pc::PeriodicCaller caller;
    std::printf("high-resolution timer: %s\n", caller.uses_high_resolution_timer() ? "yes" : "no");

    std::atomic<int> fast_count{0};
    const auto fast = caller.add(1ms, [&] { ++fast_count; });          // lambda
    const auto slow = caller.add(10ms, [] { /* e.g. poll a device */ });
    const auto beat = caller.add(500ms, heartbeat, {.run_immediately = true});  // function

    // The main thread is free; it is not blocked by the scheduler.
    std::this_thread::sleep_for(2s);

    print("1ms", *caller.stats(fast));
    print("10ms", *caller.stats(slow));
    print("500ms", *caller.stats(beat));
    std::printf("1ms task ran %d times in ~2 s\n", fast_count.load());
}
