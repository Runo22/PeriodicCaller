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
//
// Using it from several DLLs (plugins): compile PeriodicCaller.cpp into ONE
// DLL (e.g. your utility DLL) with PERIODIC_CALLER_EXPORTS defined, and define
// PERIODIC_CALLER_IMPORTS in every module that uses it. All modules then share
// PeriodicCaller::shared(). Plugins register their tasks through a TaskScope
// and clear it in their shutdown function, before FreeLibrary. See README.

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

    // Registers `task` to be called every `period`. Thread-safe, non-blocking.
    // Throws std::invalid_argument if period <= 0 or task is empty.
    TaskId add(std::chrono::nanoseconds period, Task task, TaskOptions options = {});

    // Unregisters a task. Returns false if the id is unknown. Does not wait: if
    // the task is executing right now, that call completes but no further calls
    // happen. Safe to call from inside a task (including on itself).
    bool remove(TaskId id);

    // Like remove(), but also waits until a running call has finished and
    // destroys the task object (the lambda and its captures) before returning.
    // After this returns, no code or data of the task is touched again, so the
    // module that owns it may be unloaded. When called from inside the task
    // itself it cannot wait for itself: it behaves like remove().
    bool remove_and_wait(TaskId id);

    // Changes the period of a task; the new grid starts from its last tick.
    bool set_period(TaskId id, std::chrono::nanoseconds period);

    std::optional<TaskStats> stats(TaskId id) const;

    // Stops the scheduler and all dedicated workers. Stats stay readable.
    // Called automatically by the destructor.
    void stop();

    // true if the high-resolution waitable timer is in use.
    bool uses_high_resolution_timer() const noexcept;

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

    TaskId add(std::chrono::nanoseconds period, Task task, TaskOptions options = {}) {
        const auto id = _caller->add(period, std::move(task), options);
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
