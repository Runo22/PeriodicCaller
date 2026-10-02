#include "periodic_caller/PeriodicCaller.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

// Lets the .cpp be dropped into a plain Visual Studio project without extra linker setup.
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace periodic_caller {

namespace {

using std::chrono::nanoseconds;

// Waitable timers use 100 ns units; negative values mean "relative".
LONGLONG to_relative_due_time(nanoseconds wait) {
    const auto ticks = (wait.count() + 99) / 100;  // Round up: never wake early.
    return -std::max<LONGLONG>(ticks, 1);
}

struct UniqueHandle {
    HANDLE h = nullptr;
    UniqueHandle() = default;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() {
        if (h) CloseHandle(h);
    }
};

// Resolved at runtime so the library builds with any SDK/MinGW and still runs
// on Windows versions that lack the newer APIs.
template <class Fn>
Fn load_function(const wchar_t* module, const char* name) {
    HMODULE m = GetModuleHandleW(module);
    if (!m) m = LoadLibraryExW(module, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? std::bit_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

// Applies the timing-related settings to the calling thread for its lifetime.
class ThreadTuning {
public:
    ThreadTuning(const SchedulerOptions& options, const wchar_t* name) {
        const HANDLE self = GetCurrentThread();

        static const auto set_description =
            load_function<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(L"kernel32.dll", "SetThreadDescription");
        if (set_description) set_description(self, name);

        // Opt out of EcoQoS: otherwise Windows 11 may move a background app's
        // threads to efficiency cores / lower clocks, which adds jitter.
        static const auto set_information = load_function<BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD)>(
            L"kernel32.dll", "SetThreadInformation");
        if (set_information) {
            struct {
                ULONG version, control_mask, state_mask;
            } state{1, 0x1 /* EXECUTION_SPEED */, 0 /* not throttled */};
            constexpr int thread_power_throttling = 3;
            set_information(self, thread_power_throttling, &state, static_cast<DWORD>(sizeof(state)));
        }

        if (options.high_priority_thread) SetThreadPriority(self, THREAD_PRIORITY_TIME_CRITICAL);

        if (options.use_mmcss) {
            static const auto av_set = load_function<HANDLE(WINAPI*)(LPCWSTR, LPDWORD)>(
                L"avrt.dll", "AvSetMmThreadCharacteristicsW");
            DWORD task_index = 0;
            if (av_set) _mmcss = av_set(L"Pro Audio", &task_index);
        }
    }

    ~ThreadTuning() {
        if (!_mmcss) return;
        static const auto av_revert =
            load_function<BOOL(WINAPI*)(HANDLE)>(L"avrt.dll", "AvRevertMmThreadCharacteristics");
        if (av_revert) av_revert(_mmcss);
    }

    ThreadTuning(const ThreadTuning&) = delete;
    ThreadTuning& operator=(const ThreadTuning&) = delete;

private:
    HANDLE _mmcss = nullptr;
};

struct RunResult {
    Clock::time_point start;
    Clock::time_point end;
    bool threw = false;
};

}  // namespace

struct PeriodicCaller::Impl {
    // All fields are guarded by Impl::_mutex, except `task` (only touched by the
    // thread that runs it) and `worker_exited`.
    struct Entry {
        TaskId id = 0;
        nanoseconds period{};
        Clock::time_point next;  // Next tick on the absolute grid.
        Task task;
        TaskOptions options;
        TaskStats stats;
        nanoseconds lateness_sum{0};
        bool removed = false;
        bool running = false;     // A call is in progress (either mode).
        std::thread::id runner;   // Thread making that call.

        // Dedicated mode only.
        bool executing = false;
        Clock::time_point dispatched_tick;
        std::optional<Clock::time_point> pending_tick;  // Coalesced run.
        UniqueHandle run_event;                         // Auto-reset.
        std::jthread worker;
        std::atomic<bool> worker_exited = false;
    };

    // A dedicated worker that was asked to stop but has not been joined yet.
    struct Retired {
        std::jthread thread;
        std::shared_ptr<Entry> entry;
    };

    SchedulerOptions _options;
    mutable std::mutex _mutex;
    std::unordered_map<TaskId, std::shared_ptr<Entry>> _entries;
    // Removed while running; the runner destroys the task when the call returns.
    std::unordered_map<TaskId, std::shared_ptr<Entry>> _draining;
    std::condition_variable _idle;  // Notified whenever a call finishes.
    std::vector<Retired> _retired;
    TaskId _next_id = 1;
    bool _stopped = false;

    UniqueHandle _timer;
    UniqueHandle _wake_event;  // Auto-reset; signalled whenever the schedule changes.
    bool _high_resolution = false;
    bool _time_period_raised = false;

    std::jthread _scheduler;

    explicit Impl(SchedulerOptions opts) : _options(std::move(opts)) {
        _timer.h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        _high_resolution = _timer.h != nullptr;
        if (!_high_resolution) {
            // Pre-1803 Windows: classic timer, made precise via a 1 ms system tick.
            _timer.h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            _time_period_raised = timeBeginPeriod(1) == TIMERR_NOERROR;
        }
        _wake_event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!_timer.h || !_wake_event.h) {
            if (_time_period_raised) timeEndPeriod(1);
            throw std::runtime_error("PeriodicCaller: failed to create timer/event handles");
        }

        _scheduler = std::jthread([this](std::stop_token st) { scheduler_loop(st); });
    }

    ~Impl() {
        stop();
        if (_time_period_raised) timeEndPeriod(1);
    }

    void wake() { SetEvent(_wake_event.h); }

    void stop() {
        {
            std::scoped_lock lock(_mutex);
            _stopped = true;
            for (auto& [id, e] : _entries) retire_locked(e);
        }
        _scheduler.request_stop();
        wake();
        // Joining from inside a task would deadlock; the loop exits on its own.
        if (_scheduler.joinable() && _scheduler.get_id() != std::this_thread::get_id()) {
            _scheduler.join();
        }
        reap(true);
    }

    TaskId add(nanoseconds period, Task task, TaskOptions options) {
        auto entry = std::make_shared<Entry>();
        entry->period = period;
        entry->task = std::move(task);
        entry->options = options;
        entry->next = Clock::now() + (options.run_immediately ? nanoseconds::zero() : period);

        const bool dedicated = options.execution_mode == ExecutionMode::Dedicated;
        if (dedicated) {
            entry->run_event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!entry->run_event.h) throw std::runtime_error("PeriodicCaller: failed to create event");
        }

        TaskId id;
        {
            std::scoped_lock lock(_mutex);
            if (_stopped) throw std::logic_error("PeriodicCaller::add: scheduler is stopped");
            id = _next_id++;
            entry->id = id;
            if (dedicated) {
                entry->worker = std::jthread(
                    [this, entry](std::stop_token st) { dedicated_loop(st, entry); });
            }
            _entries.emplace(id, std::move(entry));
        }
        wake();
        return id;
    }

    // Unregisters `id` from _entries (or finds it in _draining if a previous
    // remove is still finishing). If the task is idle its object is moved into
    // `dead`, to be destroyed by the caller outside the lock; if it is running,
    // the runner destroys it when the call returns (see finish_run_locked).
    std::shared_ptr<Entry> unregister_locked(TaskId id, Task& dead) {
        const auto it = _entries.find(id);
        if (it == _entries.end()) {
            const auto d = _draining.find(id);
            return d == _draining.end() ? nullptr : d->second;
        }
        auto e = it->second;
        _entries.erase(it);
        e->removed = true;
        retire_locked(e);
        if (e->running) {
            _draining.emplace(id, e);
        } else {
            dead = std::move(e->task);
        }
        return e;
    }

    bool remove(TaskId id) {
        Task dead;  // Declared first: destroyed last, outside the lock.
        {
            std::scoped_lock lock(_mutex);
            if (!_entries.contains(id)) return false;
            unregister_locked(id, dead);
        }
        wake();
        reap(false);
        return true;
    }

    bool remove_and_wait(TaskId id) {
        Task dead;
        bool found;
        {
            std::unique_lock lock(_mutex);
            found = _entries.contains(id);
            const auto e = unregister_locked(id, dead);
            if (!e) return false;
            // From inside the task itself we would wait for ourselves forever.
            if (!(e->running && e->runner == std::this_thread::get_id())) {
                _idle.wait(lock, [&] { return !e->running; });
            }
        }
        wake();
        reap(false);
        return found;
    }

    // Called by the thread that ran `e` once its call(s) returned, lock held.
    void finish_run_locked(std::unique_lock<std::mutex>& lock, Entry& e) {
        if (e.removed) {
            Task dead = std::move(e.task);
            lock.unlock();
            dead = nullptr;  // Destroy the lambda (and captures) outside the lock.
            lock.lock();
            _draining.erase(e.id);
        }
        e.running = false;
        _idle.notify_all();
    }

    // Asks a dedicated worker to exit; it is joined later by reap().
    void retire_locked(const std::shared_ptr<Entry>& e) {
        if (!e->worker.joinable()) return;
        e->worker.request_stop();
        SetEvent(e->run_event.h);
        _retired.push_back({std::move(e->worker), e});
    }

    // Joins retired workers: all of them, or only those that already exited.
    // Never joins the calling thread itself.
    void reap(bool all) {
        std::vector<Retired> done;
        {
            std::scoped_lock lock(_mutex);
            for (auto it = _retired.begin(); it != _retired.end();) {
                const bool is_self = it->thread.get_id() == std::this_thread::get_id();
                if (is_self || (!all && !it->entry->worker_exited)) {
                    ++it;
                    continue;
                }
                done.push_back(std::move(*it));
                it = _retired.erase(it);
            }
        }
        // `done` goes out of scope here: std::jthread joins, outside the lock.
    }

    // Sleeps until `deadline`. Returns false if woken early by _wake_event.
    bool wait_until(Clock::time_point deadline) {
        const auto wake_at = deadline - _options.spin_threshold;
        const auto now = Clock::now();
        if (wake_at > now) {
            LARGE_INTEGER due;
            due.QuadPart = to_relative_due_time(wake_at - now);
            SetWaitableTimer(_timer.h, &due, 0, nullptr, nullptr, FALSE);
            const HANDLE handles[2] = {_wake_event.h, _timer.h};
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) {
                CancelWaitableTimer(_timer.h);
                return false;
            }
        }
        if (_options.spin_threshold.count() > 0) {
            while (Clock::now() < deadline) YieldProcessor();
        }
        return true;
    }

    // Runs the task body; called without the lock held.
    RunResult invoke(Entry& e) {
        RunResult r;
        std::exception_ptr error;
        r.start = Clock::now();
        try {
            e.task();
        } catch (...) {
            error = std::current_exception();
        }
        r.end = Clock::now();
        if (error) {
            r.threw = true;
            if (_options.on_error) {
                try {
                    _options.on_error(e.id, error);
                } catch (...) {
                }
            }
        }
        return r;
    }

    static void record_locked(Entry& e, Clock::time_point tick, const RunResult& r) {
        auto& s = e.stats;
        const auto lateness = std::chrono::duration_cast<nanoseconds>(r.start - tick);
        const auto duration = std::chrono::duration_cast<nanoseconds>(r.end - r.start);
        ++s.runs;
        if (r.threw) ++s.exceptions;
        s.last_lateness = lateness;
        s.max_lateness = std::max(s.max_lateness, lateness);
        e.lateness_sum += lateness;
        s.avg_lateness = e.lateness_sum / static_cast<long long>(s.runs);
        s.last_duration = duration;
        s.max_duration = std::max(s.max_duration, duration);
    }

    static void skip_missed_locked(Entry& e, Clock::time_point now) {
        if (e.next > now || e.options.missed_tick_policy != MissedTickPolicy::Skip) return;
        const auto behind = (now - e.next) / e.period + 1;
        e.next += behind * e.period;
        e.stats.missed_ticks += static_cast<std::uint64_t>(behind);
    }

    void dispatch_dedicated_locked(Entry& e, Clock::time_point tick) {
        if (e.executing) {
            if (e.options.overlap_policy == OverlapPolicy::Coalesce && !e.pending_tick) {
                e.pending_tick = tick;
            } else {
                ++e.stats.overrun_skips;
            }
            return;
        }
        e.executing = true;
        e.dispatched_tick = tick;
        SetEvent(e.run_event.h);
    }

    void scheduler_loop(std::stop_token st) {
        ThreadTuning tuning(_options, L"PeriodicCaller scheduler");

        std::unique_lock lock(_mutex);
        while (!st.stop_requested()) {
            std::shared_ptr<Entry> due;
            for (auto& [id, e] : _entries) {
                if (!due || e->next < due->next) due = e;
            }

            if (!due) {
                lock.unlock();
                WaitForSingleObject(_wake_event.h, INFINITE);
                lock.lock();
                continue;
            }

            const auto tick = due->next;
            if (Clock::now() < tick) {
                lock.unlock();
                wait_until(tick);
                lock.lock();
                continue;  // Re-evaluate: the schedule may have changed meanwhile.
            }

            // Advance on the absolute grid; never relative to "now" (that drifts).
            due->next += due->period;

            if (due->options.execution_mode == ExecutionMode::Dedicated) {
                dispatch_dedicated_locked(*due, tick);
            } else {
                due->running = true;
                due->runner = std::this_thread::get_id();
                lock.unlock();
                const auto result = invoke(*due);
                lock.lock();
                const bool removed = due->removed;
                if (!removed) record_locked(*due, tick, result);
                finish_run_locked(lock, *due);
                if (removed) continue;
            }
            skip_missed_locked(*due, Clock::now());
        }
    }

    void dedicated_loop(std::stop_token st, std::shared_ptr<Entry> e) {
        {
            ThreadTuning tuning(_options, L"PeriodicCaller worker");
            while (WaitForSingleObject(e->run_event.h, INFINITE) == WAIT_OBJECT_0 &&
                   !st.stop_requested()) {
                std::unique_lock lock(_mutex);
                if (!e->executing) continue;
                if (e->removed) {  // Removed after dispatch, before it started.
                    e->executing = false;
                    continue;
                }
                e->running = true;
                e->runner = std::this_thread::get_id();
                auto tick = e->dispatched_tick;
                while (true) {
                    lock.unlock();
                    const auto result = invoke(*e);
                    lock.lock();
                    if (e->removed) break;
                    record_locked(*e, tick, result);
                    if (!e->pending_tick || st.stop_requested()) break;
                    tick = *std::exchange(e->pending_tick, std::nullopt);
                }
                e->pending_tick.reset();
                e->executing = false;
                finish_run_locked(lock, *e);
            }
        }
        e->worker_exited = true;
    }
};

PeriodicCaller::PeriodicCaller(SchedulerOptions options)
    : _impl(std::make_unique<Impl>(std::move(options))) {}

PeriodicCaller::~PeriodicCaller() = default;

TaskId PeriodicCaller::add(std::chrono::nanoseconds period, Task task, TaskOptions options) {
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("PeriodicCaller::add: period must be positive");
    }
    if (!task) throw std::invalid_argument("PeriodicCaller::add: task is empty");
    return _impl->add(period, std::move(task), options);
}

bool PeriodicCaller::remove(TaskId id) { return _impl->remove(id); }

bool PeriodicCaller::remove_and_wait(TaskId id) { return _impl->remove_and_wait(id); }

bool PeriodicCaller::set_period(TaskId id, std::chrono::nanoseconds period) {
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("PeriodicCaller::set_period: period must be positive");
    }
    {
        std::scoped_lock lock(_impl->_mutex);
        const auto it = _impl->_entries.find(id);
        if (it == _impl->_entries.end()) return false;
        auto& e = *it->second;
        // `next` is always one old period past the last tick; re-base it.
        e.next += period - e.period;
        e.period = period;
    }
    _impl->wake();
    return true;
}

std::optional<TaskStats> PeriodicCaller::stats(TaskId id) const {
    std::scoped_lock lock(_impl->_mutex);
    const auto it = _impl->_entries.find(id);
    if (it == _impl->_entries.end()) return std::nullopt;
    return it->second->stats;
}

void PeriodicCaller::stop() { _impl->stop(); }

bool PeriodicCaller::uses_high_resolution_timer() const noexcept { return _impl->_high_resolution; }

namespace {
// Defined in this translation unit, so there is exactly one per module that
// contains PeriodicCaller.cpp: one per process when it lives in a DLL.
std::mutex shared_instance_mutex;
PeriodicCaller* shared_instance = nullptr;  // Leaked on purpose; see header.
}  // namespace

PeriodicCaller& PeriodicCaller::shared(SchedulerOptions options) {
    std::scoped_lock lock(shared_instance_mutex);
    if (!shared_instance) shared_instance = new PeriodicCaller(std::move(options));
    return *shared_instance;
}

void PeriodicCaller::shutdown_shared() {
    PeriodicCaller* instance;
    {
        std::scoped_lock lock(shared_instance_mutex);
        instance = std::exchange(shared_instance, nullptr);
    }
    delete instance;
}

}  // namespace periodic_caller
