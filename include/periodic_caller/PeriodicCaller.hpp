#pragma once

// PeriodicCaller - high-precision periodic task scheduler for Windows (C++23).
//
// A single scheduler thread keeps the timing for every registered task.
// The calling thread is never blocked: add()/remove() return immediately.
//
// Precision comes from three things:
//   1. Absolute schedule: deadlines advance as `next += period`, so error never
//      accumulates (no drift), no matter how long the task itself takes.
//   2. High-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
//      Windows 10 1803+) instead of Sleep(), which is bound to the ~15.6 ms
//      system tick. Falls back to a classic timer + timeBeginPeriod(1).
//   3. Optional hybrid spin: wake up `spin_threshold` early and busy-wait the
//      last few microseconds. Disabled by default because it uses CPU.
//
// Short tasks run inline on the scheduler thread (most precise). Long tasks
// should use ExecutionMode::Dedicated so they cannot delay the other tasks.

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

namespace periodic_caller {

using Clock = std::chrono::steady_clock;
using Task = std::move_only_function<void()>;
using TaskId = std::uint64_t;

// Where a task runs.
enum class ExecutionMode {
    Inline,     // On the scheduler thread. Most precise; the task must be short,
                // since every other task waits while it runs.
    Dedicated,  // On its own persistent worker thread, signalled at each tick.
                // Adds a few microseconds of wake-up latency, but a long task
                // never delays the others.
};

// Dedicated mode: what to do when a tick arrives while the previous run of the
// same task is still executing.
enum class OverlapPolicy {
    Skip,      // Drop the tick (counted in TaskStats::overrun_skips).
    Coalesce,  // Remember it and run once more right after the current run
               // finishes. Further ticks meanwhile are dropped.
};

// What to do when the schedule itself falls behind (e.g. an inline task ran
// longer than its period).
enum class MissedTickPolicy {
    Skip,     // Drop the missed ticks and continue on the original time grid.
    CatchUp,  // Fire every missed tick back-to-back until the schedule is met.
};

struct TaskOptions {
    ExecutionMode execution_mode = ExecutionMode::Inline;
    OverlapPolicy overlap_policy = OverlapPolicy::Skip;  // Dedicated mode only.
    MissedTickPolicy missed_tick_policy = MissedTickPolicy::Skip;
    // true: first call happens right away; false: first call after one period.
    bool run_immediately = false;
};

struct SchedulerOptions {
    // Busy-wait window before each deadline. 0 disables spinning (default).
    // ~100-300 us typically gives microsecond-level accuracy. A value larger
    // than the period turns the scheduler into a pure busy loop.
    std::chrono::microseconds spin_threshold{0};
    // Run the scheduler and dedicated workers at THREAD_PRIORITY_TIME_CRITICAL.
    bool high_priority_thread = true;
    // Register the threads with MMCSS ("Pro Audio"). Lets them keep a real-time
    // class priority under heavy system load. No admin rights required.
    bool use_mmcss = false;
    // Called (on the thread that ran the task) when a task throws. Exceptions
    // are always contained and counted, with or without this handler.
    std::function<void(TaskId, std::exception_ptr)> on_error;
};

struct TaskStats {
    std::uint64_t runs = 0;
    std::uint64_t missed_ticks = 0;   // Ticks dropped by MissedTickPolicy::Skip.
    std::uint64_t overrun_skips = 0;  // Ticks dropped because the task was still running.
    std::uint64_t exceptions = 0;     // Exceptions thrown (and contained) by the task.
    std::chrono::nanoseconds last_lateness{0};  // Actual start - scheduled tick.
    std::chrono::nanoseconds max_lateness{0};
    std::chrono::nanoseconds avg_lateness{0};
    std::chrono::nanoseconds last_duration{0};  // How long the task body took.
    std::chrono::nanoseconds max_duration{0};
};

class PeriodicCaller {
public:
    explicit PeriodicCaller(SchedulerOptions options = {});
    // Stops everything and waits for running tasks to finish.
    // Must not be called from inside a task.
    ~PeriodicCaller();

    PeriodicCaller(const PeriodicCaller&) = delete;
    PeriodicCaller& operator=(const PeriodicCaller&) = delete;

    // Registers `task` to be called every `period`. Thread-safe, non-blocking.
    // Throws std::invalid_argument if period <= 0 or task is empty.
    TaskId add(std::chrono::nanoseconds period, Task task, TaskOptions options = {});

    // Unregisters a task. Returns false if the id is unknown. Does not wait: if
    // the task is executing right now, that call completes but no further calls
    // happen. Safe to call from inside a task (including on itself).
    bool remove(TaskId id);

    // Changes the period of a task; the new grid starts from its last tick.
    bool set_period(TaskId id, std::chrono::nanoseconds period);

    std::optional<TaskStats> stats(TaskId id) const;

    // Stops the scheduler and all dedicated workers. Stats stay readable.
    // Called automatically by the destructor.
    void stop();

    // true if the high-resolution waitable timer is in use.
    bool uses_high_resolution_timer() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace periodic_caller
