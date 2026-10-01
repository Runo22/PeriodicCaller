#include "periodic_caller/PeriodicCaller.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>

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
    explicit UniqueHandle(HANDLE handle) : h(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() {
        if (h) CloseHandle(h);
    }
};

}  // namespace

struct PeriodicCaller::Impl {
    struct Entry {
        TaskId id;
        nanoseconds period;
        Clock::time_point next;
        Task task;
        MissedTickPolicy policy;
        TaskStats stats;
        nanoseconds lateness_sum{0};
        bool removed = false;
        bool executing = false;
    };

    SchedulerOptions _options;
    mutable std::mutex _mutex;
    std::unordered_map<TaskId, std::shared_ptr<Entry>> _entries;
    TaskId _next_id = 1;

    UniqueHandle _timer;
    UniqueHandle _wake_event;  // Auto-reset; signalled whenever the schedule changes.
    bool _high_resolution = false;
    bool _time_period_raised = false;

    std::jthread _worker;

    explicit Impl(SchedulerOptions opts) : _options(opts) {
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

        _worker = std::jthread([this](std::stop_token st) { run(st); });
    }

    ~Impl() {
        stop();
        if (_time_period_raised) timeEndPeriod(1);
    }

    void wake() { SetEvent(_wake_event.h); }

    void stop() {
        if (!_worker.joinable()) return;
        _worker.request_stop();
        wake();
        // Joining from inside a task would deadlock; the loop exits on its own.
        if (_worker.get_id() != std::this_thread::get_id()) _worker.join();
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

    void run(std::stop_token st) {
        if (_options.high_priority_thread) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }

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

            const auto scheduled = due->next;
            if (Clock::now() < scheduled) {
                lock.unlock();
                wait_until(scheduled);
                lock.lock();
                continue;  // Re-evaluate: the schedule may have changed meanwhile.
            }

            due->executing = true;
            lock.unlock();
            const auto start = Clock::now();
            bool threw = false;
            try {
                due->task();
            } catch (...) {
                threw = true;
            }
            const auto end = Clock::now();
            lock.lock();
            due->executing = false;

            if (due->removed) continue;

            auto& s = due->stats;
            const auto lateness = std::chrono::duration_cast<nanoseconds>(start - scheduled);
            ++s.runs;
            if (threw) ++s.exceptions;
            s.last_lateness = lateness;
            s.max_lateness = std::max(s.max_lateness, lateness);
            due->lateness_sum += lateness;
            s.avg_lateness = due->lateness_sum / static_cast<long long>(s.runs);

            // Advance on the absolute grid; never relative to `end` (that drifts).
            due->next += due->period;
            if (due->next <= end && due->policy == MissedTickPolicy::Skip) {
                const auto behind = (end - due->next) / due->period + 1;
                due->next += behind * due->period;
                s.missed_ticks += static_cast<std::uint64_t>(behind);
            }
        }
    }
};

PeriodicCaller::PeriodicCaller(SchedulerOptions options)
    : _impl(std::make_unique<Impl>(options)) {}

PeriodicCaller::~PeriodicCaller() = default;

TaskId PeriodicCaller::add(std::chrono::nanoseconds period, Task task, TaskOptions options) {
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("PeriodicCaller::add: period must be positive");
    }
    if (!task) throw std::invalid_argument("PeriodicCaller::add: task is empty");

    auto entry = std::make_shared<Impl::Entry>();
    entry->period = period;
    entry->task = std::move(task);
    entry->policy = options.missed_tick_policy;
    entry->next = Clock::now() + (options.run_immediately ? std::chrono::nanoseconds::zero() : period);

    TaskId id;
    {
        std::scoped_lock lock(_impl->_mutex);
        id = _impl->_next_id++;
        entry->id = id;
        _impl->_entries.emplace(id, std::move(entry));
    }
    _impl->wake();
    return id;
}

bool PeriodicCaller::remove(TaskId id) {
    {
        std::scoped_lock lock(_impl->_mutex);
        const auto it = _impl->_entries.find(id);
        if (it == _impl->_entries.end()) return false;
        it->second->removed = true;
        _impl->_entries.erase(it);
    }
    _impl->wake();
    return true;
}

bool PeriodicCaller::set_period(TaskId id, std::chrono::nanoseconds period) {
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("PeriodicCaller::set_period: period must be positive");
    }
    {
        std::scoped_lock lock(_impl->_mutex);
        const auto it = _impl->_entries.find(id);
        if (it == _impl->_entries.end()) return false;
        auto& e = *it->second;
        // While executing, `next` is the current tick and gets advanced afterwards.
        if (!e.executing) e.next += period - e.period;
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

}  // namespace periodic_caller
