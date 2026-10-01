#pragma once

// PeriodicCaller - high-precision periodic task scheduler for Windows (C++23).
//
// A single background thread runs every registered task at its own period.
// The calling thread is never blocked: add()/remove() return immediately.
//
// Precision comes from three things:
//   1. Absolute schedule: deadlines advance as `next += period`, so error never
//      accumulates (no drift), no matter how long the task itself takes.
//   2. High-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
//      Windows 10 1803+) instead of Sleep(), which is bound to the ~15.6 ms
//      system tick. Falls back to a classic timer + timeBeginPeriod(1).
//   3. Optional hybrid spin: wake up `spinThreshold` early and busy-wait the
//      last few microseconds. Disabled by default because it uses CPU.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace periodic_caller {

using Clock = std::chrono::steady_clock;
using Task = std::move_only_function<void()>;

// What to do when a task falls behind (e.g. it ran longer than its period).
enum class MissedTickPolicy {
    Skip,     // Drop the missed ticks and continue on the original time grid.
    CatchUp,  // Run every missed tick back-to-back until the schedule is met.
};

struct TaskOptions {
    MissedTickPolicy missedTickPolicy = MissedTickPolicy::Skip;
    // true: first call happens right away; false: first call after one period.
    bool runImmediately = false;
};

struct SchedulerOptions {
    // Busy-wait window before each deadline. 0 disables spinning (default).
    // ~100-300 us typically gives microsecond-level accuracy.
    std::chrono::microseconds spinThreshold{0};
    // Raise the worker thread to THREAD_PRIORITY_TIME_CRITICAL.
    bool highPriorityThread = true;
};

struct TaskStats {
    std::uint64_t runs = 0;
    std::uint64_t missedTicks = 0;  // Ticks skipped under MissedTickPolicy::Skip.
    std::uint64_t exceptions = 0;   // Exceptions thrown (and swallowed) by the task.
    std::chrono::nanoseconds lastLateness{0};  // Actual start - scheduled start.
    std::chrono::nanoseconds maxLateness{0};
    std::chrono::nanoseconds avgLateness{0};
};

using TaskId = std::uint64_t;

class PeriodicCaller {
public:
    explicit PeriodicCaller(SchedulerOptions options = {});
    ~PeriodicCaller();  // Stops the worker; waits for a running task to finish.

    PeriodicCaller(const PeriodicCaller&) = delete;
    PeriodicCaller& operator=(const PeriodicCaller&) = delete;

    // Registers `task` to be called every `period`. Thread-safe, non-blocking.
    // Throws std::invalid_argument if period <= 0 or task is empty.
    TaskId add(std::chrono::nanoseconds period, Task task, TaskOptions options = {});

    // Unregisters a task. Returns false if the id is unknown. If the task is
    // executing right now, that call completes but no further calls happen.
    // Safe to call from inside a task (including on itself).
    bool remove(TaskId id);

    // Changes the period of a task; the new grid starts from its next deadline.
    bool setPeriod(TaskId id, std::chrono::nanoseconds period);

    std::optional<TaskStats> stats(TaskId id) const;

    // Stops the worker thread. Called automatically by the destructor.
    void stop();

    // true if the high-resolution waitable timer is in use.
    bool usesHighResolutionTimer() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace periodic_caller
