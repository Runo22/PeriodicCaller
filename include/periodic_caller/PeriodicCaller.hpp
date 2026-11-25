#pragma once

// PeriodicCaller - precise, low-CPU periodic task scheduler for Windows (C++23).
//
// Design (aimed at a handful of frame-critical tasks, e.g. 5 ms and 60 Hz):
//
//   * Every task runs on its own thread with its own high-resolution waitable
//     timer. Tasks cannot delay each other, there is no shared scheduler hop,
//     and the per-tick path never takes a lock that another thread can hold
//     (no priority inversion). add()/remove() never block on a running task.
//   * Absolute schedule: tick n happens at origin + n * period, computed with
//     an exact rational period (60 Hz is exactly 1/60 s), so nothing drifts.
//   * Adaptive early wake-up: each thread learns how late the OS timer usually
//     fires, arms it that much earlier and busy-waits only the remainder. The
//     busy-wait per tick never exceeds SchedulerOptions::max_spin (default
//     50 us), so CPU use stays near zero.
//   * Frames are never dropped by default (MissedTickPolicy::CatchUp): a late
//     frame runs as soon as it can, followed immediately by any overdue ones.
//     A run that does not finish within its deadline (default: one period) is
//     counted as a deadline miss and reported to on_deadline_miss.
//
// Using it from several DLLs (plugins): compile PeriodicCaller.cpp into ONE
// DLL (e.g. your utility DLL) with PERIODIC_CALLER_EXPORTS defined, and define
// PERIODIC_CALLER_IMPORTS in every module that uses it. All modules then share
// PeriodicCaller::shared(). Plugins register their tasks through a TaskScope
// and clear it in their shutdown function, before FreeLibrary. See README.

#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <version>

#if !defined(__cpp_lib_move_only_function)
#error "PeriodicCaller requires C++23 std::move_only_function: VS 2022 17.2+ with /std:c++latest (CMake: cxx_std_23)."
#endif

// Export macro. Static library (default): empty. DLL: define
// PERIODIC_CALLER_EXPORTS while building the DLL and PERIODIC_CALLER_IMPORTS in
// the modules that use it. A project may also predefine PERIODIC_CALLER_API.
#if !defined(PERIODIC_CALLER_API)
#if defined(PERIODIC_CALLER_EXPORTS)
#define PERIODIC_CALLER_API __declspec(dllexport)
#elif defined(PERIODIC_CALLER_IMPORTS)
#define PERIODIC_CALLER_API __declspec(dllimport)
#else
#define PERIODIC_CALLER_API
#endif
#endif

namespace periodic_caller {

using Clock = std::chrono::steady_clock;
using Task = std::move_only_function<void()>;
using TaskId = std::uint64_t;

// An exact period of num/den nanoseconds. Built implicitly from any integral
// std::chrono::duration (5ms, 16667us, duration<int64_t, std::ratio<1, 60>>{1})
// or from a frequency with hz(). Tick n is placed at floor(n * num / den) ns
// after the origin, so the long-run rate is exact even when the period is not
// a whole number of nanoseconds.
class Period {
public:
    template <class Rep, class Ratio>
        requires std::integral<Rep>
    constexpr Period(std::chrono::duration<Rep, Ratio> d) {  // NOLINT: implicit by design.
        // seconds = count * Ratio::num / Ratio::den  ->  ns = count * num * 1e9 / den
        constexpr std::int64_t giga = 1'000'000'000;
        constexpr std::int64_t g = std::gcd(giga, static_cast<std::int64_t>(Ratio::den));
        _num = checked_mul(checked_mul(static_cast<std::int64_t>(d.count()),
                                       static_cast<std::int64_t>(Ratio::num)),
                           giga / g);
        _den = static_cast<std::int64_t>(Ratio::den) / g;
        normalize();
    }

    // num/den nanoseconds. Throws std::invalid_argument if den <= 0.
    static constexpr Period from_ratio(std::int64_t num_ns, std::int64_t den) {
        Period p;
        if (den <= 0) throw std::invalid_argument("Period: denominator must be positive");
        p._num = num_ns;
        p._den = den;
        p.normalize();
        return p;
    }

    constexpr std::int64_t num() const noexcept { return _num; }
    constexpr std::int64_t den() const noexcept { return _den; }
    constexpr bool positive() const noexcept { return _num > 0; }

    // Offset of tick n from the origin: floor(n * num / den), without overflow
    // for any realistic n.
    constexpr std::chrono::nanoseconds offset(std::uint64_t n) const noexcept {
        const auto den = static_cast<std::uint64_t>(_den);
        const auto q = static_cast<std::int64_t>(n / den);
        const auto r = static_cast<std::int64_t>(n % den);
        return std::chrono::nanoseconds(q * _num + (r * _num) / _den);
    }

    // The period rounded to whole nanoseconds.
    constexpr std::chrono::nanoseconds approx() const noexcept {
        return std::chrono::nanoseconds((_num + _den / 2) / _den);
    }

    friend constexpr bool operator==(const Period&, const Period&) = default;

private:
    constexpr Period() = default;

    static constexpr std::int64_t checked_mul(std::int64_t a, std::int64_t b) {
        if (a != 0 && (a > std::numeric_limits<std::int64_t>::max() / (b < 0 ? -b : b) ||
                       a < std::numeric_limits<std::int64_t>::min() / (b < 0 ? -b : b))) {
            throw std::invalid_argument("Period: value out of range");
        }
        return a * b;
    }

    constexpr void normalize() {
        const auto g = std::gcd(_num, _den);
        if (g > 1) {
            _num /= g;
            _den /= g;
        }
        // offset() computes r * num with r < den: keep that product in range.
        const auto mag = _num < 0 ? -_num : _num;
        if (mag != 0 && _den > std::numeric_limits<std::int64_t>::max() / mag) {
            throw std::invalid_argument("Period: value out of range");
        }
    }

    std::int64_t _num = 0;
    std::int64_t _den = 1;
};

// A period of exactly 1/frequency seconds: hz(60) == 16.666... ms.
// Throws std::invalid_argument if frequency is zero.
constexpr Period hz(std::uint32_t frequency) {
    if (frequency == 0) throw std::invalid_argument("hz: frequency must be positive");
    return Period::from_ratio(1'000'000'000, frequency);
}

// What to do when a task falls behind its schedule (a run took longer than the
// period, or the thread was held up).
enum class MissedTickPolicy {
    CatchUp,  // Default. Never drop a frame: run every overdue tick back-to-back,
              // as soon as possible, until the schedule is met again.
    Skip,     // Run only the most recent overdue tick, drop the older ones
              // (counted in TaskStats::skipped_ticks) and stay on the grid.
};

struct TaskOptions {
    MissedTickPolicy missed_tick_policy = MissedTickPolicy::CatchUp;
    // CatchUp only: run at most this many overdue ticks back-to-back; older
    // ones are dropped and counted as skipped. 0 = no limit. Useful against a
    // burst of thousands of runs after a debugger break or system sleep.
    std::uint32_t max_catch_up = 0;
    // true: the first call happens right away; false: one period after add().
    // Ignored when `phase` is set.
    bool run_immediately = false;
    // A run must FINISH within this time after its scheduled tick, otherwise it
    // is a deadline miss. Default: one period (i.e. before the next tick).
    std::optional<std::chrono::nanoseconds> deadline{};
    // Places the ticks on a grid shared by all tasks of this PeriodicCaller:
    // epoch() + phase + n * period. Tasks with the same phase and compatible
    // periods tick together; different phases spread them apart. Must be in
    // [0, period). Unset: the grid starts at add().
    std::optional<std::chrono::nanoseconds> phase{};
    // Thread name shown in debuggers and profilers (UTF-8). Default: "PeriodicCaller #<id>".
    std::string name{};
};

enum class ThreadPriority { Normal, AboveNormal, Highest, TimeCritical };

// Information about the tick being executed; see current_tick().
struct TickInfo {
    TaskId id = 0;
    std::uint64_t index = 0;        // Frame number: 0 for the task's first tick, +1 per period.
    Clock::time_point scheduled;    // When this frame was due to start.
    Clock::time_point deadline;     // When it must be finished.
};

struct DeadlineMiss {
    TickInfo tick;
    Clock::time_point start;  // When the run actually started.
    Clock::time_point end;    // When it finished (after the deadline).
};

struct SchedulerOptions {
    // Upper bound of the busy-wait before each tick. The scheduler wakes up
    // early by the learned OS timer latency (at most this much) and spins the
    // rest, so a task starts within microseconds of its tick while using very
    // little CPU. 0 = never spin: lowest CPU, start jitter = OS timer jitter.
    std::chrono::microseconds max_spin{50};
    ThreadPriority priority = ThreadPriority::TimeCritical;
    // Register task threads with MMCSS ("Pro Audio"): real-time class priority,
    // so even other time-critical threads cannot preempt them. No admin rights
    // needed. Off by default: while MMCSS threads are active Windows may also
    // throttle network packet processing (NetworkThrottlingIndex).
    bool use_mmcss = false;
    // On hybrid CPUs (Intel 12th gen+), keep task threads on performance cores.
    // No effect on CPUs whose cores are all alike, or when the process affinity
    // was restricted.
    bool prefer_performance_cores = true;
    // Opt task threads out of EcoQoS / power throttling (Windows 11 may otherwise
    // run a background app's threads on efficiency cores at low clocks).
    bool disable_power_throttling = true;
    // Called on the task's thread when it throws. Exceptions are always caught
    // and counted, with or without this handler.
    std::function<void(TaskId, std::exception_ptr)> on_error{};
    // Called on the task's thread after a run that missed its deadline. Keep it
    // short: it delays the next frame of that task.
    std::function<void(const DeadlineMiss&)> on_deadline_miss{};
};

struct TaskStats {
    // Lateness histogram: bucket 0 = under 1 us, bucket b = [2^(b-1), 2^b) us,
    // the last bucket also holds everything above.
    static constexpr std::size_t histogram_buckets = 24;

    std::uint64_t runs = 0;
    std::uint64_t deadline_misses = 0;  // Runs that finished after their deadline.
    std::uint64_t skipped_ticks = 0;    // Ticks dropped (Skip policy or max_catch_up).
    std::uint64_t exceptions = 0;       // Exceptions thrown (and contained) by the task.

    // Lateness = actual start - scheduled tick.
    std::chrono::nanoseconds last_lateness{0};
    std::chrono::nanoseconds avg_lateness{0};
    std::chrono::nanoseconds max_lateness{0};
    // Duration of the task body.
    std::chrono::nanoseconds last_duration{0};
    std::chrono::nanoseconds avg_duration{0};
    std::chrono::nanoseconds max_duration{0};
    // How much later than requested the OS timer woke the thread.
    std::chrono::nanoseconds avg_timer_latency{0};
    std::chrono::nanoseconds max_timer_latency{0};
    // Current adaptive early wake-up (<= SchedulerOptions::max_spin).
    std::chrono::nanoseconds wake_lead{0};
    // Total time spent busy-waiting (the CPU cost of the precision).
    std::chrono::nanoseconds total_spin{0};

    std::array<std::uint64_t, histogram_buckets> lateness_histogram{};

    // Upper bound of the lateness below which a fraction `p` (0..1] of the runs
    // started, from the histogram (power-of-two resolution).
    std::chrono::nanoseconds lateness_percentile(double p) const noexcept {
        std::uint64_t total = 0;
        for (const auto c : lateness_histogram) total += c;
        if (total == 0) return std::chrono::nanoseconds(0);
        const auto target = static_cast<std::uint64_t>(p * static_cast<double>(total) + 0.5);
        std::uint64_t seen = 0;
        for (std::size_t b = 0; b < histogram_buckets; ++b) {
            seen += lateness_histogram[b];
            if (seen >= target && seen > 0) return std::chrono::microseconds(std::int64_t{1} << b);
        }
        return std::chrono::microseconds(std::int64_t{1} << (histogram_buckets - 1));
    }

    static constexpr std::size_t histogram_bucket(std::chrono::nanoseconds lateness) noexcept {
        if (lateness.count() < 1000) return 0;
        const auto us = static_cast<std::uint64_t>(lateness.count() / 1000);
        const auto b = static_cast<std::size_t>(std::bit_width(us));
        return b < histogram_buckets ? b : histogram_buckets - 1;
    }
};

// The tick being executed by the calling thread, or nullptr when not called
// from inside a task.
PERIODIC_CALLER_API const TickInfo* current_tick() noexcept;

// C4251: the private std::unique_ptr member has no dll-interface. Harmless,
// since every module is built with the same compiler and /MD runtime.
#pragma warning(push)
#pragma warning(disable : 4251)

class PERIODIC_CALLER_API PeriodicCaller {
public:
    explicit PeriodicCaller(SchedulerOptions options = {});
    // Stops everything and waits for running tasks to finish.
    // Must not be called from inside a task.
    ~PeriodicCaller();

    PeriodicCaller(const PeriodicCaller&) = delete;
    PeriodicCaller& operator=(const PeriodicCaller&) = delete;

    // Registers `task` to be called every `period` on a new thread of its own.
    // Thread-safe; does not wait for anything. Throws std::invalid_argument for
    // a non-positive period, an empty task or a bad phase/deadline,
    // std::logic_error after stop(), std::runtime_error if Windows refuses to
    // create the thread or timer.
    TaskId add(Period period, Task task, TaskOptions options = {});

    // Unregisters a task. Returns false if the id is unknown. Does not wait: a
    // call in progress completes, then no further calls happen and the task
    // object is destroyed shortly after on its thread. Safe to call from inside
    // a task (including on itself).
    bool remove(TaskId id);

    // Like remove(), but also waits until a running call has finished and the
    // task object (the lambda and its captures) has been destroyed. After it
    // returns no code or data of the task is touched again, so the module that
    // owns it may be unloaded. Called from inside the task itself it cannot
    // wait for itself and behaves like remove(). Also waits for a task that was
    // already remove()d but has not finished yet. Two tasks must not wait for
    // each other this way (deadlock).
    bool remove_and_wait(TaskId id);

    // Changes the period; the new grid starts from the last tick. Takes effect
    // right away (or after the current call). Throws std::invalid_argument for
    // a non-positive period.
    bool set_period(TaskId id, Period period);

    // A snapshot, at most one tick old (final and exact after stop()).
    // Never blocks a task thread.
    std::optional<TaskStats> stats(TaskId id) const;

    // Stops all tasks and waits for running calls. Stats stay readable; add()
    // throws afterwards. Called by the destructor. From inside a task it only
    // requests the stop (no waiting); the destructor waits later.
    void stop();

    // true if the high-resolution waitable timer (Windows 10 1803+) is in use.
    bool uses_high_resolution_timer() const noexcept;

    // Origin of the grid used by TaskOptions::phase (the construction time).
    Clock::time_point epoch() const noexcept;

    // Process-wide instance, shared by every module when PeriodicCaller lives
    // in a DLL. Created on first use; `options` only apply to that first call.
    // It is intentionally NOT destroyed automatically: a destructor running in
    // DllMain (DLL_PROCESS_DETACH) would join threads under the loader lock and
    // deadlock. Call shutdown_shared() from the host before unloading the DLL
    // that contains PeriodicCaller (not needed at process exit).
    static PeriodicCaller& shared(SchedulerOptions options = {});

    // Stops and destroys the shared instance. Waits for running tasks.
    // A later shared() call creates a new instance.
    static void shutdown_shared();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

#pragma warning(pop)

// Owns a group of tasks, typically all tasks of one plugin. clear() (or the
// destructor) removes every task in the group and waits for running calls to
// finish, so the plugin can be unloaded safely right afterwards.
//
// Call clear() from the plugin's own shutdown function, NOT from DllMain or a
// global destructor that runs during FreeLibrary (loader lock).
class TaskScope {
public:
    explicit TaskScope(PeriodicCaller& caller = PeriodicCaller::shared()) : _caller(&caller) {}
    ~TaskScope() { clear(); }

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    TaskId add(Period period, Task task, TaskOptions options = {}) {
        const auto id = _caller->add(period, std::move(task), std::move(options));
        std::scoped_lock lock(_mutex);
        _ids.push_back(id);
        return id;
    }

    // Removes one task of this scope and waits for it (see remove_and_wait).
    bool remove(TaskId id) {
        {
            std::scoped_lock lock(_mutex);
            if (std::erase(_ids, id) == 0) return false;
        }
        return _caller->remove_and_wait(id);
    }

    // Removes all tasks of this scope and waits for running calls to finish.
    void clear() {
        std::vector<TaskId> ids;
        {
            std::scoped_lock lock(_mutex);
            ids.swap(_ids);
        }
        for (const auto id : ids) _caller->remove_and_wait(id);
    }

    PeriodicCaller& caller() const noexcept { return *_caller; }

private:
    PeriodicCaller* _caller;
    std::mutex _mutex;
    std::vector<TaskId> _ids;
};

}  // namespace periodic_caller
