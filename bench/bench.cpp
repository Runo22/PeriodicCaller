// Measures timing precision and CPU cost on the machine it runs on.
//
//   periodic_caller_bench [--seconds N] [--spin US[,US...]] [--work US] [--mmcss]
//
// Workload: two 5 ms tasks (phases 0 and 2.5 ms) plus 60, 60 and 50 Hz tasks,
// each busy for --work microseconds per frame. For every --spin value it runs
// the workload for --seconds and prints per-task lateness, deadline misses and
// the process CPU usage. Run it on the target PC (not in a VM) for real numbers;
// a long run (e.g. --seconds 600) shows the rare outliers that cause misses.
#include <periodic_caller/PeriodicCaller.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace pc = periodic_caller;

namespace {

double cpu_seconds() {
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto v = [](FILETIME f) {
        return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(v(kernel) + v(user)) / 1e7;
}

void busy_for(std::chrono::nanoseconds d) {
    const auto end = pc::Clock::now() + d;
    while (pc::Clock::now() < end) {
    }
}

double us(std::chrono::nanoseconds d) { return std::chrono::duration<double, std::micro>(d).count(); }

std::vector<long long> parse_list(const char* s) {
    std::vector<long long> out;
    while (*s) {
        char* end = nullptr;
        out.push_back(std::strtoll(s, &end, 10));
        if (end == s) break;
        s = *end == ',' ? end + 1 : end;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    long long seconds = 10;
    std::vector<long long> spins = {0, 50, 200};
    long long work_us = 200;
    bool mmcss = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoll(argv[++i]);
        } else if (a == "--spin" && i + 1 < argc) {
            spins = parse_list(argv[++i]);
        } else if (a == "--work" && i + 1 < argc) {
            work_us = std::atoll(argv[++i]);
        } else if (a == "--mmcss") {
            mmcss = true;
        } else {
            std::fprintf(stderr, "usage: %s [--seconds N] [--spin US[,US...]] [--work US] [--mmcss]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const auto cores = static_cast<double>(si.dwNumberOfProcessors);
    const auto work = std::chrono::microseconds(work_us);

    for (const auto spin : spins) {
        pc::PeriodicCaller caller({.max_spin = std::chrono::microseconds(spin), .use_mmcss = mmcss});
        struct Row {
            const char* name;
            pc::TaskId id;
        };
        const auto body = [work] { busy_for(work); };
        const std::vector<Row> rows = {
            {"5ms #1", caller.add(5ms, body, {.phase = 0ms, .name = "bench 5ms #1"})},
            {"5ms #2", caller.add(5ms, body, {.phase = 2500us, .name = "bench 5ms #2"})},
            {"60Hz #1", caller.add(pc::hz(60), body, {.phase = 0ms, .name = "bench 60Hz #1"})},
            {"60Hz #2", caller.add(pc::hz(60), body, {.phase = 8ms, .name = "bench 60Hz #2"})},
            {"50Hz", caller.add(pc::hz(50), body, {.phase = 1ms, .name = "bench 50Hz"})},
        };

        const double cpu0 = cpu_seconds();
        const auto t0 = pc::Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const double cpu = cpu_seconds() - cpu0;
        const double wall = std::chrono::duration<double>(pc::Clock::now() - t0).count();
        caller.stop();

        // Subtract the work itself to show the scheduler's own cost.
        const double work_cpu = static_cast<double>(work_us) * 1e-6 * (200 + 200 + 60 + 60 + 50) * wall;
        std::printf("\n== max_spin = %lld us, work = %lld us/frame, %s, %.1f s ==\n", spin, work_us,
                    mmcss ? "MMCSS" : "no MMCSS", wall);
        std::printf("CPU: %.2f%% of one core (%.3f%% of all %d), scheduler overhead ~%.2f%% of one core\n",
                    100.0 * cpu / wall, 100.0 * cpu / wall / cores, static_cast<int>(cores),
                    100.0 * (cpu - work_cpu) / wall);
        std::printf("%-8s %8s %6s %6s | %8s %7s %7s %8s %9s | %8s %9s | %7s\n", "task", "runs", "miss",
                    "skip", "lat avg", "p50<=", "p99<=", "p99.9<=", "lat max", "tmr avg", "tmr max", "lead");
        for (const auto& r : rows) {
            const auto s = *caller.stats(r.id);
            std::printf("%-8s %8llu %6llu %6llu | %8.1f %7.0f %7.0f %8.0f %9.1f | %8.1f %9.1f | %7.1f\n",
                        r.name, static_cast<unsigned long long>(s.runs),
                        static_cast<unsigned long long>(s.deadline_misses),
                        static_cast<unsigned long long>(s.skipped_ticks), us(s.avg_lateness),
                        us(s.lateness_percentile(0.5)), us(s.lateness_percentile(0.99)),
                        us(s.lateness_percentile(0.999)), us(s.max_lateness), us(s.avg_timer_latency),
                        us(s.max_timer_latency), us(s.wake_lead));
        }
    }
}
