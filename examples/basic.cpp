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
    std::printf("%-8s runs=%-5llu misses=%-3llu skipped=%-3llu lateness avg=%7.1f p99<=%6.0f max=%8.1f us"
                "  duration max=%8.1f us  spin=%6.2f ms\n",
                name, static_cast<unsigned long long>(s.runs),
                static_cast<unsigned long long>(s.deadline_misses),
                static_cast<unsigned long long>(s.skipped_ticks), us(s.avg_lateness).count(),
                us(s.lateness_percentile(0.99)).count(), us(s.max_lateness).count(),
                us(s.max_duration).count(), std::chrono::duration<double, std::milli>(s.total_spin).count());
}

int main() {
    // Defaults: adaptive early wake-up with at most 50 us busy-wait per tick,
    // never drop a frame (CatchUp), deadline = one period.
    pc::PeriodicCaller caller({
        .on_deadline_miss = [](const pc::DeadlineMiss& d) {
            std::printf("deadline miss: task %llu frame %llu finished %.2f ms late\n",
                        static_cast<unsigned long long>(d.tick.id),
                        static_cast<unsigned long long>(d.tick.index),
                        std::chrono::duration<double, std::milli>(d.end - d.tick.deadline).count());
        },
    });
    std::printf("high-resolution timer: %s\n", caller.uses_high_resolution_timer() ? "yes" : "no");

    std::atomic<int> frames{0};
    const auto fast = caller.add(5ms, [&] { ++frames; }, {.name = "5 ms control"});
    const auto video = caller.add(pc::hz(60), [] { /* exactly 60 per second */ }, {.name = "60 Hz"});
    const auto beat = caller.add(500ms, heartbeat, {.run_immediately = true});  // free function

    // Every task has its own thread: this one overruns its 20 ms deadline
    // every time, yet the 5 ms task is not delayed by it.
    const auto heavy = caller.add(20ms, [] {
        const auto end = pc::Clock::now() + 25ms;
        while (pc::Clock::now() < end) {
        }
    }, {.missed_tick_policy = pc::MissedTickPolicy::Skip, .name = "heavy"});

    // The main thread is free; it is not blocked by the tasks.
    std::this_thread::sleep_for(100ms);
    caller.remove(heavy);
    std::this_thread::sleep_for(1900ms);
    caller.stop();

    print("5ms", *caller.stats(fast));
    print("60Hz", *caller.stats(video));
    print("500ms", *caller.stats(beat));
    std::printf("5 ms task ran %d times in ~2 s\n", frames.load());
}
